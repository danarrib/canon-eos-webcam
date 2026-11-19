#include "pipeline.h"

#include "fsr1.h"
#include "log.h"

#include <turbojpeg.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace canon_eos_webcam {

namespace {

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

inline uint8_t Cl(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// BT.601 limited range, same coefficients as the Windows version.
inline uint8_t LumaOf(int r, int g, int b) { return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
inline uint8_t CbOf(int r, int g, int b) { return Cl(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
inline uint8_t CrOf(int r, int g, int b) { return Cl(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

// Small persistent thread pool: Run(n, fn) calls fn(worker, i) for i in [0, n) across the workers (the calling
// thread is worker 0) and returns when all are done. Nothing is created per frame; idle workers sleep.
class Pool {
public:
    explicit Pool(int threads) : n_(std::max(1, threads)) {
        for (int i = 1; i < n_; i++) th_.emplace_back([this, i] { Loop(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        for (auto& t : th_) t.join();
    }
    int Size() const { return n_; }
    void Run(int count, const std::function<void(int, int)>& fn) {
        if (count <= 0) return;
        if (n_ == 1 || count == 1) {
            for (int i = 0; i < count; i++) fn(0, i);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            fn_ = &fn;
            count_ = count;
            next_.store(0);
            active_ = n_ - 1;
            gen_++;
        }
        cv_.notify_all();
        Work(0);
        std::unique_lock<std::mutex> lk(mu_);
        done_.wait(lk, [&] { return active_ == 0; });
        fn_ = nullptr;
    }

private:
    void Work(int worker) {
        for (;;) {
            int i = next_.fetch_add(1);
            if (i >= count_) break;
            (*fn_)(worker, i);
        }
    }
    void Loop(int worker) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
                if (quit_) return;
                seen = gen_;
            }
            Work(worker);
            std::lock_guard<std::mutex> lk(mu_);
            if (--active_ == 0) done_.notify_one();
        }
    }
    int n_;
    std::vector<std::thread> th_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    const std::function<void(int, int)>* fn_ = nullptr;
    int count_ = 0, active_ = 0;
    uint64_t gen_ = 0;
    bool quit_ = false;
    std::atomic<int> next_{0};
};

int PoolThreads() {
    if (const char* e = getenv("CANON_EOS_WEBCAM_THREADS")) {
        int v = atoi(e);
        if (v >= 1 && v <= 64) return v;
    }
    int hw = int(std::thread::hardware_concurrency());
    return std::max(1, std::min(hw > 0 ? hw : 4, 16));
}

constexpr int kBandRows = 30;  // output rows per band (even: the chroma is 2x2)

struct AdjustLut {
    uint8_t r[256], g[256], b[256];
    int sat = 128;
    bool identity = true;
};

void BuildAdjust(const ImageSettings& s, AdjustLut& lut) {
    lut.identity = s.IsIdentity();
    lut.sat = int(std::lround(s.saturation * 128.0 / 100.0));
    double tg = s.temperature / 100.0 * 0.25;  // warm: more red, less blue
    double mg = s.tint / 100.0 * 0.25;         // magenta: less green
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

void ApplyAdjustRows(const AdjustLut& a, const RgbView& v, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        uint8_t* p = v.data + size_t(y) * size_t(v.stride);
        for (int x = 0; x < v.width; x++, p += 3) {
            int r = a.r[p[0]], g = a.g[p[1]], b = a.b[p[2]];
            if (a.sat != 128) {
                int gray = (54 * r + 183 * g + 19 * b) >> 8;
                r = Cl(gray + (((r - gray) * a.sat) >> 7));
                g = Cl(gray + (((g - gray) * a.sat) >> 7));
                b = Cl(gray + (((b - gray) * a.sat) >> 7));
            }
            p[0] = uint8_t(r);
            p[1] = uint8_t(g);
            p[2] = uint8_t(b);
        }
    }
}

// Positions and weights to resample a row of sw pixels into dw pixels (bilinear).
struct HTable {
    int srcW = 0, dstW = 0;
    std::vector<int> i0, i1;
    std::vector<int> w;  // weight of i1, 0..256
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
            i0[size_t(x)] = a * 3;
            i1[size_t(x)] = std::min(a + 1, sw - 1) * 3;
            w[size_t(x)] = int(std::lround(f * 256));
        }
    }
};

// Output rows [y0, y1) of the dw x dh result (dst is the whole image). t must have been built.
void ScaleBilinearRows(const RgbView& src, const HTable& t, std::vector<uint8_t>& tmp, uint8_t* dst, int dw, int dh,
                       int y0, int y1) {
    tmp.resize(size_t(src.width) * 3);
    for (int y = y0; y < y1; y++) {
        double sy = (y + 0.5) * src.height / dh - 0.5;
        int a = int(std::floor(sy));
        double f = sy - a;
        if (a < 0) { a = 0; f = 0; }
        if (a >= src.height - 1) { a = src.height - 1; f = 0; }
        int b = std::min(a + 1, src.height - 1);
        int fy = int(std::lround(f * 256)), fy0 = 256 - fy;
        const uint8_t* r0 = src.data + size_t(a) * size_t(src.stride);
        const uint8_t* r1 = src.data + size_t(b) * size_t(src.stride);
        uint8_t* tp = tmp.data();
        const size_t n = size_t(src.width) * 3;
        for (size_t i = 0; i < n; i++) tp[i] = uint8_t((r0[i] * fy0 + r1[i] * fy + 128) >> 8);
        uint8_t* o = dst + size_t(y) * size_t(dw) * 3;
        for (int x = 0; x < dw; x++, o += 3) {
            int w1 = t.w[size_t(x)], w0 = 256 - w1;
            const uint8_t* p0 = tp + t.i0[size_t(x)];
            const uint8_t* p1 = tp + t.i1[size_t(x)];
            o[0] = uint8_t((p0[0] * w0 + p1[0] * w1 + 128) >> 8);
            o[1] = uint8_t((p0[1] * w0 + p1[1] * w1 + 128) >> 8);
            o[2] = uint8_t((p0[2] * w0 + p1[2] * w1 + 128) >> 8);
        }
    }
}

// RGB (w x h, both even) -> YUY2 / I420 / NV12. Chroma is averaged over the 2 (YUY2) or 2x2 (planar) pixels.
// Only rows [y0, y1) (y0 and y1 even) are converted.
void ConvertOutRows(const uint8_t* rgb, int w, int h, OutFormat f, uint8_t* out, int y0, int y1) {
    auto px = [&](int x, int y) { return rgb + (size_t(y) * size_t(w) + size_t(x)) * 3; };
    if (f == OutFormat::Yuy2) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* p = px(0, y);
            uint8_t* o = out + size_t(y) * size_t(w) * 2;
            for (int x = 0; x < w; x += 2, p += 6, o += 4) {
                int r0 = p[0], g0 = p[1], b0 = p[2], r1 = p[3], g1 = p[4], b1 = p[5];
                int r = (r0 + r1 + 1) >> 1, g = (g0 + g1 + 1) >> 1, b = (b0 + b1 + 1) >> 1;
                o[0] = LumaOf(r0, g0, b0);
                o[1] = CbOf(r, g, b);
                o[2] = LumaOf(r1, g1, b1);
                o[3] = CrOf(r, g, b);
            }
        }
        return;
    }
    uint8_t* yp = out;
    uint8_t* up = out + size_t(w) * size_t(h);
    uint8_t* vp = up + size_t(w / 2) * size_t(h / 2);  // I420 only; NV12 interleaves in up
    for (int y = y0; y < y1; y += 2) {
        for (int x = 0; x < w; x += 2) {
            const uint8_t* a = px(x, y);
            const uint8_t* b = px(x + 1, y);
            const uint8_t* c = px(x, y + 1);
            const uint8_t* d = px(x + 1, y + 1);
            yp[size_t(y) * size_t(w) + size_t(x)] = LumaOf(a[0], a[1], a[2]);
            yp[size_t(y) * size_t(w) + size_t(x) + 1] = LumaOf(b[0], b[1], b[2]);
            yp[size_t(y + 1) * size_t(w) + size_t(x)] = LumaOf(c[0], c[1], c[2]);
            yp[size_t(y + 1) * size_t(w) + size_t(x) + 1] = LumaOf(d[0], d[1], d[2]);
            int r = (a[0] + b[0] + c[0] + d[0] + 2) >> 2;
            int g = (a[1] + b[1] + c[1] + d[1] + 2) >> 2;
            int bl = (a[2] + b[2] + c[2] + d[2] + 2) >> 2;
            size_t ci = size_t(y / 2) * size_t(w / 2) + size_t(x / 2);
            if (f == OutFormat::I420) {
                up[ci] = CbOf(r, g, bl);
                vp[ci] = CrOf(r, g, bl);
            } else {
                up[ci * 2] = CbOf(r, g, bl);
                up[ci * 2 + 1] = CrOf(r, g, bl);
            }
        }
    }
    (void)h;
}

}  // namespace

size_t OutputFrameSize(OutFormat f) {
    size_t px = size_t(kOutWidth) * size_t(kOutHeight);
    return f == OutFormat::Yuy2 ? px * 2 : px * 3 / 2;
}

void FillPlaceholder(OutFormat f, std::vector<uint8_t>& out) {
    std::vector<uint8_t> rgb(size_t(kOutWidth) * size_t(kOutHeight) * 3, 40);
    out.resize(OutputFrameSize(f));
    ConvertOutRows(rgb.data(), kOutWidth, kOutHeight, f, out.data(), 0, kOutHeight);
}

struct Pipeline::Impl {
    tjhandle tj = nullptr;
    std::vector<uint8_t> decoded;
    std::vector<uint8_t> scaled;  // 1920x1080 RGB
    std::vector<std::vector<uint8_t>> tmp;  // bilinear row scratch, per worker
    Pool pool{PoolThreads()};
    Fsr1 fsr;
    HTable table;
    AdjustLut lut;
    ImageSettings lutFor;
    bool lutValid = false;
};

Pipeline::Pipeline() : impl_(new Impl) { impl_->tj = tj3Init(TJINIT_DECOMPRESS); }
Pipeline::~Pipeline() {
    if (impl_->tj) tj3Destroy(impl_->tj);
}

bool Pipeline::Process(const uint8_t* jpeg, size_t size, const Settings& s, std::vector<uint8_t>& out,
                       std::vector<uint8_t>* rgb, PipelineTimes* times) {
    using Clock = std::chrono::steady_clock;
    Impl& m = *impl_;
    if (!m.tj) return false;
    auto t0 = Clock::now();
    if (tj3DecompressHeader(m.tj, jpeg, size) != 0) return false;
    int w = tj3Get(m.tj, TJPARAM_JPEGWIDTH), h = tj3Get(m.tj, TJPARAM_JPEGHEIGHT);
    if (w < 64 || h < 64 || w > 8192 || h > 8192) return false;
    m.decoded.resize(size_t(w) * size_t(h) * 3);
    if (tj3Decompress8(m.tj, jpeg, size, m.decoded.data(), w * 3, TJPF_RGB) != 0) return false;
    auto t1 = Clock::now();

    // 16:9 crop: the full width, with the height that gives the aspect ratio
    int ch = std::min(h, (w * 9 / 16) & ~1);
    int y0 = s.crop == CropAnchor::Bottom ? h - ch : s.crop == CropAnchor::Center ? ((h - ch) / 2) & ~1 : 0;
    srcW_ = w; srcH_ = h; cropY_ = y0; cropH_ = ch;
    RgbView view{m.decoded.data() + size_t(y0) * size_t(w) * 3, w, ch, w * 3};

    if (effect_) effect_->Apply(view);

    auto t2 = Clock::now();
    if (!s.image.IsIdentity()) {
        if (!m.lutValid || !(m.lutFor == s.image)) {
            BuildAdjust(s.image, m.lut);
            m.lutFor = s.image;
            m.lutValid = true;
        }
        const int nb = (view.height + kBandRows - 1) / kBandRows;
        m.pool.Run(nb, [&](int, int i) { ApplyAdjustRows(m.lut, view, i * kBandRows, std::min(view.height, (i + 1) * kBandRows)); });
    }
    auto t3 = Clock::now();

    m.scaled.resize(size_t(kOutWidth) * size_t(kOutHeight) * 3);
    out.resize(OutputFrameSize(s.format));
    const int nb = (kOutHeight + kBandRows - 1) / kBandRows;
    auto band = [&](int i, int& y0, int& y1) { y0 = i * kBandRows; y1 = std::min(kOutHeight, y0 + kBandRows); };
    double scaleMs = 0, convertMs = 0;
    Clock::time_point t4, t5;
    if (s.upscale == Upscale::Fsr) {
        // FSR 1: planar float load, then per band EASU + RCAS + conversion, so the band stays in cache.
        m.fsr.Configure(view.width, view.height, kOutWidth, kOutHeight, s.sharpness, m.pool.Size());
        const int nsrc = (view.height + kBandRows - 1) / kBandRows;
        m.pool.Run(nsrc, [&](int, int i) {
            m.fsr.LoadRows(view.data, view.stride, i * kBandRows, std::min(view.height, (i + 1) * kBandRows));
        });
        std::atomic<int64_t> convNs{0};
        m.pool.Run(nb, [&](int w, int i) {
            int y0, y1;
            band(i, y0, y1);
            uint8_t* d = m.scaled.data() + size_t(y0) * size_t(kOutWidth) * 3;
            m.fsr.RenderBand(w, y0, y1, d, kOutWidth * 3);
            auto c0 = Clock::now();
            ConvertOutRows(m.scaled.data(), kOutWidth, kOutHeight, s.format, out.data(), y0, y1);
            convNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - c0).count();
        });
        t4 = t5 = Clock::now();
        convertMs = double(convNs.load()) / 1e6 / m.pool.Size();  // CPU time spread over the workers (approximate)
        scaleMs = Ms(t3, t4) - convertMs;
    } else {
        m.table.Build(view.width, kOutWidth);
        if (int(m.tmp.size()) != m.pool.Size()) m.tmp.assign(size_t(m.pool.Size()), {});
        m.pool.Run(nb, [&](int w, int i) {
            int y0, y1;
            band(i, y0, y1);
            ScaleBilinearRows(view, m.table, m.tmp[size_t(w)], m.scaled.data(), kOutWidth, kOutHeight, y0, y1);
        });
        t4 = Clock::now();
        m.pool.Run(nb, [&](int, int i) {
            int y0, y1;
            band(i, y0, y1);
            ConvertOutRows(m.scaled.data(), kOutWidth, kOutHeight, s.format, out.data(), y0, y1);
        });
        t5 = Clock::now();
        scaleMs = Ms(t3, t4);
        convertMs = Ms(t4, t5);
    }
    if (rgb) *rgb = m.scaled;

    if (times) {
        times->decodeMs = Ms(t0, t1);
        times->adjustMs = Ms(t2, t3);
        times->scaleMs = scaleMs;
        times->convertMs = convertMs;
        times->totalMs = Ms(t0, t5);
    }
    return true;
}

}  // namespace canon_eos_webcam
