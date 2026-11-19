// CPU port of AMD FidelityFX Super Resolution 1 (EASU + RCAS). See fsr1.h for the license and notes.
#include "fsr1.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

namespace canon_eos_webcam {

namespace {

constexpr float kRcasLimit = 0.25f - 1.0f / 16.0f;  // FSR_RCAS_LIMIT
constexpr int kEasuHalo = 1;  // RCAS reads one output row above and below

inline float BitsToFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t FloatToBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// APrxLoRcpF1 / APrxLoRsqF1 / APrxMedRcpF1 of ffx_a.h
inline float PrxLoRcp(float a) { return BitsToFloat(0x7ef07ebbu - FloatToBits(a)); }
inline float PrxLoRsq(float a) { return BitsToFloat(0x5f347d74u - (FloatToBits(a) >> 1)); }
inline float PrxMedRcp(float a) {
    float b = BitsToFloat(0x7ef19fffu - FloatToBits(a));
    return b * (-b * a + 2.0f);
}
inline float Min(float a, float b) { return a < b ? a : b; }
inline float Max(float a, float b) { return a > b ? a : b; }
inline float Sat(float a) { return Min(Max(a, 0.0f), 1.0f); }
inline float Abs(float a) { return a < 0.0f ? -a : a; }

// One EASU "set": accumulates the direction and length of one of the four bilinear quadrants (FsrEasuSetF).
// lA..lE are the '+' shape: a above, b left, c centre, d right, e below (luma times 2).
inline void EasuSet(float& dirX, float& dirY, float& len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = PrxLoRcp(Max(Abs(dc), Abs(cb)));
    float dX = lD - lB;
    dirX += dX * w;
    lenX = Sat(Abs(dX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = PrxLoRcp(Max(Abs(ec), Abs(ca)));
    float dY = lE - lA;
    dirY += dY * w;
    lenY = Sat(Abs(dY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// One tap (FsrEasuTapF): ox/oy are the offset from the resolve position to the tap, in source pixels.
struct Tap {
    float dirX, dirY, len0, len1, lob, clp;
    float aR = 0, aG = 0, aB = 0, aW = 0;
    inline void Add(float ox, float oy, float cr, float cg, float cb) {
        float vx = ox * dirX + oy * dirY;
        float vy = ox * -dirY + oy * dirX;
        vx *= len0;
        vy *= len1;
        float d2 = Min(vx * vx + vy * vy, clp);
        float wB = (2.0f / 5.0f) * d2 - 1.0f;
        float wA = lob * d2 - 1.0f;
        wB *= wB;
        wA *= wA;
        wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
        float w = wB * wA;
        aR += cr * w;
        aG += cg * w;
        aB += cb * w;
        aW += w;
    }
};

// EASU over one output row. rows[k] (k = 0..3) point to the source rows fy-1, fy, fy+1, fy+2 of each plane;
// idx/pp are the per-column tables. Written as a plain branch-free loop so it auto-vectorizes (gathers on AVX2).
struct EasuRowArgs {
    const float* r[4];
    const float* g[4];
    const float* b[4];
    const int* idx;
    const float* ppx;
    float ppy;
    int n;
    float *outR, *outG, *outB;
};

#define EASU_BODY                                                                                            \
    for (int x = 0; x < a.n; x++) {                                                                          \
        const int i = a.idx[x];                                                                              \
        const float px = a.ppx[x], py = a.ppy;                                                               \
        float R[12], G[12], B[12], L[12];                                                                    \
        /* b c e f g h i j k l n o */                                                                        \
        static const int kRow[12] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3};                                    \
        static const int kCol[12] = {0, 1, -1, 0, 1, 2, -1, 0, 1, 2, 0, 1};                                  \
        for (int t = 0; t < 12; t++) {                                                                       \
            R[t] = a.r[kRow[t]][i + kCol[t]];                                                                \
            G[t] = a.g[kRow[t]][i + kCol[t]];                                                                \
            B[t] = a.b[kRow[t]][i + kCol[t]];                                                                \
            L[t] = B[t] * 0.5f + (R[t] * 0.5f + G[t]);                                                       \
        }                                                                                                    \
        enum { b, c, e, f, g, h, i_, j, k, l, n, o };                                                        \
        float dirX = 0, dirY = 0, len = 0;                                                                   \
        EasuSet(dirX, dirY, len, (1.0f - px) * (1.0f - py), L[b], L[e], L[f], L[g], L[j]);                   \
        EasuSet(dirX, dirY, len, px * (1.0f - py), L[c], L[f], L[g], L[h], L[k]);                            \
        EasuSet(dirX, dirY, len, (1.0f - px) * py, L[f], L[i_], L[j], L[k], L[n]);                           \
        EasuSet(dirX, dirY, len, px * py, L[g], L[j], L[k], L[l], L[o]);                                     \
        float dirR = dirX * dirX + dirY * dirY;                                                              \
        const bool zro = dirR < 1.0f / 32768.0f;                                                             \
        dirR = PrxLoRsq(dirR);                                                                               \
        dirR = zro ? 1.0f : dirR;                                                                            \
        dirX = zro ? 1.0f : dirX;                                                                            \
        dirX *= dirR;                                                                                        \
        dirY *= dirR;                                                                                        \
        len = len * 0.5f;                                                                                    \
        len *= len;                                                                                          \
        float stretch = (dirX * dirX + dirY * dirY) * PrxLoRcp(Max(Abs(dirX), Abs(dirY)));                  \
        Tap tp;                                                                                              \
        tp.dirX = dirX;                                                                                      \
        tp.dirY = dirY;                                                                                      \
        tp.len0 = 1.0f + (stretch - 1.0f) * len;                                                             \
        tp.len1 = 1.0f - 0.5f * len;                                                                         \
        tp.lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;                                                \
        tp.clp = PrxLoRcp(tp.lob);                                                                           \
        float mn[3], mx[3];                                                                                  \
        mn[0] = Min(Min(Min(R[f], R[g]), R[j]), R[k]);                                                       \
        mn[1] = Min(Min(Min(G[f], G[g]), G[j]), G[k]);                                                       \
        mn[2] = Min(Min(Min(B[f], B[g]), B[j]), B[k]);                                                       \
        mx[0] = Max(Max(Max(R[f], R[g]), R[j]), R[k]);                                                       \
        mx[1] = Max(Max(Max(G[f], G[g]), G[j]), G[k]);                                                       \
        mx[2] = Max(Max(Max(B[f], B[g]), B[j]), B[k]);                                                       \
        tp.Add(0.0f - px, -1.0f - py, R[b], G[b], B[b]);                                                     \
        tp.Add(1.0f - px, -1.0f - py, R[c], G[c], B[c]);                                                     \
        tp.Add(-1.0f - px, 1.0f - py, R[i_], G[i_], B[i_]);                                                  \
        tp.Add(0.0f - px, 1.0f - py, R[j], G[j], B[j]);                                                      \
        tp.Add(0.0f - px, 0.0f - py, R[f], G[f], B[f]);                                                      \
        tp.Add(-1.0f - px, 0.0f - py, R[e], G[e], B[e]);                                                     \
        tp.Add(1.0f - px, 1.0f - py, R[k], G[k], B[k]);                                                      \
        tp.Add(2.0f - px, 1.0f - py, R[l], G[l], B[l]);                                                      \
        tp.Add(2.0f - px, 0.0f - py, R[h], G[h], B[h]);                                                      \
        tp.Add(1.0f - px, 0.0f - py, R[g], G[g], B[g]);                                                      \
        tp.Add(1.0f - px, 2.0f - py, R[o], G[o], B[o]);                                                      \
        tp.Add(0.0f - px, 2.0f - py, R[n], G[n], B[n]);                                                      \
        const float rcpW = 1.0f / tp.aW;                                                                     \
        a.outR[x] = Min(mx[0], Max(mn[0], tp.aR * rcpW));                                                    \
        a.outG[x] = Min(mx[1], Max(mn[1], tp.aG * rcpW));                                                    \
        a.outB[x] = Min(mx[2], Max(mn[2], tp.aB * rcpW));                                                    \
    }

// RCAS over one row. up/mid/dn point to the EASU output rows (with 1 pixel of replicated edge on each side).
struct RcasRowArgs {
    const float* up[3];
    const float* mid[3];
    const float* dn[3];
    float sharp;
    int n;
    uint8_t* dst;  // interleaved RGB
};

// Reciprocals must be true divisions here; the clamp keeps 0 * inf from producing NaN on flat black / white.
#define RCAS_BODY                                                                                            \
    for (int x = 0; x < a.n; x++) {                                                                          \
        float bC[3], dC[3], eC[3], fC[3], hC[3];                                                             \
        for (int c = 0; c < 3; c++) {                                                                        \
            bC[c] = a.up[c][x + 1];                                                                          \
            dC[c] = a.mid[c][x];                                                                             \
            eC[c] = a.mid[c][x + 1];                                                                         \
            fC[c] = a.mid[c][x + 2];                                                                         \
            hC[c] = a.dn[c][x + 1];                                                                          \
        }                                                                                                    \
        float mn4[3], mx4[3], lobeC[3];                                                                      \
        for (int c = 0; c < 3; c++) {                                                                        \
            mn4[c] = Min(Min(Min(bC[c], dC[c]), fC[c]), hC[c]);                                              \
            mx4[c] = Max(Max(Max(bC[c], dC[c]), fC[c]), hC[c]);                                              \
            float hitMin = Min(mn4[c], eC[c]) * (1.0f / Max(4.0f * mx4[c], 1e-6f));                          \
            float hitMax = (1.0f - Max(mx4[c], eC[c])) * (1.0f / Min(4.0f * mn4[c] - 4.0f, -1e-6f));         \
            lobeC[c] = Max(-hitMin, hitMax);                                                                 \
        }                                                                                                    \
        float lobe = Max(-kRcasLimit, Min(Max(Max(lobeC[0], lobeC[1]), lobeC[2]), 0.0f)) * a.sharp;          \
        float rcpL = PrxMedRcp(4.0f * lobe + 1.0f);                                                          \
        for (int c = 0; c < 3; c++) {                                                                        \
            float p = (lobe * bC[c] + lobe * dC[c] + lobe * hC[c] + lobe * fC[c] + eC[c]) * rcpL;            \
            int q = int(Sat(p) * 255.0f + 0.5f);                                                             \
            a.dst[x * 3 + c] = uint8_t(q);                                                                   \
        }                                                                                                    \
    }

void EasuRowGeneric(const EasuRowArgs& a) { EASU_BODY }
void RcasRowGeneric(const RcasRowArgs& a) { RCAS_BODY }
#if !(defined(__x86_64__) && defined(__GNUC__))
void EasuRowAvx2(const EasuRowArgs& a) { EasuRowGeneric(a); }
void RcasRowAvx2(const RcasRowArgs& a) { RcasRowGeneric(a); }
#endif

#if defined(__x86_64__) && defined(__GNUC__)

// AVX2 + FMA versions: the same maths as the scalar code above, 8 output pixels per iteration, source taps fetched
// with gathers. (GCC doesn't vectorize the scalar loops: too much control flow and indirect loads.)
#define V_FN __attribute__((target("avx2,fma"), always_inline)) inline
using V = __m256;
V_FN V Vs(float x) { return _mm256_set1_ps(x); }
V_FN V Vmin(V a, V b) { return _mm256_min_ps(a, b); }
V_FN V Vmax(V a, V b) { return _mm256_max_ps(a, b); }
V_FN V Vabs(V a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
V_FN V Vsat(V a) { return Vmin(Vmax(a, _mm256_setzero_ps()), Vs(1.0f)); }
V_FN V Vfma(V a, V b, V c) { return _mm256_fmadd_ps(a, b, c); }
V_FN V VprxLoRcp(V a) {
    return _mm256_castsi256_ps(_mm256_sub_epi32(_mm256_set1_epi32(0x7ef07ebb), _mm256_castps_si256(a)));
}
V_FN V VprxLoRsq(V a) {
    return _mm256_castsi256_ps(
        _mm256_sub_epi32(_mm256_set1_epi32(0x5f347d74), _mm256_srli_epi32(_mm256_castps_si256(a), 1)));
}
V_FN V VprxMedRcp(V a) {
    V b = _mm256_castsi256_ps(_mm256_sub_epi32(_mm256_set1_epi32(0x7ef19fff), _mm256_castps_si256(a)));
    return _mm256_mul_ps(b, Vfma(_mm256_sub_ps(_mm256_setzero_ps(), b), a, Vs(2.0f)));
}

V_FN void VEasuSet(V& dirX, V& dirY, V& len, V w, V lA, V lB, V lC, V lD, V lE) {
    V dc = _mm256_sub_ps(lD, lC), cb = _mm256_sub_ps(lC, lB);
    V lenX = VprxLoRcp(Vmax(Vabs(dc), Vabs(cb)));
    V dX = _mm256_sub_ps(lD, lB);
    dirX = Vfma(dX, w, dirX);
    lenX = Vsat(_mm256_mul_ps(Vabs(dX), lenX));
    lenX = _mm256_mul_ps(lenX, lenX);
    len = Vfma(lenX, w, len);
    V ec = _mm256_sub_ps(lE, lC), ca = _mm256_sub_ps(lC, lA);
    V lenY = VprxLoRcp(Vmax(Vabs(ec), Vabs(ca)));
    V dY = _mm256_sub_ps(lE, lA);
    dirY = Vfma(dY, w, dirY);
    lenY = Vsat(_mm256_mul_ps(Vabs(dY), lenY));
    lenY = _mm256_mul_ps(lenY, lenY);
    len = Vfma(lenY, w, len);
}

struct VTap {
    V dirX, dirY, len0, len1, lob, clp;
    V aR, aG, aB, aW;
    V_FN void Add(V ox, V oy, V cr, V cg, V cb) {
        V vx = Vfma(ox, dirX, _mm256_mul_ps(oy, dirY));
        V vy = Vfma(ox, _mm256_sub_ps(_mm256_setzero_ps(), dirY), _mm256_mul_ps(oy, dirX));
        vx = _mm256_mul_ps(vx, len0);
        vy = _mm256_mul_ps(vy, len1);
        V d2 = Vmin(Vfma(vx, vx, _mm256_mul_ps(vy, vy)), clp);
        V wB = Vfma(Vs(2.0f / 5.0f), d2, Vs(-1.0f));
        V wA = Vfma(lob, d2, Vs(-1.0f));
        wB = _mm256_mul_ps(wB, wB);
        wA = _mm256_mul_ps(wA, wA);
        wB = Vfma(Vs(25.0f / 16.0f), wB, Vs(-(25.0f / 16.0f - 1.0f)));
        V w = _mm256_mul_ps(wB, wA);
        aR = Vfma(cr, w, aR);
        aG = Vfma(cg, w, aG);
        aB = Vfma(cb, w, aB);
        aW = _mm256_add_ps(aW, w);
    }
};

__attribute__((target("avx2,fma"))) void EasuRowAvx2(const EasuRowArgs& a) {
    const int n8 = a.n & ~7;
    const V py = Vs(a.ppy);
    for (int x = 0; x < n8; x += 8) {
        const __m256i vi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a.idx + x));
        const V px = _mm256_loadu_ps(a.ppx + x);
        V R[12], G[12], B[12], L[12];
        static const int kRow[12] = {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3};
        static const int kCol[12] = {0, 1, -1, 0, 1, 2, -1, 0, 1, 2, 0, 1};
        for (int t = 0; t < 12; t++) {
            R[t] = _mm256_i32gather_ps(a.r[kRow[t]] + kCol[t], vi, 4);
            G[t] = _mm256_i32gather_ps(a.g[kRow[t]] + kCol[t], vi, 4);
            B[t] = _mm256_i32gather_ps(a.b[kRow[t]] + kCol[t], vi, 4);
            L[t] = Vfma(B[t], Vs(0.5f), Vfma(R[t], Vs(0.5f), G[t]));
        }
        enum { b, c, e, f, g, h, i_, j, k, l, n, o };
        const V one = Vs(1.0f);
        const V ipx = _mm256_sub_ps(one, px), ipy = _mm256_sub_ps(one, py);
        V dirX = _mm256_setzero_ps(), dirY = _mm256_setzero_ps(), len = _mm256_setzero_ps();
        VEasuSet(dirX, dirY, len, _mm256_mul_ps(ipx, ipy), L[b], L[e], L[f], L[g], L[j]);
        VEasuSet(dirX, dirY, len, _mm256_mul_ps(px, ipy), L[c], L[f], L[g], L[h], L[k]);
        VEasuSet(dirX, dirY, len, _mm256_mul_ps(ipx, py), L[f], L[i_], L[j], L[k], L[n]);
        VEasuSet(dirX, dirY, len, _mm256_mul_ps(px, py), L[g], L[j], L[k], L[l], L[o]);
        V dirR = Vfma(dirX, dirX, _mm256_mul_ps(dirY, dirY));
        const V zro = _mm256_cmp_ps(dirR, Vs(1.0f / 32768.0f), _CMP_LT_OQ);
        dirR = VprxLoRsq(dirR);
        dirR = _mm256_blendv_ps(dirR, one, zro);
        dirX = _mm256_blendv_ps(dirX, one, zro);
        dirX = _mm256_mul_ps(dirX, dirR);
        dirY = _mm256_mul_ps(dirY, dirR);
        len = _mm256_mul_ps(len, Vs(0.5f));
        len = _mm256_mul_ps(len, len);
        V stretch = _mm256_mul_ps(Vfma(dirX, dirX, _mm256_mul_ps(dirY, dirY)),
                                  VprxLoRcp(Vmax(Vabs(dirX), Vabs(dirY))));
        VTap tp;
        tp.dirX = dirX;
        tp.dirY = dirY;
        tp.len0 = Vfma(_mm256_sub_ps(stretch, one), len, one);
        tp.len1 = Vfma(Vs(-0.5f), len, one);
        tp.lob = Vfma(Vs((1.0f / 4.0f - 0.04f) - 0.5f), len, Vs(0.5f));
        tp.clp = VprxLoRcp(tp.lob);
        tp.aR = tp.aG = tp.aB = tp.aW = _mm256_setzero_ps();
        V mn[3], mx[3];
        const V* ch[3] = {R, G, B};
        for (int q = 0; q < 3; q++) {
            mn[q] = Vmin(Vmin(Vmin(ch[q][f], ch[q][g]), ch[q][j]), ch[q][k]);
            mx[q] = Vmax(Vmax(Vmax(ch[q][f], ch[q][g]), ch[q][j]), ch[q][k]);
        }
        // offsets (ox - px, oy - py), taps in the order of the reference
        struct Off { int t; float ox, oy; };
        static const Off kTaps[12] = {{b, 0, -1}, {c, 1, -1}, {i_, -1, 1}, {j, 0, 1}, {f, 0, 0}, {e, -1, 0},
                                      {k, 1, 1},  {l, 2, 1},  {h, 2, 0},   {g, 1, 0}, {o, 1, 2}, {n, 0, 2}};
        for (int t = 0; t < 12; t++)
            tp.Add(_mm256_sub_ps(Vs(kTaps[t].ox), px), _mm256_sub_ps(Vs(kTaps[t].oy), py), R[kTaps[t].t],
                   G[kTaps[t].t], B[kTaps[t].t]);
        const V rcpW = _mm256_div_ps(one, tp.aW);
        _mm256_storeu_ps(a.outR + x, Vmin(mx[0], Vmax(mn[0], _mm256_mul_ps(tp.aR, rcpW))));
        _mm256_storeu_ps(a.outG + x, Vmin(mx[1], Vmax(mn[1], _mm256_mul_ps(tp.aG, rcpW))));
        _mm256_storeu_ps(a.outB + x, Vmin(mx[2], Vmax(mn[2], _mm256_mul_ps(tp.aB, rcpW))));
    }
    if (n8 < a.n) {  // tail
        EasuRowArgs t = a;
        t.idx += n8; t.ppx += n8; t.outR += n8; t.outG += n8; t.outB += n8; t.n = a.n - n8;
        EasuRowGeneric(t);
    }
}

__attribute__((target("avx2,fma"))) void RcasRowAvx2(const RcasRowArgs& a) {
    const int n8 = a.n & ~7;
    const V sharp = Vs(a.sharp), one = Vs(1.0f), zero = _mm256_setzero_ps();
    for (int x = 0; x < n8; x += 8) {
        V bC[3], dC[3], eC[3], fC[3], hC[3], lobe = Vs(-1e30f);
        for (int c = 0; c < 3; c++) {
            bC[c] = _mm256_loadu_ps(a.up[c] + x + 1);
            dC[c] = _mm256_loadu_ps(a.mid[c] + x);
            eC[c] = _mm256_loadu_ps(a.mid[c] + x + 1);
            fC[c] = _mm256_loadu_ps(a.mid[c] + x + 2);
            hC[c] = _mm256_loadu_ps(a.dn[c] + x + 1);
            V mn4 = Vmin(Vmin(Vmin(bC[c], dC[c]), fC[c]), hC[c]);
            V mx4 = Vmax(Vmax(Vmax(bC[c], dC[c]), fC[c]), hC[c]);
            V hitMin = _mm256_mul_ps(Vmin(mn4, eC[c]), _mm256_div_ps(one, Vmax(_mm256_mul_ps(Vs(4.0f), mx4), Vs(1e-6f))));
            V hitMax = _mm256_mul_ps(_mm256_sub_ps(one, Vmax(mx4, eC[c])),
                                     _mm256_div_ps(one, Vmin(Vfma(Vs(4.0f), mn4, Vs(-4.0f)), Vs(-1e-6f))));
            lobe = Vmax(lobe, Vmax(_mm256_sub_ps(zero, hitMin), hitMax));
        }
        lobe = _mm256_mul_ps(Vmax(Vs(-kRcasLimit), Vmin(lobe, zero)), sharp);
        const V rcpL = VprxMedRcp(Vfma(Vs(4.0f), lobe, one));
        alignas(32) int32_t q[3][8];
        for (int c = 0; c < 3; c++) {
            V sum = Vfma(lobe, _mm256_add_ps(_mm256_add_ps(bC[c], dC[c]), _mm256_add_ps(hC[c], fC[c])), eC[c]);
            V pix = Vsat(_mm256_mul_ps(sum, rcpL));
            _mm256_store_si256(reinterpret_cast<__m256i*>(q[c]), _mm256_cvttps_epi32(Vfma(pix, Vs(255.0f), Vs(0.5f))));
        }
        uint8_t* o = a.dst + size_t(x) * 3;
        for (int k = 0; k < 8; k++) {
            o[k * 3] = uint8_t(q[0][k]);
            o[k * 3 + 1] = uint8_t(q[1][k]);
            o[k * 3 + 2] = uint8_t(q[2][k]);
        }
    }
    if (n8 < a.n) {
        RcasRowArgs t = a;
        for (int c = 0; c < 3; c++) { t.up[c] += n8; t.mid[c] += n8; t.dn[c] += n8; }
        t.dst += size_t(n8) * 3;
        t.n = a.n - n8;
        RcasRowGeneric(t);
    }
}
bool UseAvx2() {
    static const bool v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") &&
                          !getenv("CANON_EOS_WEBCAM_FSR_GENERIC");  // test hook: force the scalar kernels
    return v;
}
#else
bool UseAvx2() { return false; }
#endif

}  // namespace

const char* Fsr1::KernelName() { return UseAvx2() ? "avx2" : "generic"; }

void Fsr1::Configure(int inW, int inH, int outW, int outH, float sharpnessStops, int workers) {
    sharp_ = std::exp2(-std::max(0.0f, sharpnessStops));  // FsrRcasCon
    if (inW != inW_ || inH != inH_ || outW != outW_ || outH != outH_) {
        inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
        planeStride_ = inW + kPadL + kPadR;
        // FsrEasuCon: output integer position -> position in the input viewport
        scaleX_ = float(inW) / float(outW);
        scaleY_ = float(inH) / float(outH);
        offX_ = 0.5f * scaleX_ - 0.5f;
        offY_ = 0.5f * scaleY_ - 0.5f;
        colIdx_.resize(size_t(outW));
        colPp_.resize(size_t(outW));
        for (int x = 0; x < outW; x++) {
            float pp = float(x) * scaleX_ + offX_;
            float fp = std::floor(pp);
            colIdx_[size_t(x)] = int(fp) + kPadL;
            colPp_[size_t(x)] = pp - fp;
        }
        rowF_.resize(size_t(outH));
        rowPp_.resize(size_t(outH));
        for (int y = 0; y < outH; y++) {
            float pp = float(y) * scaleY_ + offY_;
            float fp = std::floor(pp);
            rowF_[size_t(y)] = int(fp);
            rowPp_[size_t(y)] = pp - fp;
        }
        for (auto& p : plane_) p.assign(size_t(planeStride_) * size_t(inH), 0.0f);
        scratch_.clear();
    }
    if (int(scratch_.size()) != workers) scratch_.assign(size_t(workers), {});
}

void Fsr1::LoadRows(const uint8_t* rgb, int stride, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        const uint8_t* s = rgb + size_t(y) * size_t(stride);
        float* pr = plane_[0].data() + size_t(y) * size_t(planeStride_) + kPadL;
        float* pg = plane_[1].data() + size_t(y) * size_t(planeStride_) + kPadL;
        float* pb = plane_[2].data() + size_t(y) * size_t(planeStride_) + kPadL;
        for (int x = 0; x < inW_; x++) {
            pr[x] = float(s[x * 3]) * (1.0f / 255.0f);
            pg[x] = float(s[x * 3 + 1]) * (1.0f / 255.0f);
            pb[x] = float(s[x * 3 + 2]) * (1.0f / 255.0f);
        }
        for (int c = 0; c < 3; c++) {  // replicate the edges (clamp-to-edge sampler)
            float* p = plane_[c].data() + size_t(y) * size_t(planeStride_);
            for (int k = 0; k < kPadL; k++) p[k] = p[kPadL];
            for (int k = 0; k < kPadR; k++) p[kPadL + inW_ + k] = p[kPadL + inW_ - 1];
        }
    }
}

void Fsr1::RenderBand(int worker, int y0, int y1, uint8_t* dst, int stride) {
    const bool avx2 = UseAvx2();
    // EASU for rows [y0 - halo, y1 + halo) clamped to the image, stored with 1 pixel of edge on each side for RCAS
    const int e0 = std::max(0, y0 - kEasuHalo), e1 = std::min(outH_, y1 + kEasuHalo);
    const int rows = e1 - e0, ew = outW_ + 2;
    std::vector<float>& sc = scratch_[size_t(worker)];
    if (sc.size() < size_t(3) * size_t(rows) * size_t(ew)) sc.resize(size_t(3) * size_t(rows) * size_t(ew));
    auto row = [&](int c, int y) { return sc.data() + (size_t(c) * size_t(rows) + size_t(y - e0)) * size_t(ew); };
    for (int y = e0; y < e1; y++) {
        EasuRowArgs a;
        const int fy = rowF_[size_t(y)];
        for (int k = 0; k < 4; k++) {
            int sy = std::min(std::max(fy - 1 + k, 0), inH_ - 1);
            size_t off = size_t(sy) * size_t(planeStride_);
            a.r[k] = plane_[0].data() + off;
            a.g[k] = plane_[1].data() + off;
            a.b[k] = plane_[2].data() + off;
        }
        a.idx = colIdx_.data();
        a.ppx = colPp_.data();
        a.ppy = rowPp_[size_t(y)];
        a.n = outW_;
        a.outR = row(0, y) + 1;
        a.outG = row(1, y) + 1;
        a.outB = row(2, y) + 1;
        if (avx2) EasuRowAvx2(a); else EasuRowGeneric(a);
        for (int c = 0; c < 3; c++) {
            float* p = row(c, y);
            p[0] = p[1];
            p[ew - 1] = p[ew - 2];
        }
    }
    for (int y = y0; y < y1; y++) {
        RcasRowArgs a;
        const int yu = std::max(y - 1, 0), yd = std::min(y + 1, outH_ - 1);
        for (int c = 0; c < 3; c++) {
            a.up[c] = row(c, yu);
            a.mid[c] = row(c, y);
            a.dn[c] = row(c, yd);
        }
        a.sharp = sharp_;
        a.n = outW_;
        a.dst = dst + size_t(y - y0) * size_t(stride);
        if (avx2) RcasRowAvx2(a); else RcasRowGeneric(a);
    }
}

}  // namespace canon_eos_webcam
