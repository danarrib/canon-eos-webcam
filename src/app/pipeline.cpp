#include "pipeline.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

#include "applog.h"
#include "imaging.h"
#include "maxine.h"
#include "nvCVImage.h"

using Microsoft::WRL::ComPtr;

namespace canon_eos_webcam {

namespace {

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

// Default geometry of the T3 Live View (used to preload the effects before the first frame).
constexpr unsigned kDefaultSrcW = 1056;

struct FxKey {
    int level = 0;  // 0 = off; 1 = con; 2 = agg
    unsigned w = 0, h = 0;
    bool operator==(const FxKey& o) const { return level == o.level && w == o.w && h == o.h; }
};

// Keeps a Maxine effect and lets it be swapped "hot": the current frame keeps using the old effect
// while the new one is loaded on a separate thread (NvVFX_Load can take seconds).
// All calls, except Info(), come from the processing thread.
template <class T>
class HotEffect {
public:
    using Factory = std::function<std::unique_ptr<T>(const FxKey&, std::string&)>;

    HotEffect(Factory f, const char* what, bool showLevel = true)
        : factory_(std::move(f)), what_(what), showLevel_(showLevel) {}
    ~HotEffect() { Join(); }

    T* Get(const FxKey& want) {
        Poll(want);
        if (want.level == 0) {
            active_.reset();
            if (!loading_) SetInfo(FxState::Off, Msg(Str::FxStateOff));
            return nullptr;
        }
        if (active_ && activeKey_ == want) {
            Str s = !showLevel_ ? Str::FxStateActive
                    : want.level == 2 ? Str::FxStateActiveAgg
                                      : Str::FxStateActiveCon;
            SetInfo(FxState::Active, Msg(s));
            return active_.get();
        }
        if (!loading_ && !(failedKey_ == want)) Start(want);
        if (active_ && (activeKey_.w != want.w || activeKey_.h != want.h)) active_.reset();  // no longer fits
        return active_.get();
    }

    T* Peek() { return active_.get(); }

    // Drops the effect (and waits for a load in progress to finish).
    void Drop() {
        Join();
        active_.reset();
        ready_.reset();
    }

    FxInfo Info() const {
        std::lock_guard<std::mutex> lk(mu_);
        return info_;
    }

private:
    void SetInfo(FxState s, const Msg& t) {
        std::lock_guard<std::mutex> lk(mu_);
        info_.state = s;
        info_.text = t;
    }

    void Join() {
        if (loader_.joinable()) loader_.join();
        loading_ = false;
        done_ = false;
    }

    void Poll(const FxKey& want) {
        if (!loader_.joinable() || !done_) return;
        loader_.join();
        loading_ = false;
        done_ = false;
        if (ready_ && want.level != 0) {
            active_ = std::move(ready_);
            activeKey_ = loadingKey_;
            failedKey_ = FxKey{};
        } else if (!ready_) {
            failedKey_ = loadingKey_;
            SetInfo(FxState::Failed, Msg(Str::FxStateFailed, err_));
            Log("%s: failed to load (%s)", what_, err_.c_str());
        }
        ready_.reset();
    }

    void Start(const FxKey& want) {
        loadingKey_ = want;
        loading_ = true;
        done_ = false;
        err_.clear();
        SetInfo(FxState::Loading, Msg(active_ ? Str::FxStateLoadingMode : Str::FxStateLoadingModel));
        loader_ = std::thread([this, want] {
            double t = NowMs();
            std::string e;
            std::unique_ptr<T> fx = factory_(want, e);
            if (fx) Log("%s: loaded in %.0f ms (mode %d, %ux%u)", what_, NowMs() - t, want.level, want.w, want.h);
            ready_ = std::move(fx);
            err_ = e;
            done_ = true;
        });
    }

    Factory factory_;
    const char* what_;
    bool showLevel_;
    std::unique_ptr<T> active_;
    FxKey activeKey_, loadingKey_, failedKey_;
    std::thread loader_;
    std::atomic<bool> loading_{false}, done_{false};
    std::unique_ptr<T> ready_;
    std::string err_;
    mutable std::mutex mu_;
    FxInfo info_;
};

double Ema(double old, double sample) { return old == 0 ? sample : old * 0.9 + sample * 0.1; }

}  // namespace

struct FramePipeline::Impl {
    explicit Impl(SettingsStore& s)
        : store(s),
          dn(MakePixel(PixelEffect::Kind::Denoise), "Denoise"),
          ar(MakePixel(PixelEffect::Kind::ArtifactReduction), "Artifact Reduction"),
          sr(MakePixel(PixelEffect::Kind::SuperRes), "Super Resolution"),
          gs(MakeSegmenter(), "Green Screen", false),
          blur(MakeBlur(), "SDK blur", false) {}

    static HotEffect<PixelEffect>::Factory MakePixel(PixelEffect::Kind kind) {
        return [kind](const FxKey& k, std::string& err) -> std::unique_ptr<PixelEffect> {
            auto fx = std::make_unique<PixelEffect>();
            if (!fx->Create(kind, k.level - 1, k.w, k.h, err)) return nullptr;
            return fx;
        };
    }
    static HotEffect<Segmenter>::Factory MakeSegmenter() {
        return [](const FxKey& k, std::string& err) -> std::unique_ptr<Segmenter> {
            auto fx = std::make_unique<Segmenter>();
            if (!fx->Create(0, k.w, k.h, err)) return nullptr;
            return fx;
        };
    }
    static HotEffect<BgBlurEffect>::Factory MakeBlur() {
        return [](const FxKey& k, std::string& err) -> std::unique_ptr<BgBlurEffect> {
            auto fx = std::make_unique<BgBlurEffect>();
            if (!fx->Create(k.w, k.h, err)) return nullptr;
            return fx;
        };
    }

    bool Init();
    bool Decode(const uint8_t* jpeg, size_t n);
    void UpdateBgImage(const BackgroundSettings& bg);
    void EnsureGpu(unsigned w, unsigned h);
    bool RunGpu(const Settings& cfg, PixelEffect* dnE, PixelEffect* arE, PixelEffect* srE, Segmenter* gsE,
                BgBlurEffect* blurE, bool wantPlate, PipelineStats& st);
    void ManageEffects(const Settings& cfg, unsigned w, unsigned h, PixelEffect*& dnE, PixelEffect*& arE,
                       PixelEffect*& srE, Segmenter*& gsE, BgBlurEffect*& blurE);
    void PublishStatus(const Settings& cfg);

    SettingsStore& store;
    std::unique_ptr<ThreadPool> pool;
    ComPtr<IWICImagingFactory> wic;
    bool inited = false;
    bool maxineOk = false;
    bool forceCpuBlur = false;

    HotEffect<PixelEffect> dn, ar, sr;
    HotEffect<Segmenter> gs;
    HotEffect<BgBlurEffect> blur;

    // GPU images (created only if Maxine exists; NvCVImage uses the SDK DLLs)
    std::unique_ptr<NvCVImage> gU8, hub, tmp;
    unsigned gW = 0, gH = 0;
    BgBlurEffect* maskZeroedFor = nullptr;

    // CPU buffers
    unsigned inW = 0, inH = 0;
    std::vector<uint8_t> cpuIn, cpuOut, cpuMask, cpuPlate, tmpMask, plateSmall, plateMask, blurTmp;
    YuvImage bgImage;
    unsigned outW = 0, outH = 0;

    AdjustLut lut;
    ImageSettings lutFor;
    bool lutValid = false;
    std::wstring bgImagePath;
    bool bgImageOk = false;
    Msg bgNote;

    uint64_t gpuBackoffUntil = 0;
    int gpuFailures = 0;
    std::string lastGpuError;

    mutable std::mutex statMu;
    PipelineStats stats;
    std::vector<uint8_t> preview;
    uint64_t previewFrame = 0;
    bool previewValid = false;
    uint64_t fpsTick = 0, fpsCount = 0;
};

bool FramePipeline::Impl::Init() {
    if (inited) return true;
    inited = true;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) Log("pipeline: failed to start WIC (hr=0x%08lX)", (unsigned long)hr);
    // Measured (1080p, SR+AR and everything on): the CPU work of a frame fits in ~15-25 ms on a single thread
    // (< 33 ms), and waking the pool on every frame costs far more CPU than it gains (SR+AR: 21% of a core with
    // 1 thread versus 30% with 4). Hence the default of 1; CANON_EOS_WEBCAM_THREADS (1..8) trades CPU for latency.
    int threads = 1;
    wchar_t tv[8];
    if (GetEnvironmentVariableW(L"CANON_EOS_WEBCAM_THREADS", tv, 8) > 0 && tv[0] >= L'1' && tv[0] <= L'8') threads = tv[0] - L'0';
    pool = std::make_unique<ThreadPool>(threads);

    wchar_t v[8];
    forceCpuBlur = GetEnvironmentVariableW(L"CANON_EOS_WEBCAM_FORCE_CPU_BLUR", v, 8) > 0 && v[0] == L'1';

    maxineOk = MaxineRuntime::Init();
    {
        std::lock_guard<std::mutex> lk(statMu);
        stats.maxineAvailable = maxineOk;
        stats.maxineReason = MaxineRuntime::Reason();
        stats.maxineVersion = MaxineRuntime::Version();
    }
    if (maxineOk) {
        Log("pipeline: NVIDIA Maxine %s available (models in %s)", MaxineRuntime::Version().c_str(),
            MaxineRuntime::ModelDir().c_str());
        gU8 = std::make_unique<NvCVImage>();
        hub = std::make_unique<NvCVImage>();
        tmp = std::make_unique<NvCVImage>();
    } else {
        Log("pipeline: AI effects unavailable (%s); using the CPU only", Text(MaxineRuntime::Reason(), Lang::En).c_str());
    }
    return wic != nullptr;
}

// Decodes the JPEG, cropping 16:9 anchored at the bottom (drops the top, which is usually wall/ceiling).
bool FramePipeline::Impl::Decode(const uint8_t* jpeg, size_t n) {
    if (!wic) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), DWORD(n))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), &GUID_ContainerFormatJpeg, WICDecodeMetadataCacheOnDemand,
                                            &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return false;
    UINT sw = 0, sh = 0;
    frame->GetSize(&sw, &sh);
    if (sw < 64 || sh < 64) return false;
    UINT cw = sw, ch = sh, cx = 0, cy = 0;
    if (uint64_t(sw) * 9 > uint64_t(sh) * 16) {
        cw = UINT(std::lround(sh * 16.0 / 9.0)) & ~1u;
        cx = (sw - cw) / 2;
    } else {
        ch = UINT(std::lround(sw * 9.0 / 16.0)) & ~1u;
        cy = sh - ch;
    }
    cw &= ~1u;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    inW = cw;
    inH = ch;
    cpuIn.resize(size_t(cw) * ch * 3);
    WICRect rc{INT(cx), INT(cy), INT(cw), INT(ch)};
    return SUCCEEDED(conv->CopyPixels(&rc, cw * 3, UINT(cpuIn.size()), cpuIn.data()));
}

void FramePipeline::Impl::UpdateBgImage(const BackgroundSettings& bg) {
    if (!bg.greenScreen || bg.mode != BgMode::Image) return;
    if (bg.imagePath == bgImagePath && (bgImageOk || !bgNote.empty())) return;
    bgImagePath = bg.imagePath;
    bgImageOk = false;
    bgNote = {};
    if (bg.imagePath.empty()) {
        bgNote = Msg(Str::BgNoteNoImage);
        return;
    }
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> cv;
    if (!wic || FAILED(wic->CreateDecoderFromFilename(bg.imagePath.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(wic->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom))) {
        bgNote = Msg(Str::BgNoteOpenFailed);
        Log("background: couldn't open the image %S", bg.imagePath.c_str());
        return;
    }
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    std::vector<uint8_t> raw(size_t(w) * h * 3);
    if (FAILED(cv->CopyPixels(nullptr, w * 3, UINT(raw.size()), raw.data()))) {
        bgNote = Msg(Str::BgNoteReadFailed);
        return;
    }
    std::vector<uint8_t> cover;
    CoverBgr(raw.data(), int(w), int(h), int(w) * 3, kOutW, kOutH, cover);
    BgrToYuv(cover.data(), kOutW, kOutH, kOutW * 3, nullptr, bgImage, *pool);
    bgImageOk = true;
    Log("background: image %S loaded (%ux%u)", bg.imagePath.c_str(), w, h);
}

void FramePipeline::Impl::EnsureGpu(unsigned w, unsigned h) {
    if (gW == w && gH == h && gU8->pixels) return;
    NvCVImage_Dealloc(gU8.get());
    NvCVImage_Dealloc(hub.get());
    NvCVImage_Alloc(gU8.get(), w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
    NvCVImage_Alloc(hub.get(), w, h, NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1);
    gW = w;
    gH = h;
}

void FramePipeline::Impl::ManageEffects(const Settings& cfg, unsigned w, unsigned h, PixelEffect*& dnE,
                                        PixelEffect*& arE, PixelEffect*& srE, Segmenter*& gsE,
                                        BgBlurEffect*& blurE) {
    dnE = arE = srE = nullptr;
    gsE = nullptr;
    blurE = nullptr;
    if (!maxineOk) return;
    FxKey kdn{int(cfg.fx.denoise), w & ~7u, h & ~7u};  // Denoise requires dimensions that are multiples of 8
    FxKey kar{int(cfg.fx.artifact), w, h};
    FxKey ksr{int(cfg.fx.superRes), w, h};
    FxKey kgs{cfg.bg.greenScreen ? 1 : 0, w, h};
    FxKey kbl{(cfg.bg.greenScreen && cfg.bg.mode == BgMode::Blur && !forceCpuBlur) ? 1 : 0, w, h};
    if (kdn.level == 0) kdn.w = kdn.h = 0;
    dnE = dn.Get(kdn);
    arE = ar.Get(kar);
    srE = sr.Get(ksr);
    gsE = gs.Get(kgs);
    blurE = blur.Get(kbl);
}

#define GPU_STEP(expr, what)                                                       \
    do {                                                                           \
        NvCV_Status _s = (expr);                                                   \
        if (_s != NVCV_SUCCESS) {                                                  \
            lastGpuError = std::string(what) + ": " + NvStatusText(_s);            \
            return false;                                                          \
        }                                                                          \
    } while (0)

// Runs the GPU stages. On success, cpuOut (if a pixel effect ran), cpuMask (if Green Screen ran) and
// cpuPlate (if the SDK blur ran) are filled in.
bool FramePipeline::Impl::RunGpu(const Settings& cfg, PixelEffect* dnE, PixelEffect* arE, PixelEffect* srE,
                                 Segmenter* gsE, BgBlurEffect* blurE, bool wantPlate, PipelineStats& st) {
    CUstream stream = MaxineRuntime::Stream();
    const unsigned W = inW, H = inH;
    EnsureGpu(W, H);

    NvCVImage cpuImg;
    NvCVImage_Init(&cpuImg, W, H, int(W) * 3, cpuIn.data(), NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    double t = NowMs();
    GPU_STEP(NvCVImage_Transfer(&cpuImg, gU8.get(), 1.0f, stream, nullptr), "upload to the GPU");
    st.msUpload = NowMs() - t;

    double segMs = 0, dlMs = 0;
    if (gsE) {
        t = NowMs();
        GPU_STEP(NvCVImage_Transfer(gU8.get(), &gsE->In(), 1.0f, stream, nullptr), "Green Screen (input)");
        std::string e;
        if (!gsE->Run(e)) {
            lastGpuError = "Green Screen: " + e;
            return false;
        }
        segMs += NowMs() - t;
        t = NowMs();
        cpuMask.resize(size_t(W) * H);
        NvCVImage m;
        NvCVImage_Init(&m, W, H, int(W), cpuMask.data(), NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        GPU_STEP(NvCVImage_Transfer(&gsE->Out(), &m, 1.0f, stream, tmp.get()), "Green Screen (mask)");
        dlMs += NowMs() - t;
    }
    if (blurE && wantPlate) {
        t = NowMs();
        // The SDK blur uses the Green Screen mask to blur only the background; the result is the background
        // "plate" of the final compositing (the person comes from the sharp image, which may have Super Resolution).
        if (gsE) GPU_STEP(NvCVImage_Transfer(&gsE->Out(), &blurE->Mask(), 1.0f, stream, nullptr), "blur (mask)");
        GPU_STEP(NvCVImage_Transfer(gU8.get(), &blurE->In(), 1.0f, stream, nullptr), "blur (input)");
        std::string e;
        if (!blurE->Run(std::max(0.0f, std::min(1.0f, cfg.bg.blur / 100.0f)), e)) {
            lastGpuError = "blur: " + e;
            return false;
        }
        segMs += NowMs() - t;
        t = NowMs();
        cpuPlate.resize(size_t(W) * H * 3);
        NvCVImage pl;
        NvCVImage_Init(&pl, W, H, int(W) * 3, cpuPlate.data(), NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        GPU_STEP(NvCVImage_Transfer(&blurE->Out(), &pl, 1.0f, stream, nullptr), "blur (output)");
        dlMs += NowMs() - t;
    }

    st.msDenoise = st.msArtifact = st.msSuperRes = 0;
    if (dnE || arE || srE) {
        // hub: current image in planar BGR f32 at the source resolution
        GPU_STEP(NvCVImage_Transfer(gU8.get(), hub.get(), 1.0f / 255.0f, stream, nullptr), "conversion to float");
        std::string e;
        if (dnE) {
            t = NowMs();
            unsigned aw = dnE->inW(), ah = dnE->inH();
            unsigned oy = H - ah;  // aligned to the bottom (the leftover rows at the top get no Denoise)
            NvCVRect2i rect{0, int(oy), int(aw), int(ah)};
            NvCVPoint2i pt{0, int(oy)};
            GPU_STEP(NvCVImage_TransferRect(hub.get(), &rect, &dnE->In(), nullptr, 1.0f, stream, nullptr),
                     "Denoise (input)");
            if (!dnE->Run(e)) {
                lastGpuError = "Denoise: " + e;
                return false;
            }
            GPU_STEP(NvCVImage_TransferRect(&dnE->Out(), nullptr, hub.get(), &pt, 1.0f, stream, nullptr),
                     "Denoise (output)");
            st.msDenoise = NowMs() - t;
        }
        if (arE) {
            t = NowMs();
            GPU_STEP(NvCVImage_Transfer(hub.get(), &arE->In(), 1.0f, stream, nullptr), "Artifact Reduction (input)");
            if (!arE->Run(e)) {
                lastGpuError = "Artifact Reduction: " + e;
                return false;
            }
            GPU_STEP(NvCVImage_Transfer(&arE->Out(), hub.get(), 1.0f, stream, nullptr), "Artifact Reduction (output)");
            st.msArtifact = NowMs() - t;
        }
        NvCVImage* result = hub.get();
        unsigned rw = W, rh = H;
        if (srE) {
            t = NowMs();
            GPU_STEP(NvCVImage_Transfer(hub.get(), &srE->In(), 1.0f, stream, nullptr), "Super Resolution (input)");
            if (!srE->Run(e)) {
                lastGpuError = "Super Resolution: " + e;
                return false;
            }
            st.msSuperRes = NowMs() - t;
            result = &srE->Out();
            rw = srE->outW();
            rh = srE->outH();
        }
        t = NowMs();
        outW = rw;
        outH = rh;
        cpuOut.resize(size_t(rw) * rh * 3);
        NvCVImage o;
        NvCVImage_Init(&o, rw, rh, int(rw) * 3, cpuOut.data(), NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        GPU_STEP(NvCVImage_Transfer(result, &o, 255.0f, stream, tmp.get()), "image download");
        dlMs += NowMs() - t;
    }
    st.msSegment = segMs;
    st.msDownload = dlMs;
    return true;
}

// ---------------------------------------------------------------- FramePipeline

FramePipeline::FramePipeline(SettingsStore& store) : d_(std::make_unique<Impl>(store)) {}

FramePipeline::~FramePipeline() {
    d_->dn.Drop();
    d_->ar.Drop();
    d_->sr.Drop();
    d_->gs.Drop();
    d_->blur.Drop();
}

bool FramePipeline::Init() { return d_->Init(); }

void FramePipeline::Prewarm() {
    Impl& s = *d_;
    if (!s.inited) s.Init();
    Settings cfg = s.store.Get();
    PixelEffect *a, *b, *c;
    Segmenter* g;
    BgBlurEffect* bl;
    unsigned w = s.inW ? s.inW : kDefaultSrcW;
    unsigned h = s.inH ? s.inH : ((kDefaultSrcW * 9 / 16) & ~1u);  // 16:9 crop of the Live View (594)
    s.ManageEffects(cfg, w, h, a, b, c, g, bl);
    s.PublishStatus(cfg);
}

void FramePipeline::StreamRestarted() {
    Impl& s = *d_;
    // Called by the processing thread (the only owner of the effects).
    if (PixelEffect* e = s.dn.Peek()) e->ResetState();
    if (Segmenter* g = s.gs.Peek()) g->ResetState();
}

void FramePipeline::Impl::PublishStatus(const Settings& cfg) {
    std::lock_guard<std::mutex> lk(statMu);
    stats.maxineAvailable = maxineOk;
    auto unavailable = [&](FxInfo& i, bool wanted) {
        i.state = wanted ? FxState::Unavailable : FxState::Off;
        i.text = wanted ? MaxineRuntime::Reason() : Msg(Str::FxStateOff);
    };
    if (maxineOk) {
        stats.denoise = dn.Info();
        stats.artifact = ar.Info();
        stats.superRes = sr.Info();
        stats.greenScreen = gs.Info();
        stats.blur = blur.Info();
    } else {
        unavailable(stats.denoise, cfg.fx.denoise != FxLevel::Off);
        unavailable(stats.artifact, cfg.fx.artifact != FxLevel::Off);
        unavailable(stats.superRes, cfg.fx.superRes != FxLevel::Off);
        unavailable(stats.greenScreen, cfg.bg.greenScreen);
        unavailable(stats.blur, false);
    }
    stats.backgroundNote = bgNote;
}

bool FramePipeline::Process(const uint8_t* jpeg, size_t size, uint8_t* nv12) {
    Impl& s = *d_;
    if (!s.inited) s.Init();
    const double t0 = NowMs();
    Settings cfg = s.store.Get();

    double tDec = NowMs();
    if (!s.Decode(jpeg, size)) return false;
    double msDecode = NowMs() - tDec;

    if (!s.lutValid || !(s.lutFor == cfg.image)) {
        BuildAdjust(cfg.image, s.lut);
        s.lutFor = cfg.image;
        s.lutValid = true;
    }
    s.UpdateBgImage(cfg.bg);

    PixelEffect *dnE, *arE, *srE;
    Segmenter* gsE;
    BgBlurEffect* blurE;
    s.ManageEffects(cfg, s.inW, s.inH, dnE, arE, srE, gsE, blurE);

    PipelineStats local;  // timings of this round
    const unsigned W = s.inW, H = s.inH;
    const uint8_t* img = s.cpuIn.data();
    int iw = int(W), ih = int(H), ipitch = int(W) * 3;
    const uint8_t* mask = nullptr;
    const uint8_t* plate = nullptr;
    int pw = 0, ph = 0;

    const bool bgWanted = cfg.bg.greenScreen;
    const bool wantPlate = bgWanted && cfg.bg.mode == BgMode::Blur;
    const uint64_t now = GetTickCount64();
    const bool anyGpu = dnE || arE || srE || gsE || (blurE && wantPlate);
    if (anyGpu && now >= s.gpuBackoffUntil) {
        if (s.RunGpu(cfg, dnE, arE, srE, gsE, blurE, wantPlate, local)) {
            s.gpuFailures = 0;
            if (dnE || arE || srE) {
                img = s.cpuOut.data();
                iw = int(s.outW);
                ih = int(s.outH);
                ipitch = iw * 3;
            }
            if (gsE) {
                mask = s.cpuMask.data();
                FeatherMask(s.cpuMask.data(), int(W), int(H), int(W), s.tmpMask);
            }
            if (blurE && wantPlate) {
                plate = s.cpuPlate.data();
                pw = int(W);
                ph = int(H);
            }
        } else {
            // GPU failure on this frame: carry on on the CPU and try again in 3 s (without flooding the log)
            if (s.gpuFailures++ < 3) Log("pipeline: GPU failure (%s); frame processed on the CPU only", s.lastGpuError.c_str());
            s.gpuBackoffUntil = now + 3000;
        }
    }

    // virtual background
    FinalParams fp;
    fp.src = img;
    fp.sw = iw;
    fp.sh = ih;
    fp.spitch = ipitch;
    fp.adj = &s.lut;
    fp.out = nv12;
    if (mask && bgWanted) {
        fp.mask = mask;
        fp.mw = int(W);
        fp.mh = int(H);
        fp.mpitch = int(W);
        switch (cfg.bg.mode) {
            case BgMode::Solid:
                fp.bg = BgKind::Solid;
                fp.solidR = uint8_t(cfg.bg.color >> 16);
                fp.solidG = uint8_t(cfg.bg.color >> 8);
                fp.solidB = uint8_t(cfg.bg.color);
                break;
            case BgMode::Image:
                if (s.bgImageOk) {
                    fp.bg = BgKind::Image;
                    fp.bgImage = &s.bgImage;
                } else {
                    fp.bg = BgKind::Solid;  // no valid image: dark gray
                    fp.solidR = fp.solidG = fp.solidB = 32;
                }
                break;
            case BgMode::Blur:
                if (!plate) {
                    // blur on the CPU: downscale 4x and apply a box blur (3 passes) to the small version
                    int dw = 0, dh = 0;
                    DownscaleBgr(s.cpuIn.data(), int(W), int(H), int(W) * 3, 4, s.plateSmall, dw, dh);
                    int radius = std::max(1, int(std::lround(cfg.bg.blur / 100.0 * 10.0)));
                    if (cfg.bg.blur > 0) {
                        // mask at the same reduced scale (average of 4x4 blocks), so the blur ignores the person
                        s.plateMask.resize(size_t(dw) * dh);
                        for (int y = 0; y < dh; y++)
                            for (int x = 0; x < dw; x++) {
                                int sum = 0;
                                for (int j = 0; j < 4; j++)
                                    for (int i = 0; i < 4; i++)
                                        sum += s.cpuMask[size_t(y * 4 + j) * W + size_t(x * 4 + i)];
                                s.plateMask[size_t(y) * dw + x] = uint8_t(sum / 16);
                            }
                        MaskedBoxBlurBgr(s.plateSmall.data(), s.plateMask.data(), dw, dh, radius);
                    }
                    plate = s.plateSmall.data();
                    pw = dw;
                    ph = dh;
                }
                fp.bg = BgKind::Blur;
                fp.plate = plate;
                fp.pw = pw;
                fp.ph = ph;
                break;
        }
    }
    double tFin = NowMs();
    RenderFinal(fp, *s.pool);
    double msFinal = NowMs() - tFin;
    double msTotal = NowMs() - t0;

    s.PublishStatus(cfg);
    {
        std::lock_guard<std::mutex> lk(s.statMu);
        PipelineStats& st = s.stats;
        st.msDecode = Ema(st.msDecode, msDecode);
        st.msFinal = Ema(st.msFinal, msFinal);
        st.msTotal = Ema(st.msTotal, msTotal);
        // GPU timings only enter the average when the stage ran (otherwise they decay smoothly to zero)
        st.msUpload = anyGpu ? Ema(st.msUpload, local.msUpload) : st.msUpload * 0.9;
        st.msDenoise = dnE ? Ema(st.msDenoise, local.msDenoise) : 0;
        st.msArtifact = arE ? Ema(st.msArtifact, local.msArtifact) : 0;
        st.msSuperRes = srE ? Ema(st.msSuperRes, local.msSuperRes) : 0;
        st.msSegment = (gsE || (blurE && wantPlate)) ? Ema(st.msSegment, local.msSegment) : 0;
        st.msDownload = anyGpu ? Ema(st.msDownload, local.msDownload) : st.msDownload * 0.9;
        st.frames++;
        st.outputWidth = kOutW;
        st.outputHeight = kOutH;
        s.fpsCount++;
        uint64_t tn = GetTickCount64();
        if (s.fpsTick == 0) s.fpsTick = tn;
        if (tn - s.fpsTick >= 1000) {
            st.fpsOut = s.fpsCount * 1000.0 / double(tn - s.fpsTick);
            s.fpsTick = tn;
            s.fpsCount = 0;
        }
        if (previewWanted_.exchange(false)) {
            s.preview.assign(nv12, nv12 + size_t(kOutW) * kOutH * 3 / 2);
            s.previewFrame = st.frames;
            s.previewValid = true;
        }
    }
    return true;
}

PipelineStats FramePipeline::Stats() const {
    std::lock_guard<std::mutex> lk(d_->statMu);
    return d_->stats;
}

bool FramePipeline::GetPreview(std::vector<uint8_t>& nv12, uint64_t& frameNo) {
    std::lock_guard<std::mutex> lk(d_->statMu);
    if (!d_->previewValid) return false;
    nv12 = d_->preview;
    frameNo = d_->previewFrame;
    return true;
}

}  // namespace canon_eos_webcam
