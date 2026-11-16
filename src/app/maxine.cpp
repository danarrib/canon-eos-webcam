#include "maxine.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "nvCVImage.h"

// O proxy do SDK (third_party/maxine/src/NVVideoEffectsProxy.cpp) consulta esta variável para saber
// de onde carregar NVVideoEffects.dll. Preenchida em MaxineRuntime::Init().
char* g_nvVFXSDKPath = nullptr;

namespace eoscam {

namespace {

std::mutex g_initMutex;
bool g_initDone = false;
bool g_available = false;
std::string g_reason = "o runtime do NVIDIA Maxine ainda não foi inicializado";
std::string g_version, g_modelDir;
CUstream g_stream = nullptr;

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string St(const char* what, NvCV_Status s) { return std::string(what) + ": " + NvStatusText(s); }

#define NV_TRY(expr, what)                          \
    do {                                            \
        NvCV_Status _s = (expr);                    \
        if (_s != NVCV_SUCCESS) {                   \
            err = St(what, _s);                     \
            return false;                           \
        }                                           \
    } while (0)

// Pasta com NVVideoEffects.dll: NV_VIDEO_EFFECTS_PATH, se definida (e diferente de USE_APP_PATH);
// senão o caminho padrão do instalador.
bool LocateRuntime(std::wstring& dir, std::string& why) {
    wchar_t env[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH && wcscmp(env, L"USE_APP_PATH") != 0) {
        dir = env;
    } else {
        wchar_t pf[MAX_PATH] = {};
        GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
        dir = std::wstring(pf) + L"\\NVIDIA Corporation\\NVIDIA Video Effects";
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (!FileExists(dir + L"\\NVVideoEffects.dll")) {
        why = "runtime do NVIDIA Video Effects não encontrado em " + Utf8(dir);
        return false;
    }
    return true;
}

}  // namespace

std::string NvStatusText(NvCV_Status s) {
    const char* name = "";
    switch (s) {
        case NVCV_SUCCESS: name = "NVCV_SUCCESS"; break;
        case NVCV_ERR_GENERAL: name = "NVCV_ERR_GENERAL"; break;
        case NVCV_ERR_UNIMPLEMENTED: name = "NVCV_ERR_UNIMPLEMENTED"; break;
        case NVCV_ERR_MEMORY: name = "NVCV_ERR_MEMORY"; break;
        case NVCV_ERR_EFFECT: name = "NVCV_ERR_EFFECT"; break;
        case NVCV_ERR_SELECTOR: name = "NVCV_ERR_SELECTOR"; break;
        case NVCV_ERR_BUFFER: name = "NVCV_ERR_BUFFER"; break;
        case NVCV_ERR_PARAMETER: name = "NVCV_ERR_PARAMETER"; break;
        case NVCV_ERR_MISMATCH: name = "NVCV_ERR_MISMATCH"; break;
        case NVCV_ERR_PIXELFORMAT: name = "NVCV_ERR_PIXELFORMAT"; break;
        case NVCV_ERR_MODEL: name = "NVCV_ERR_MODEL"; break;
        case NVCV_ERR_LIBRARY: name = "NVCV_ERR_LIBRARY"; break;
        case NVCV_ERR_INITIALIZATION: name = "NVCV_ERR_INITIALIZATION"; break;
        case NVCV_ERR_FILE: name = "NVCV_ERR_FILE"; break;
        case NVCV_ERR_FEATURENOTFOUND: name = "NVCV_ERR_FEATURENOTFOUND"; break;
        case NVCV_ERR_MISSINGINPUT: name = "NVCV_ERR_MISSINGINPUT"; break;
        case NVCV_ERR_RESOLUTION: name = "NVCV_ERR_RESOLUTION"; break;
        case NVCV_ERR_UNSUPPORTEDGPU: name = "NVCV_ERR_UNSUPPORTEDGPU"; break;
        case NVCV_ERR_MODELDEPENDENCIES: name = "NVCV_ERR_MODELDEPENDENCIES"; break;
        case NVCV_ERR_MODELSUBSTITUTION: name = "NVCV_ERR_MODELSUBSTITUTION"; break;
        case NVCV_ERR_CONFIG: name = "NVCV_ERR_CONFIG"; break;
        case NVCV_ERR_CUDA_MEMORY: name = "NVCV_ERR_CUDA_MEMORY"; break;
        case NVCV_ERR_CUDA_VALUE: name = "NVCV_ERR_CUDA_VALUE"; break;
        default: name = "erro"; break;
    }
    char b[64];
    snprintf(b, sizeof b, "%s (%d)", name, int(s));
    return b;
}

// ---------------------------------------------------------------- MaxineRuntime

bool MaxineRuntime::Init() {
    std::lock_guard<std::mutex> lk(g_initMutex);
    if (g_initDone) return g_available;
    g_initDone = true;

    std::wstring dir;
    std::string why;
    if (!LocateRuntime(dir, why)) {
        g_reason = why;
        return false;
    }
    // O proxy do SDK usa SetDllDirectory(g_nvVFXSDKPath): as dependências (cudart, nvinfer, npp...)
    // ficam na mesma pasta que NVVideoEffects.dll.
    static std::string path = Narrow(dir);
    g_nvVFXSDKPath = path.data();
    SetDllDirectoryW(dir.c_str());
    g_modelDir = Utf8(dir + L"\\models");

    unsigned int ver = 0;
    NvCV_Status s = NvVFX_GetVersion(&ver);
    if (s != NVCV_SUCCESS) {
        g_reason = "não consegui carregar NVVideoEffects.dll (" + NvStatusText(s) + ")";
        return false;
    }
    char vb[32];
    snprintf(vb, sizeof vb, "%u.%u.%u", ver >> 24, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);
    g_version = vb;

    s = NvVFX_CudaStreamCreate(&g_stream);
    if (s != NVCV_SUCCESS) {
        g_reason = "não consegui criar o stream CUDA (" + NvStatusText(s) + "); GPU NVIDIA ou driver ausente?";
        return false;
    }
    g_available = true;
    g_reason.clear();
    return true;
}

bool MaxineRuntime::Available() { return g_initDone && g_available; }
const std::string& MaxineRuntime::Reason() { return g_reason; }
const std::string& MaxineRuntime::Version() { return g_version; }
const std::string& MaxineRuntime::ModelDir() { return g_modelDir; }
CUstream MaxineRuntime::Stream() { return g_stream; }

// ---------------------------------------------------------------- PixelEffect

PixelEffect::~PixelEffect() {
    if (fx_) {
        if (state_) NvVFX_DeallocateState(fx_, state_);
        NvVFX_DestroyEffect(fx_);
    }
    // in_, out_, stateBuf_ liberam seus buffers nos destrutores
}

bool PixelEffect::Create(Kind kind, int mode, unsigned w, unsigned h, std::string& err) {
    if (!MaxineRuntime::Available()) {
        err = "Maxine indisponível";
        return false;
    }
    // Medido neste runtime: com largura ou altura que não seja múltipla de 8 o Denoise "roda" mas devolve
    // a imagem toda preta; por isso o pipeline entrega a ele uma região alinhada (ver FramePipeline).
    if (kind == Kind::Denoise && ((w | h) & 7)) {
        err = "o Denoise exige largura e altura múltiplas de 8";
        return false;
    }
    kind_ = kind;
    mode_ = mode;
    w_ = w;
    h_ = h;
    outW_ = kind == Kind::SuperRes ? w * 2 : w;
    outH_ = kind == Kind::SuperRes ? h * 2 : h;

    const char* sel = kind == Kind::Denoise ? NVVFX_FX_DENOISING
                      : kind == Kind::ArtifactReduction ? NVVFX_FX_ARTIFACT_REDUCTION
                                                        : NVVFX_FX_SUPER_RES;
    NV_TRY(NvVFX_CreateEffect(sel, &fx_), "CreateEffect");
    NV_TRY(NvVFX_SetString(fx_, NVVFX_MODEL_DIRECTORY, MaxineRuntime::ModelDir().c_str()), "ModelDir");
    NV_TRY(NvVFX_SetCudaStream(fx_, NVVFX_CUDA_STREAM, MaxineRuntime::Stream()), "CudaStream");
    NV_TRY(NvCVImage_Alloc(&in_, w, h, NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1), "Alloc(entrada)");
    NV_TRY(NvCVImage_Alloc(&out_, outW_, outH_, NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1), "Alloc(saída)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE, &in_), "SetImage(entrada)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &out_), "SetImage(saída)");
    if (kind == Kind::Denoise) {
        NV_TRY(NvVFX_SetF32(fx_, NVVFX_STRENGTH, mode ? 1.0f : 0.0f), "Strength");
        NV_TRY(NvVFX_GetU32(fx_, NVVFX_STATE_SIZE, &stateBytes_), "StateSize");
    } else {
        NV_TRY(NvVFX_SetU32(fx_, NVVFX_MODE, unsigned(mode)), "Mode");
    }
    NV_TRY(NvVFX_Load(fx_), "Load");

    if (kind == Kind::Denoise) {
        // Estado temporal na GPU, sem cudaMalloc: primeiro tenta a API de objetos de estado do SDK;
        // se o runtime não aceitar para o Denoise, usa um buffer GPU alocado por NvCVImage_Alloc.
        NvCV_Status s = NvVFX_AllocateState(fx_, &state_);
        if (s == NVCV_SUCCESS && state_) {
            s = NvVFX_SetStateObjectHandleArray(fx_, NVVFX_STATE, &state_);
            if (s != NVCV_SUCCESS) {
                NvVFX_DeallocateState(fx_, state_);
                state_ = nullptr;
            }
        } else {
            state_ = nullptr;
        }
        if (!state_) {
            NV_TRY(NvCVImage_Alloc(&stateBuf_, stateBytes_, 1, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1),
                   "Alloc(estado)");
            stateArray_[0] = stateBuf_.pixels;
            NV_TRY(NvVFX_SetObject(fx_, NVVFX_STATE, stateArray_), "SetObject(estado)");
            ResetState();
        }
    }
    return true;
}

void PixelEffect::ResetState() {
    if (kind_ != Kind::Denoise || !fx_) return;
    if (state_) {
        NvVFX_ResetState(fx_, state_);
    } else if (stateBuf_.pixels) {
        // zera pela cópia de uma imagem CPU zerada de mesmo formato
        NvCVImage zero;
        if (NvCVImage_Alloc(&zero, stateBuf_.width, 1, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1) == NVCV_SUCCESS) {
            std::memset(zero.pixels, 0, size_t(zero.pitch) * zero.height);
            NvCVImage_Transfer(&zero, &stateBuf_, 1.0f, MaxineRuntime::Stream(), nullptr);
        }
    }
}

bool PixelEffect::Run(std::string& err) {
    NV_TRY(NvVFX_Run(fx_, 0), "Run");
    return true;
}

// ---------------------------------------------------------------- Segmenter

Segmenter::~Segmenter() {
    if (fx_) {
        if (state_) NvVFX_DeallocateState(fx_, state_);
        NvVFX_DestroyEffect(fx_);
    }
}

bool Segmenter::Create(int mode, unsigned w, unsigned h, std::string& err) {
    if (!MaxineRuntime::Available()) {
        err = "Maxine indisponível";
        return false;
    }
    mode_ = mode;
    w_ = w;
    h_ = h;
    NV_TRY(NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &fx_), "CreateEffect");
    NV_TRY(NvVFX_SetString(fx_, NVVFX_MODEL_DIRECTORY, MaxineRuntime::ModelDir().c_str()), "ModelDir");
    NV_TRY(NvVFX_SetU32(fx_, NVVFX_MODE, unsigned(mode)), "Mode");
    NV_TRY(NvVFX_SetCudaStream(fx_, NVVFX_CUDA_STREAM, MaxineRuntime::Stream()), "CudaStream");
    NV_TRY(NvVFX_SetU32(fx_, NVVFX_MAX_INPUT_WIDTH, w), "MaxInputWidth");
    NV_TRY(NvVFX_SetU32(fx_, NVVFX_MAX_INPUT_HEIGHT, h), "MaxInputHeight");
    NV_TRY(NvVFX_SetU32(fx_, NVVFX_MAX_NUMBER_STREAMS, 1), "MaxNumberStreams");
    NV_TRY(NvCVImage_Alloc(&in_, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc(entrada)");
    NV_TRY(NvCVImage_Alloc(&out_, w, h, NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc(máscara)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE, &in_), "SetImage(entrada)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &out_), "SetImage(saída)");
    NV_TRY(NvVFX_Load(fx_), "Load");
    NV_TRY(NvVFX_AllocateState(fx_, &state_), "AllocateState");
    batch_[0] = state_;
    NV_TRY(NvVFX_SetStateObjectHandleArray(fx_, NVVFX_STATE, batch_), "SetStateObjectHandleArray");
    return true;
}

void Segmenter::ResetState() {
    if (fx_ && state_) NvVFX_ResetState(fx_, state_);
}

bool Segmenter::Run(std::string& err) {
    NV_TRY(NvVFX_Run(fx_, 0), "Run");
    return true;
}

// ---------------------------------------------------------------- BgBlurEffect

BgBlurEffect::~BgBlurEffect() {
    if (fx_) NvVFX_DestroyEffect(fx_);
}

bool BgBlurEffect::Create(unsigned w, unsigned h, std::string& err) {
    if (!MaxineRuntime::Available()) {
        err = "Maxine indisponível";
        return false;
    }
    w_ = w;
    h_ = h;
    NV_TRY(NvVFX_CreateEffect(NVVFX_FX_BGBLUR, &fx_), "CreateEffect(BackgroundBlur)");
    NV_TRY(NvVFX_SetCudaStream(fx_, NVVFX_CUDA_STREAM, MaxineRuntime::Stream()), "CudaStream");
    NV_TRY(NvCVImage_Alloc(&in_, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc(entrada)");
    NV_TRY(NvCVImage_Alloc(&mask_, w, h, NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc(máscara)");
    NV_TRY(NvCVImage_Alloc(&out_, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc(saída)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE_0, &in_), "SetImage(entrada)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_INPUT_IMAGE_1, &mask_), "SetImage(máscara)");
    NV_TRY(NvVFX_SetImage(fx_, NVVFX_OUTPUT_IMAGE, &out_), "SetImage(saída)");
    return true;
}

bool BgBlurEffect::Run(float strength, std::string& err) {
    NV_TRY(NvVFX_SetF32(fx_, NVVFX_STRENGTH, strength), "Strength");
    if (!loaded_) {
        NV_TRY(NvVFX_Load(fx_), "Load");
        loaded_ = true;
    }
    NV_TRY(NvVFX_Run(fx_, 0), "Run");
    return true;
}

}  // namespace eoscam
