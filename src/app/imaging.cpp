#include "imaging.h"

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eoscam {

// ---------------------------------------------------------------- ThreadPool

ThreadPool::ThreadPool(int threads) {
    for (int i = 1; i < threads; i++) workers_.emplace_back([this] {
        SetThreadDescription(GetCurrentThread(), L"pool");
        Work();
    });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::Work() {
    uint64_t seen = 0;
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
        if (stop_) return;
        seen = gen_;
        const std::function<void(int)>* fn = fn_;
        int n = count_;
        active_++;
        lk.unlock();
        for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) (*fn)(i);
        lk.lock();
        if (--active_ == 0) doneCv_.notify_all();
    }
}

void ThreadPool::ParallelFor(int n, const std::function<void(int)>& fn, bool parallel) {
    if (!parallel || workers_.empty() || n <= 1) {
        for (int i = 0; i < n; i++) fn(i);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn_ = &fn;
        count_ = n;
        next_ = 0;
        gen_++;
    }
    cv_.notify_all();
    for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) fn(i);
    std::unique_lock<std::mutex> lk(mu_);
    // espera os workers que entraram nesta geração terminarem; os que ainda não acordaram vão
    // encontrar next_ >= n e sair sem fazer nada
    doneCv_.wait(lk, [&] { return active_ == 0; });
    count_ = 0;
}

// ---------------------------------------------------------------- ajustes

void BuildAdjust(const ImageSettings& s, AdjustLut& lut) {
    lut.identity = s.IsIdentity();
    lut.sat = int(std::lround(s.saturation * 128.0 / 100.0));
    double tg = s.temperature / 100.0 * 0.25;  // quente: mais vermelho, menos azul
    double mg = s.tint / 100.0 * 0.25;         // magenta: menos verde
    double gainR = 1.0 + tg, gainB = 1.0 - tg, gainG = 1.0 - mg;
    double c = s.contrast * 2.55;
    double cf = (259.0 * (c + 255.0)) / (255.0 * (259.0 - c));
    double br = s.brightness / 100.0 * 0.4;
    double invGamma = 1.0 / std::max(0.1, double(s.gamma));
    auto build = [&](uint8_t* t, double gain) {
        for (int i = 0; i < 256; i++) {
            double x = std::min(1.0, i / 255.0 * gain);
            x = std::pow(x, invGamma);
            x = (x - 0.5) * cf + 0.5 + br;
            t[i] = uint8_t(std::lround(std::max(0.0, std::min(1.0, x)) * 255.0));
        }
    };
    build(lut.r, gainR);
    build(lut.g, gainG);
    build(lut.b, gainB);
}

namespace {

inline uint8_t Cl(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Tabela de posições/pesos para reamostrar uma linha de sw pixels em dw pixels (bilinear).
struct HTable {
    int srcW = 0, dstW = 0;
    std::vector<int> i0, i1;
    std::vector<uint16_t> w;  // peso de i1, 0..256
    void Build(int sw, int dw) {
        if (sw == srcW && dw == dstW) return;
        srcW = sw;
        dstW = dw;
        i0.resize(size_t(dw));
        i1.resize(size_t(dw));
        w.resize(size_t(dw));
        for (int x = 0; x < dw; x++) {
            double sx = (x + 0.5) * sw / dw - 0.5;
            int a = int(std::floor(sx));
            double f = sx - a;
            if (a < 0) { a = 0; f = 0; }
            if (a >= sw - 1) { a = sw - 1; f = 0; }
            i0[size_t(x)] = a;
            i1[size_t(x)] = std::min(a + 1, sw - 1);
            w[size_t(x)] = uint16_t(std::lround(f * 256));
        }
    }
};

struct VPos {
    int y0, y1, fy;
};
inline VPos VertPos(int y, int sh, int dh) {
    double sy = (y + 0.5) * sh / dh - 0.5;
    int a = int(std::floor(sy));
    double f = sy - a;
    if (a < 0) { a = 0; f = 0; }
    if (a >= sh - 1) { a = sh - 1; f = 0; }
    return {a, std::min(a + 1, sh - 1), int(std::lround(f * 256))};
}

bool HasAvx2();

inline void VBlend(const uint8_t* r0, const uint8_t* r1, int fy, uint8_t* dst, int n) {
    if (fy == 0) {
        std::memcpy(dst, r0, size_t(n));
        return;
    }
    int i = 0;
    if (HasAvx2()) {
        const __m256i f0 = _mm256_set1_epi16(short(256 - fy)), f1 = _mm256_set1_epi16(short(fy)),
                      c128 = _mm256_set1_epi16(128), zero = _mm256_setzero_si256();
        for (; i + 32 <= n; i += 32) {
            __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + i));
            __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + i));
            __m256i lo = _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(a, zero), f0),
                                                           _mm256_mullo_epi16(_mm256_unpacklo_epi8(b, zero), f1)),
                                          c128);
            __m256i hi = _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(a, zero), f0),
                                                           _mm256_mullo_epi16(_mm256_unpackhi_epi8(b, zero), f1)),
                                          c128);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                                _mm256_packus_epi16(_mm256_srli_epi16(lo, 8), _mm256_srli_epi16(hi, 8)));
        }
    }
    const uint16_t u1 = uint16_t(fy), u0 = uint16_t(256 - fy);
    for (; i < n; i++) dst[i] = uint8_t((uint16_t(r0[i]) * u0 + uint16_t(r1[i]) * u1 + 128) >> 8);
}

inline void HResample3(const uint8_t* v, const HTable& t, uint8_t* out) {
    const int n = t.dstW;
    const int* i0 = t.i0.data();
    const int* i1 = t.i1.data();
    const uint16_t* w = t.w.data();
    for (int x = 0; x < n; x++) {
        const uint8_t* a = v + i0[x] * 3;
        const uint8_t* b = v + i1[x] * 3;
        int w1 = w[x], w0 = 256 - w1;
        out[0] = uint8_t((a[0] * w0 + b[0] * w1 + 128) >> 8);
        out[1] = uint8_t((a[1] * w0 + b[1] * w1 + 128) >> 8);
        out[2] = uint8_t((a[2] * w0 + b[2] * w1 + 128) >> 8);
        out += 3;
    }
}

// A CPU tem AVX2 (e o sistema operacional o habilita)?
bool HasAvx2() {
    static const bool ok = [] {
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuidex(r, 1, 0);
        if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28))) return false;  // OSXSAVE e AVX
        if ((_xgetbv(0) & 6) != 6) return false;                       // estado YMM habilitado
        __cpuidex(r, 7, 0);
        return (r[1] & (1 << 5)) != 0;
    }();
    return ok;
}

// Reamostragem horizontal de um plano com 8 pixels por vez. Um gather de 32 bits em i0 já traz os dois
// vizinhos (i0 e i0+1; em i1 != i0+1, na borda direita, o peso é zero). v precisa ter 3 bytes de folga.
void HResample1Avx2(const uint8_t* v, const HTable& t, uint8_t* out) {
    const int n = t.dstW;
    const __m256i m255 = _mm256_set1_epi32(255), c256 = _mm256_set1_epi32(256), c128 = _mm256_set1_epi32(128);
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        __m256i idx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&t.i0[size_t(x)]));
        __m256i g = _mm256_i32gather_epi32(reinterpret_cast<const int*>(v), idx, 1);
        __m256i a = _mm256_and_si256(g, m255);
        __m256i b = _mm256_and_si256(_mm256_srli_epi32(g, 8), m255);
        __m256i w = _mm256_cvtepu16_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(&t.w[size_t(x)])));
        __m256i r = _mm256_add_epi32(
            _mm256_add_epi32(_mm256_mullo_epi32(a, _mm256_sub_epi32(c256, w)), _mm256_mullo_epi32(b, w)), c128);
        r = _mm256_srli_epi32(r, 8);
        __m128i p16 = _mm_packus_epi32(_mm256_castsi256_si128(r), _mm256_extracti128_si256(r, 1));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + x), _mm_packus_epi16(p16, p16));
    }
    for (; x < n; x++) {
        int w1 = t.w[size_t(x)], w0 = 256 - w1;
        out[x] = uint8_t((v[t.i0[size_t(x)]] * w0 + v[t.i1[size_t(x)]] * w1 + 128) >> 8);
    }
}

inline void HResample1(const uint8_t* v, const HTable& t, uint8_t* out) {
    if (HasAvx2()) {
        HResample1Avx2(v, t, out);
        return;
    }
    const int n = t.dstW;
    for (int x = 0; x < n; x++) {
        int w1 = t.w[size_t(x)], w0 = 256 - w1;
        out[x] = uint8_t((v[t.i0[size_t(x)]] * w0 + v[t.i1[size_t(x)]] * w1 + 128) >> 8);
    }
}

inline void AdjustPixel(const AdjustLut& a, int& b, int& g, int& r) {
    b = a.b[b];
    g = a.g[g];
    r = a.r[r];
    if (a.sat != 128) {
        int gray = (54 * r + 183 * g + 19 * b) >> 8;
        b = Cl(gray + (((b - gray) * a.sat) >> 7));
        g = Cl(gray + (((g - gray) * a.sat) >> 7));
        r = Cl(gray + (((r - gray) * a.sat) >> 7));
    }
}

inline uint8_t LumaOf(int r, int g, int b) { return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
inline uint8_t CbOf(int r, int g, int b) { return Cl(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
inline uint8_t CrOf(int r, int g, int b) { return Cl(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

// Linha de um plano (1 canal) reamostrada para dw pixels: mistura vertical na largura de origem e depois
// horizontal pela tabela.
inline void ScaleRow(const uint8_t* plane, int pitch, int srcH, int dstH, int y, const HTable& t, uint8_t* tmp,
                     uint8_t* out) {
    VPos vp = VertPos(y, srcH, dstH);
    VBlend(plane + size_t(vp.y0) * pitch, plane + size_t(vp.y1) * pitch, vp.fy, tmp, t.srcW);
    HResample1(tmp, t, out);
}

inline void BlendRow(uint8_t* f, const uint8_t* b, const uint8_t* a, int n) {
    int i = 0;
    if (HasAvx2()) {
        // mesma conta do laço escalar abaixo, sem desvios: t = f*a + b*(255-a) + 128; (t + (t>>8)) >> 8.
        // Com a = 0 ou 255 o resultado coincide com b ou f, então não precisa tratar os extremos à parte.
        // t <= 65153 e t + (t>>8) <= 65407 cabem em 16 bits sem sinal.
        const __m256i c255 = _mm256_set1_epi16(255), c128 = _mm256_set1_epi16(128), zero = _mm256_setzero_si256();
        for (; i + 32 <= n; i += 32) {
            __m256i F = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(f + i));
            __m256i B = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            __m256i A = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            auto half = [&](__m256i fv, __m256i bv, __m256i av) {
                __m256i t = _mm256_add_epi16(
                    _mm256_add_epi16(_mm256_mullo_epi16(fv, av), _mm256_mullo_epi16(bv, _mm256_sub_epi16(c255, av))),
                    c128);
                return _mm256_srli_epi16(_mm256_add_epi16(t, _mm256_srli_epi16(t, 8)), 8);
            };
            __m256i lo = half(_mm256_unpacklo_epi8(F, zero), _mm256_unpacklo_epi8(B, zero),
                              _mm256_unpacklo_epi8(A, zero));
            __m256i hi = half(_mm256_unpackhi_epi8(F, zero), _mm256_unpackhi_epi8(B, zero),
                              _mm256_unpackhi_epi8(A, zero));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(f + i), _mm256_packus_epi16(lo, hi));
        }
    }
    for (; i < n; i++) {
        int al = a[i];
        if (al == 255) continue;
        if (al == 0) {
            f[i] = b[i];
        } else {
            int t = f[i] * al + b[i] * (255 - al) + 128;
            f[i] = uint8_t((t + (t >> 8)) >> 8);
        }
    }
}

struct Tables {
    HTable fgY, fgC, maskY, maskC, plY, plC;
};

struct Scratch {
    std::vector<uint8_t> tmp, rowY, rowU, rowV, aY, aU, bgY, bgU, bgV;
    void Ensure(int vw) {
        auto fit = [](std::vector<uint8_t>& v, size_t n) { if (v.size() < n) v.resize(n); };
        fit(tmp, size_t(vw) + 16);  // folga para o gather do AVX2
        fit(rowY, kOutW);
        fit(rowU, kOutW / 2);
        fit(rowV, kOutW / 2);
        fit(aY, kOutW);
        fit(aU, kOutW / 2);
        fit(bgY, kOutW);
        fit(bgU, kOutW / 2);
        fit(bgV, kOutW / 2);
    }
};

struct FinalCtx {
    const FinalParams* p;
    const YuvImage* fg;
    const YuvImage* plate;  // modo Blur
    const Tables* tb;
    bool useBg;
    uint8_t solidY, solidU, solidV;
};

void RenderBand(const FinalCtx& c, int yStart, int yEnd) {
    static thread_local Scratch sc;
    const FinalParams& p = *c.p;
    const YuvImage& fg = *c.fg;
    int vw = std::max(std::max(fg.w, p.mw), c.plate ? c.plate->w : 0);
    sc.Ensure(vw);
    uint8_t* yPlane = p.out;
    uint8_t* uvPlane = p.out + size_t(kOutW) * kOutH;
    const int cw = kOutW / 2, ch = kOutH / 2;

    if (c.useBg && p.bg == BgKind::Solid) {
        std::fill(sc.bgY.begin(), sc.bgY.begin() + kOutW, c.solidY);
        std::fill(sc.bgU.begin(), sc.bgU.begin() + cw, c.solidU);
        std::fill(sc.bgV.begin(), sc.bgV.begin() + cw, c.solidV);
    }

    for (int y = yStart; y < yEnd; y++) {
        uint8_t* dstY = yPlane + size_t(y) * kOutW;
        ScaleRow(fg.y.data(), fg.w, fg.h, kOutH, y, c.tb->fgY, sc.tmp.data(), dstY);
        if (c.useBg) {
            ScaleRow(p.mask, p.mpitch, p.mh, kOutH, y, c.tb->maskY, sc.tmp.data(), sc.aY.data());
            const uint8_t* bgRow;
            if (p.bg == BgKind::Solid) {
                bgRow = sc.bgY.data();
            } else if (p.bg == BgKind::Image) {
                bgRow = p.bgImage->y.data() + size_t(y) * kOutW;
            } else {
                ScaleRow(c.plate->y.data(), c.plate->w, c.plate->h, kOutH, y, c.tb->plY, sc.tmp.data(),
                         sc.bgY.data());
                bgRow = sc.bgY.data();
            }
            BlendRow(dstY, bgRow, sc.aY.data(), kOutW);
        }
        if (y & 1) continue;

        // linha de crominância (metade da resolução nas duas direções)
        const int r = y / 2;
        uint8_t* dstUV = uvPlane + size_t(r) * kOutW;
        int fcw = fg.w / 2, fch = fg.h / 2;
        const uint8_t* fu = fg.u.data();
        const uint8_t* fv = fg.v.data();
        ScaleRow(fu, fcw, fch, ch, r, c.tb->fgC, sc.tmp.data(), sc.rowU.data());
        ScaleRow(fv, fcw, fch, ch, r, c.tb->fgC, sc.tmp.data(), sc.rowV.data());
        if (c.useBg) {
            ScaleRow(p.mask, p.mpitch, p.mh, ch, r, c.tb->maskC, sc.tmp.data(), sc.aU.data());
            const uint8_t *bu, *bv;
            if (p.bg == BgKind::Solid) {
                bu = sc.bgU.data();
                bv = sc.bgV.data();
            } else if (p.bg == BgKind::Image) {
                bu = p.bgImage->u.data() + size_t(r) * cw;
                bv = p.bgImage->v.data() + size_t(r) * cw;
            } else {
                int pcw = c.plate->w / 2, pch = c.plate->h / 2;
                ScaleRow(c.plate->u.data(), pcw, pch, ch, r, c.tb->plC, sc.tmp.data(), sc.bgU.data());
                ScaleRow(c.plate->v.data(), pcw, pch, ch, r, c.tb->plC, sc.tmp.data(), sc.bgV.data());
                bu = sc.bgU.data();
                bv = sc.bgV.data();
            }
            BlendRow(sc.rowU.data(), bu, sc.aU.data(), cw);
            BlendRow(sc.rowV.data(), bv, sc.aU.data(), cw);
        }
        const uint8_t* ru = sc.rowU.data();
        const uint8_t* rv = sc.rowV.data();
        for (int x = 0; x < cw; x++) {
            dstUV[2 * x] = ru[x];
            dstUV[2 * x + 1] = rv[x];
        }
    }
}

}  // namespace

// Versão AVX2 do núcleo de BgrToYuv sem ajustes: processa 16 pixels (de duas linhas) por iteração e devolve
// quantos pixels consumiu (múltiplo de 16); o resto fica para o laço escalar. Produz exatamente os mesmos
// bytes do laço escalar (mesma aritmética inteira, mesmos arredondamentos).
static int BgrPairToYuvAvx2(const uint8_t* a, const uint8_t* b, uint8_t* y0, uint8_t* y1, uint8_t* du, uint8_t* dv, int w) {
    // máscaras de pshufb para separar os canais de 16 pixels BGR (48 bytes = 3 blocos de 16)
    struct Masks {
        __m128i m[3][3];  // [canal][bloco]
        Masks() {
            for (int c = 0; c < 3; c++) {
                alignas(16) uint8_t t[3][16];
                std::memset(t, 0x80, sizeof t);
                for (int i = 0; i < 16; i++) {
                    int idx = 3 * i + c;
                    t[idx / 16][i] = uint8_t(idx % 16);
                }
                for (int k = 0; k < 3; k++) m[c][k] = _mm_load_si128(reinterpret_cast<const __m128i*>(t[k]));
            }
        }
    };
    static const Masks mk;
    auto split = [&](const uint8_t* row, __m128i& B, __m128i& G, __m128i& R) {
        __m128i p0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row));
        __m128i p1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + 16));
        __m128i p2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + 32));
        B = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(p0, mk.m[0][0]), _mm_shuffle_epi8(p1, mk.m[0][1])),
                         _mm_shuffle_epi8(p2, mk.m[0][2]));
        G = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(p0, mk.m[1][0]), _mm_shuffle_epi8(p1, mk.m[1][1])),
                         _mm_shuffle_epi8(p2, mk.m[1][2]));
        R = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(p0, mk.m[2][0]), _mm_shuffle_epi8(p1, mk.m[2][1])),
                         _mm_shuffle_epi8(p2, mk.m[2][2]));
    };
    const __m256i k66 = _mm256_set1_epi16(66), k129 = _mm256_set1_epi16(129), k25 = _mm256_set1_epi16(25),
                  c128 = _mm256_set1_epi16(128), c16 = _mm256_set1_epi16(16), ones = _mm256_set1_epi16(1);
    const __m256i c128d = _mm256_set1_epi32(128), z = _mm256_setzero_si256(), m255 = _mm256_set1_epi32(255);
    auto luma = [&](__m256i r, __m256i g, __m256i bl) {
        __m256i t = _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(r, k66), _mm256_mullo_epi16(g, k129)),
                                     _mm256_add_epi16(_mm256_mullo_epi16(bl, k25), c128));
        return _mm256_add_epi16(_mm256_srli_epi16(t, 8), c16);  // t < 65536: deslocamento lógico
    };
    auto store16 = [](uint8_t* dst, __m256i v) {  // 16 x u16 (0..255) -> 16 bytes
        __m256i p = _mm256_packus_epi16(v, v);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(p));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 8), _mm256_extracti128_si256(p, 1));
    };
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        __m128i B0, G0, R0, B1, G1, R1;
        split(a + size_t(x) * 3, B0, G0, R0);
        split(b + size_t(x) * 3, B1, G1, R1);
        __m256i b0 = _mm256_cvtepu8_epi16(B0), g0 = _mm256_cvtepu8_epi16(G0), r0 = _mm256_cvtepu8_epi16(R0);
        __m256i b1 = _mm256_cvtepu8_epi16(B1), g1 = _mm256_cvtepu8_epi16(G1), r1 = _mm256_cvtepu8_epi16(R1);
        store16(y0 + x, luma(r0, g0, b0));
        store16(y1 + x, luma(r1, g1, b1));
        // soma 2x2: linhas somadas e pares horizontais somados (madd) -> 8 valores de 32 bits
        __m256i sb = _mm256_madd_epi16(_mm256_add_epi16(b0, b1), ones);
        __m256i sg = _mm256_madd_epi16(_mm256_add_epi16(g0, g1), ones);
        __m256i sr = _mm256_madd_epi16(_mm256_add_epi16(r0, r1), ones);
        sb = _mm256_srli_epi32(sb, 2);
        sg = _mm256_srli_epi32(sg, 2);
        sr = _mm256_srli_epi32(sr, 2);
        __m256i cb = _mm256_add_epi32(
            _mm256_srai_epi32(
                _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(sr, _mm256_set1_epi32(-38)),
                                                  _mm256_mullo_epi32(sg, _mm256_set1_epi32(-74))),
                                 _mm256_add_epi32(_mm256_mullo_epi32(sb, _mm256_set1_epi32(112)), c128d)),
                8),
            c128d);
        __m256i cr = _mm256_add_epi32(
            _mm256_srai_epi32(
                _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(sr, _mm256_set1_epi32(112)),
                                                  _mm256_mullo_epi32(sg, _mm256_set1_epi32(-94))),
                                 _mm256_add_epi32(_mm256_mullo_epi32(sb, _mm256_set1_epi32(-18)), c128d)),
                8),
            c128d);
        cb = _mm256_min_epi32(_mm256_max_epi32(cb, z), m255);
        cr = _mm256_min_epi32(_mm256_max_epi32(cr, z), m255);
        auto pack8 = [](__m256i v, uint8_t* dst) {  // 8 x s32 (0..255) -> 8 bytes
            __m128i p = _mm_packus_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), _mm_packus_epi16(p, p));
        };
        pack8(cb, du + x / 2);
        pack8(cr, dv + x / 2);
    }
    return x;
}

// BGR u8 -> planos Y (largura x altura) e U, V (metade da resolução, média 2x2), BT.601 de faixa
// limitada. w e h precisam ser pares. Aplica os ajustes (LUT + saturação) na passada.
void BgrToYuv(const uint8_t* bgr, int w, int h, int pitch, const AdjustLut* adj, YuvImage& out, ThreadPool& pool) {
    out.w = w;
    out.h = h;
    out.y.resize(size_t(w) * h);
    out.u.resize(size_t(w / 2) * (h / 2));
    out.v.resize(size_t(w / 2) * (h / 2));
    const bool useAdj = adj && !adj->identity;
    const int pairs = h / 2;
    const int perBand = 16;
    const int n = (pairs + perBand - 1) / perBand;
    // imagens pequenas sem ajustes (o caminho padrão) ficam numa thread só: é o que menos gasta CPU
    const bool par = useAdj || size_t(w) * h > 1'000'000;
    pool.ParallelFor(n, [&](int band) {
        int p0 = band * perBand, p1 = std::min(pairs, p0 + perBand);
        for (int pr = p0; pr < p1; pr++) {
            const uint8_t* a = bgr + size_t(2 * pr) * pitch;
            const uint8_t* b = a + pitch;
            uint8_t* y0 = out.y.data() + size_t(2 * pr) * w;
            uint8_t* y1 = y0 + w;
            uint8_t* du = out.u.data() + size_t(pr) * (w / 2);
            uint8_t* dv = out.v.data() + size_t(pr) * (w / 2);
            int xStart = 0;
            if (!useAdj && HasAvx2()) xStart = BgrPairToYuvAvx2(a, b, y0, y1, du, dv, w);
            for (int x = xStart; x < w; x += 2) {
                int b00 = a[x * 3], g00 = a[x * 3 + 1], r00 = a[x * 3 + 2];
                int b01 = a[x * 3 + 3], g01 = a[x * 3 + 4], r01 = a[x * 3 + 5];
                int b10 = b[x * 3], g10 = b[x * 3 + 1], r10 = b[x * 3 + 2];
                int b11 = b[x * 3 + 3], g11 = b[x * 3 + 4], r11 = b[x * 3 + 5];
                if (useAdj) {
                    AdjustPixel(*adj, b00, g00, r00);
                    AdjustPixel(*adj, b01, g01, r01);
                    AdjustPixel(*adj, b10, g10, r10);
                    AdjustPixel(*adj, b11, g11, r11);
                }
                y0[x] = LumaOf(r00, g00, b00);
                y0[x + 1] = LumaOf(r01, g01, b01);
                y1[x] = LumaOf(r10, g10, b10);
                y1[x + 1] = LumaOf(r11, g11, b11);
                int sb = (b00 + b01 + b10 + b11) >> 2, sg = (g00 + g01 + g10 + g11) >> 2,
                    sr = (r00 + r01 + r10 + r11) >> 2;
                du[x / 2] = CbOf(sr, sg, sb);
                dv[x / 2] = CrOf(sr, sg, sb);
            }
        }
    }, par);
}

void RenderFinal(const FinalParams& p, ThreadPool& pool) {
    static Tables tb;
    static YuvImage fg, plate;
    BgrToYuv(p.src, p.sw, p.sh, p.spitch, p.adj, fg, pool);
    tb.fgY.Build(p.sw, kOutW);
    tb.fgC.Build(p.sw / 2, kOutW / 2);

    FinalCtx c{};
    c.p = &p;
    c.fg = &fg;
    c.plate = nullptr;
    c.tb = &tb;
    c.useBg = p.bg != BgKind::None && p.mask;
    if (c.useBg) {
        tb.maskY.Build(p.mw, kOutW);
        tb.maskC.Build(p.mw, kOutW / 2);
        if (p.bg == BgKind::Solid) {
            c.solidY = LumaOf(p.solidR, p.solidG, p.solidB);
            c.solidU = CbOf(p.solidR, p.solidG, p.solidB);
            c.solidV = CrOf(p.solidR, p.solidG, p.solidB);
        } else if (p.bg == BgKind::Blur) {
            // o fundo desfocado é a própria imagem e recebe os mesmos ajustes
            BgrToYuv(p.plate, p.pw & ~1, p.ph & ~1, p.pw * 3, p.adj, plate, pool);
            tb.plY.Build(plate.w, kOutW);
            tb.plC.Build(plate.w / 2, kOutW / 2);
            c.plate = &plate;
        }
    }
    // faixas de linhas pares; mais faixas que threads para equilibrar a carga
    const int bands = 12;
    int rowsPerBand = (kOutH / bands + 1) & ~1;
    int n = (kOutH + rowsPerBand - 1) / rowsPerBand;
    pool.ParallelFor(n, [&](int i) {
        int ys = i * rowsPerBand;
        int ye = std::min(kOutH, ys + rowsPerBand);
        if (ys < ye) RenderBand(c, ys, ye);
    }, p.sw * p.sh > 1'000'000 || c.useBg || (p.adj && !p.adj->identity));
}

// ---------------------------------------------------------------- máscara, desfoque, fundo

void FeatherMask(uint8_t* m, int w, int h, int pitch, std::vector<uint8_t>& tmp) {
    tmp.resize(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        const uint8_t* s = m + size_t(y) * pitch;
        uint8_t* d = tmp.data() + size_t(y) * w;
        d[0] = s[0];
        for (int x = 1; x < w - 1; x++) d[x] = uint8_t((s[x - 1] + 2 * s[x] + s[x + 1] + 2) >> 2);
        d[w - 1] = s[w - 1];
    }
    for (int y = 0; y < h; y++) {
        const uint8_t* a = tmp.data() + size_t(std::max(0, y - 1)) * w;
        const uint8_t* b = tmp.data() + size_t(y) * w;
        const uint8_t* c = tmp.data() + size_t(std::min(h - 1, y + 1)) * w;
        uint8_t* d = m + size_t(y) * pitch;
        for (int x = 0; x < w; x++) d[x] = uint8_t((a[x] + 2 * b[x] + c[x] + 2) >> 2);
    }
}

void DownscaleBgr(const uint8_t* src, int sw, int sh, int spitch, int f, std::vector<uint8_t>& dst, int& dw,
                  int& dh) {
    dw = std::max(1, sw / f);
    dh = std::max(1, sh / f);
    dst.resize(size_t(dw) * dh * 3);
    const int area = f * f;
    for (int y = 0; y < dh; y++) {
        uint8_t* d = dst.data() + size_t(y) * dw * 3;
        for (int x = 0; x < dw; x++) {
            int sb = 0, sg = 0, sr = 0;
            for (int j = 0; j < f; j++) {
                const uint8_t* s = src + size_t(y * f + j) * spitch + size_t(x) * f * 3;
                for (int i = 0; i < f; i++, s += 3) {
                    sb += s[0]; sg += s[1]; sr += s[2];
                }
            }
            d[x * 3] = uint8_t((sb + area / 2) / area);
            d[x * 3 + 1] = uint8_t((sg + area / 2) / area);
            d[x * 3 + 2] = uint8_t((sr + area / 2) / area);
        }
    }
}

// Passada de caixa com soma deslizante em uma linha/coluna de n elementos (3 canais), bordas replicadas.
static void BoxLine(const uint8_t* in, uint8_t* out, int n, int stride, int radius) {
    int win = 2 * radius + 1;
    for (int c = 0; c < 3; c++) {
        int sum = 0;
        for (int k = -radius; k <= radius; k++) sum += in[size_t(std::max(0, std::min(n - 1, k))) * stride + c];
        for (int i = 0; i < n; i++) {
            out[size_t(i) * stride + c] = uint8_t((sum + win / 2) / win);
            int add = std::min(n - 1, i + radius + 1), sub = std::max(0, i - radius);
            sum += in[size_t(add) * stride + c] - in[size_t(sub) * stride + c];
        }
    }
}

void BoxBlurBgr(uint8_t* img, int w, int h, int radius, std::vector<uint8_t>& tmp) {
    if (radius < 1) return;
    tmp.resize(size_t(w) * h * 3);
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) BoxLine(img + size_t(y) * w * 3, tmp.data() + size_t(y) * w * 3, w, 3, radius);
        for (int x = 0; x < w; x++) BoxLine(tmp.data() + size_t(x) * 3, img + size_t(x) * 3, h, w * 3, radius);
    }
}

// Soma deslizante em float (4 canais intercalados), bordas replicadas.
static void BoxLineF(const float* in, float* out, int n, int stride, int radius) {
    const float inv = 1.0f / float(2 * radius + 1);
    for (int c = 0; c < 4; c++) {
        float sum = 0;
        for (int k = -radius; k <= radius; k++) sum += in[size_t(std::max(0, std::min(n - 1, k))) * stride + c];
        for (int i = 0; i < n; i++) {
            out[size_t(i) * stride + c] = sum * inv;
            int add = std::min(n - 1, i + radius + 1), sub = std::max(0, i - radius);
            sum += in[size_t(add) * stride + c] - in[size_t(sub) * stride + c];
        }
    }
}

void MaskedBoxBlurBgr(uint8_t* img, const uint8_t* mask, int w, int h, int radius) {
    if (radius < 1) return;
    const size_t n = size_t(w) * h;
    // 4 canais: B*peso, G*peso, R*peso e o peso (peso = fundo = 1 - máscara)
    std::vector<float> a(n * 4), b(n * 4);
    for (size_t i = 0; i < n; i++) {
        float wgt = (255 - mask[i]) / 255.0f;
        a[i * 4] = img[i * 3] * wgt;
        a[i * 4 + 1] = img[i * 3 + 1] * wgt;
        a[i * 4 + 2] = img[i * 3 + 2] * wgt;
        a[i * 4 + 3] = wgt;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) BoxLineF(a.data() + size_t(y) * w * 4, b.data() + size_t(y) * w * 4, w, 4, radius);
        for (int x = 0; x < w; x++) BoxLineF(b.data() + size_t(x) * 4, a.data() + size_t(x) * 4, h, w * 4, radius);
    }
    // onde quase não há fundo por perto (centro da pessoa), usa o desfoque comum como reserva
    std::vector<uint8_t> plain(img, img + n * 3), tmp;
    BoxBlurBgr(plain.data(), w, h, radius, tmp);
    for (size_t i = 0; i < n; i++) {
        float wgt = a[i * 4 + 3];
        for (int c = 0; c < 3; c++) {
            float v = wgt > 0.02f ? a[i * 4 + size_t(c)] / wgt : float(plain[i * 3 + size_t(c)]);
            if (wgt <= 0.15f && wgt > 0.02f) {  // transição suave para a reserva
                float t = (wgt - 0.02f) / 0.13f;
                v = v * t + float(plain[i * 3 + size_t(c)]) * (1 - t);
            }
            img[i * 3 + size_t(c)] = uint8_t(std::max(0.0f, std::min(255.0f, v + 0.5f)));
        }
    }
}

void CoverBgr(const uint8_t* src, int sw, int sh, int spitch, int dw, int dh, std::vector<uint8_t>& dst) {
    dst.assign(size_t(dw) * dh * 3, 0);
    if (sw <= 0 || sh <= 0) return;
    // recorte central na proporção do destino
    double target = double(dw) / dh;
    int cw = sw, ch = sh;
    if (double(sw) / sh > target) cw = std::max(1, int(std::lround(sh * target)));
    else ch = std::max(1, int(std::lround(sw / target)));
    int cx = (sw - cw) / 2, cy = (sh - ch) / 2;
    const uint8_t* base = src + size_t(cy) * spitch + size_t(cx) * 3;

    // redução por média em blocos enquanto der, para não serrilhar imagens muito maiores que 1080p
    std::vector<uint8_t> reduced;
    int f = std::min(cw / dw, ch / dh);
    int pitch = spitch;
    if (f >= 2) {
        int nw = 0, nh = 0;
        DownscaleBgr(base, cw, ch, spitch, f, reduced, nw, nh);
        base = reduced.data();
        pitch = nw * 3;
        cw = nw;
        ch = nh;
    }
    HTable t;
    t.Build(cw, dw);
    std::vector<uint8_t> v(size_t(cw) * 3);
    for (int y = 0; y < dh; y++) {
        VPos vp = VertPos(y, ch, dh);
        VBlend(base + size_t(vp.y0) * pitch, base + size_t(vp.y1) * pitch, vp.fy, v.data(), cw * 3);
        HResample3(v.data(), t, dst.data() + size_t(y) * dw * 3);
    }
}

}  // namespace eoscam
