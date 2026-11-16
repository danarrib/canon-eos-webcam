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

namespace eoscam {

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

// Geometria padrão do Live View da T3 (usada para pré-carregar os efeitos antes do primeiro quadro).
constexpr unsigned kDefaultSrcW = 1056;

struct FxKey {
    int level = 0;  // 0 = desligado; 1 = con; 2 = agg
    unsigned w = 0, h = 0;
    bool operator==(const FxKey& o) const { return level == o.level && w == o.w && h == o.h; }
};

const char* LevelText(int level) { return level == 2 ? "agressivo" : "conservador"; }

// Mantém um efeito do Maxine e permite trocá-lo "a quente": o quadro atual continua usando o efeito
// antigo enquanto o novo é carregado numa thread à parte (NvVFX_Load pode levar segundos).
// Todas as chamadas, exceto Info(), vêm da thread de processamento.
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
            if (!loading_) SetInfo(FxState::Off, "desligado");
            return nullptr;
        }
        if (active_ && activeKey_ == want) {
            SetInfo(FxState::Active, showLevel_ ? std::string("ativo (") + LevelText(want.level) + ")" : std::string("ativo"));
            return active_.get();
        }
        if (!loading_ && !(failedKey_ == want)) Start(want);
        if (active_ && (activeKey_.w != want.w || activeKey_.h != want.h)) active_.reset();  // não serve mais
        return active_.get();
    }

    T* Peek() { return active_.get(); }

    // Descarta o efeito (e espera um carregamento em andamento terminar).
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
    void SetInfo(FxState s, const std::string& t) {
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
            SetInfo(FxState::Failed, "falhou: " + err_);
            Log("%s: falhou ao carregar (%s)", what_, err_.c_str());
        }
        ready_.reset();
    }

    void Start(const FxKey& want) {
        loadingKey_ = want;
        loading_ = true;
        done_ = false;
        err_.clear();
        SetInfo(FxState::Loading, active_ ? "carregando o novo modo..." : "carregando o modelo...");
        loader_ = std::thread([this, want] {
            double t = NowMs();
            std::string e;
            std::unique_ptr<T> fx = factory_(want, e);
            if (fx) Log("%s: carregado em %.0f ms (modo %d, %ux%u)", what_, NowMs() - t, want.level, want.w, want.h);
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
          blur(MakeBlur(), "Desfoque do SDK", false) {}

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

    // imagens GPU (criadas só se o Maxine existir; NvCVImage usa as DLLs do SDK)
    std::unique_ptr<NvCVImage> gU8, hub, tmp;
    unsigned gW = 0, gH = 0;
    BgBlurEffect* maskZeroedFor = nullptr;

    // buffers CPU
    unsigned inW = 0, inH = 0;
    std::vector<uint8_t> cpuIn, cpuOut, cpuMask, cpuPlate, tmpMask, plateSmall, plateMask, blurTmp;
    YuvImage bgImage;
    unsigned outW = 0, outH = 0;

    AdjustLut lut;
    ImageSettings lutFor;
    bool lutValid = false;
    std::wstring bgImagePath;
    bool bgImageOk = false;
    std::string bgNote;

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
    if (FAILED(hr)) Log("pipeline: falha ao iniciar o WIC (hr=0x%08lX)", (unsigned long)hr);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int threads = int(std::min<DWORD>(4, std::max<DWORD>(1, si.dwNumberOfProcessors)));
    wchar_t tv[8];
    if (GetEnvironmentVariableW(L"EOSCAM_THREADS", tv, 8) > 0 && tv[0] >= L'1' && tv[0] <= L'8') threads = tv[0] - L'0';
    pool = std::make_unique<ThreadPool>(threads);

    wchar_t v[8];
    forceCpuBlur = GetEnvironmentVariableW(L"EOSCAM_FORCE_CPU_BLUR", v, 8) > 0 && v[0] == L'1';

    maxineOk = MaxineRuntime::Init();
    {
        std::lock_guard<std::mutex> lk(statMu);
        stats.maxineAvailable = maxineOk;
        stats.maxineReason = MaxineRuntime::Reason();
        stats.maxineVersion = MaxineRuntime::Version();
    }
    if (maxineOk) {
        Log("pipeline: NVIDIA Maxine %s disponível (modelos em %s)", MaxineRuntime::Version().c_str(),
            MaxineRuntime::ModelDir().c_str());
        gU8 = std::make_unique<NvCVImage>();
        hub = std::make_unique<NvCVImage>();
        tmp = std::make_unique<NvCVImage>();
    } else {
        Log("pipeline: efeitos de IA indisponíveis (%s); usando só a CPU", MaxineRuntime::Reason().c_str());
    }
    return wic != nullptr;
}

// Decodifica o JPEG, recortando 16:9 ancorado na base (descarta o topo, que costuma ser parede/teto).
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
    bgNote.clear();
    if (bg.imagePath.empty()) {
        bgNote = "nenhuma imagem de fundo escolhida";
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
        bgNote = "não consegui abrir a imagem de fundo";
        Log("fundo: não consegui abrir a imagem %S", bg.imagePath.c_str());
        return;
    }
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    std::vector<uint8_t> raw(size_t(w) * h * 3);
    if (FAILED(cv->CopyPixels(nullptr, w * 3, UINT(raw.size()), raw.data()))) {
        bgNote = "falha ao ler a imagem de fundo";
        return;
    }
    std::vector<uint8_t> cover;
    CoverBgr(raw.data(), int(w), int(h), int(w) * 3, kOutW, kOutH, cover);
    BgrToYuv(cover.data(), kOutW, kOutH, kOutW * 3, nullptr, bgImage, *pool);
    bgImageOk = true;
    Log("fundo: imagem %S carregada (%ux%u)", bg.imagePath.c_str(), w, h);
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
    FxKey kdn{int(cfg.fx.denoise), w & ~7u, h & ~7u};  // o Denoise exige dimensões múltiplas de 8
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

// Executa os estágios de GPU. Em caso de sucesso: cpuOut (se algum efeito de pixel rodou), cpuMask (se o
// Green Screen rodou) e cpuPlate (se o desfoque do SDK rodou) ficam preenchidos.
bool FramePipeline::Impl::RunGpu(const Settings& cfg, PixelEffect* dnE, PixelEffect* arE, PixelEffect* srE,
                                 Segmenter* gsE, BgBlurEffect* blurE, bool wantPlate, PipelineStats& st) {
    CUstream stream = MaxineRuntime::Stream();
    const unsigned W = inW, H = inH;
    EnsureGpu(W, H);

    NvCVImage cpuImg;
    NvCVImage_Init(&cpuImg, W, H, int(W) * 3, cpuIn.data(), NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
    double t = NowMs();
    GPU_STEP(NvCVImage_Transfer(&cpuImg, gU8.get(), 1.0f, stream, nullptr), "envio à GPU");
    st.msUpload = NowMs() - t;

    double segMs = 0, dlMs = 0;
    if (gsE) {
        t = NowMs();
        GPU_STEP(NvCVImage_Transfer(gU8.get(), &gsE->In(), 1.0f, stream, nullptr), "Green Screen (entrada)");
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
        GPU_STEP(NvCVImage_Transfer(&gsE->Out(), &m, 1.0f, stream, tmp.get()), "Green Screen (máscara)");
        dlMs += NowMs() - t;
    }
    if (blurE && wantPlate) {
        t = NowMs();
        // O desfoque do SDK usa a máscara do Green Screen para desfocar só o fundo; o resultado serve de
        // "placa" de fundo na composição final (a pessoa vem da imagem nítida, que pode ter Super Resolution).
        if (gsE) GPU_STEP(NvCVImage_Transfer(&gsE->Out(), &blurE->Mask(), 1.0f, stream, nullptr), "desfoque (máscara)");
        GPU_STEP(NvCVImage_Transfer(gU8.get(), &blurE->In(), 1.0f, stream, nullptr), "desfoque (entrada)");
        std::string e;
        if (!blurE->Run(std::max(0.0f, std::min(1.0f, cfg.bg.blur / 100.0f)), e)) {
            lastGpuError = "desfoque: " + e;
            return false;
        }
        segMs += NowMs() - t;
        t = NowMs();
        cpuPlate.resize(size_t(W) * H * 3);
        NvCVImage pl;
        NvCVImage_Init(&pl, W, H, int(W) * 3, cpuPlate.data(), NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        GPU_STEP(NvCVImage_Transfer(&blurE->Out(), &pl, 1.0f, stream, nullptr), "desfoque (saída)");
        dlMs += NowMs() - t;
    }

    st.msDenoise = st.msArtifact = st.msSuperRes = 0;
    if (dnE || arE || srE) {
        // hub: imagem corrente em BGR f32 planar na resolução de origem
        GPU_STEP(NvCVImage_Transfer(gU8.get(), hub.get(), 1.0f / 255.0f, stream, nullptr), "conversão para float");
        std::string e;
        if (dnE) {
            t = NowMs();
            unsigned aw = dnE->inW(), ah = dnE->inH();
            unsigned oy = H - ah;  // alinhado à base (as linhas do topo que sobram ficam sem Denoise)
            NvCVRect2i rect{0, int(oy), int(aw), int(ah)};
            NvCVPoint2i pt{0, int(oy)};
            GPU_STEP(NvCVImage_TransferRect(hub.get(), &rect, &dnE->In(), nullptr, 1.0f, stream, nullptr),
                     "Denoise (entrada)");
            if (!dnE->Run(e)) {
                lastGpuError = "Denoise: " + e;
                return false;
            }
            GPU_STEP(NvCVImage_TransferRect(&dnE->Out(), nullptr, hub.get(), &pt, 1.0f, stream, nullptr),
                     "Denoise (saída)");
            st.msDenoise = NowMs() - t;
        }
        if (arE) {
            t = NowMs();
            GPU_STEP(NvCVImage_Transfer(hub.get(), &arE->In(), 1.0f, stream, nullptr), "Artifact Reduction (entrada)");
            if (!arE->Run(e)) {
                lastGpuError = "Artifact Reduction: " + e;
                return false;
            }
            GPU_STEP(NvCVImage_Transfer(&arE->Out(), hub.get(), 1.0f, stream, nullptr), "Artifact Reduction (saída)");
            st.msArtifact = NowMs() - t;
        }
        NvCVImage* result = hub.get();
        unsigned rw = W, rh = H;
        if (srE) {
            t = NowMs();
            GPU_STEP(NvCVImage_Transfer(hub.get(), &srE->In(), 1.0f, stream, nullptr), "Super Resolution (entrada)");
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
        GPU_STEP(NvCVImage_Transfer(result, &o, 255.0f, stream, tmp.get()), "retorno da imagem");
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
    unsigned h = s.inH ? s.inH : ((kDefaultSrcW * 9 / 16) & ~1u);  // recorte 16:9 do Live View (594)
    s.ManageEffects(cfg, w, h, a, b, c, g, bl);
    s.PublishStatus(cfg);
}

void FramePipeline::StreamRestarted() {
    Impl& s = *d_;
    // Chamada pela thread de processamento (única dona dos efeitos).
    if (PixelEffect* e = s.dn.Peek()) e->ResetState();
    if (Segmenter* g = s.gs.Peek()) g->ResetState();
}

void FramePipeline::Impl::PublishStatus(const Settings& cfg) {
    std::lock_guard<std::mutex> lk(statMu);
    stats.maxineAvailable = maxineOk;
    auto unavailable = [&](FxInfo& i, bool wanted) {
        i.state = wanted ? FxState::Unavailable : FxState::Off;
        i.text = wanted ? ("indisponível: " + MaxineRuntime::Reason()) : "desligado";
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

    PipelineStats local;  // tempos desta rodada
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
            // falha na GPU neste quadro: segue na CPU e tenta de novo em 3 s (sem encher o log)
            if (s.gpuFailures++ < 3) Log("pipeline: falha na GPU (%s); quadro processado só na CPU", s.lastGpuError.c_str());
            s.gpuBackoffUntil = now + 3000;
        }
    }

    // fundo virtual
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
                    fp.bg = BgKind::Solid;  // sem imagem válida: cinza escuro
                    fp.solidR = fp.solidG = fp.solidB = 32;
                }
                break;
            case BgMode::Blur:
                if (!plate) {
                    // desfoque na CPU: reduz 4x e aplica um desfoque de caixa (3 passadas) na versão pequena
                    int dw = 0, dh = 0;
                    DownscaleBgr(s.cpuIn.data(), int(W), int(H), int(W) * 3, 4, s.plateSmall, dw, dh);
                    int radius = std::max(1, int(std::lround(cfg.bg.blur / 100.0 * 10.0)));
                    if (cfg.bg.blur > 0) {
                        // máscara na mesma escala reduzida (média de blocos 4x4), para o desfoque ignorar a pessoa
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
        // os tempos de GPU só entram na média quando o estágio rodou (senão zeram suavemente)
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

}  // namespace eoscam
