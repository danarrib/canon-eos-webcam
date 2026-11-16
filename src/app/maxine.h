// Encapsulamento do NVIDIA Maxine Video Effects SDK (runtime 0.7.x) carregado dinamicamente.
//
// Só usa a API do SDK (NvVFX_* e NvCVImage_*): não depende do CUDA Toolkit nem do nvcc.
// Todas as imagens ficam na GPU; as conversões CPU<->GPU e de formato (u8<->f32, chunky<->planar)
// são feitas por NvCVImage_Transfer.
//
// Convenções de modo (confirmadas nos headers e na documentação do SDK):
//   - Artifact Reduction e Super Resolution: NVVFX_MODE 0 = "con" (conservador),
//     1 = "agg" (agressivo); são os sufixos dos modelos AR-con/agg e SR_2x_con/agg.
//   - Denoise: NVVFX_STRENGTH 0 = fraco ("con"), 1 = forte ("agg").
//   - Green Screen: NVVFX_MODE 0 = qualidade (usado aqui).
//   - Denoise: largura e altura da imagem precisam ser múltiplas de 8 (senão a saída vem preta).
//   - Super Resolution 2x: entrada de 90p a 2160p; Artifact Reduction: de 90p a 1080p.
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "nvVideoEffects.h"

namespace eoscam {

// Estado global do runtime: localizado uma única vez, no primeiro Init().
class MaxineRuntime {
public:
    // Procura o runtime (NV_VIDEO_EFFECTS_PATH ou o caminho padrão em Program Files), carrega a DLL e
    // cria o stream CUDA. Devolve false (e preenche Reason()) se algo falhar. Idempotente.
    static bool Init();
    static bool Available();
    static const std::string& Reason();     // por que está indisponível (em português)
    static const std::string& Version();    // "0.8.1" etc., se disponível
    static const std::string& ModelDir();
    static CUstream Stream();
};

// Texto curto para um NvCV_Status (ex.: "NVCV_ERR_MODEL (-10)").
std::string NvStatusText(NvCV_Status s);

// Efeito que transforma uma imagem BGR f32 planar na GPU: Denoise, Artifact Reduction ou
// Super Resolution 2x. Cada instância tem seus próprios buffers de entrada e saída.
class PixelEffect {
public:
    enum class Kind { Denoise, ArtifactReduction, SuperRes };

    PixelEffect() = default;
    PixelEffect(const PixelEffect&) = delete;
    PixelEffect& operator=(const PixelEffect&) = delete;
    ~PixelEffect();

    // mode: 0 = conservador, 1 = agressivo. Carrega o modelo (pode levar vários segundos).
    bool Create(Kind kind, int mode, unsigned w, unsigned h, std::string& err);
    bool Run(std::string& err);
    // Zera o estado temporal (só o Denoise tem estado).
    void ResetState();

    Kind kind() const { return kind_; }
    int mode() const { return mode_; }
    unsigned inW() const { return w_; }
    unsigned inH() const { return h_; }
    unsigned outW() const { return outW_; }
    unsigned outH() const { return outH_; }
    NvCVImage& In() { return in_; }
    NvCVImage& Out() { return out_; }

private:
    NvVFX_Handle fx_ = nullptr;
    NvVFX_StateObjectHandle state_ = nullptr;     // Denoise via AllocateState
    NvCVImage stateBuf_;                          // Denoise: estado alocado como imagem GPU (alternativa)
    void* stateArray_[1] = {nullptr};
    NvCVImage in_, out_;
    Kind kind_ = Kind::Denoise;
    int mode_ = 0;
    unsigned w_ = 0, h_ = 0, outW_ = 0, outH_ = 0;
    unsigned stateBytes_ = 0;
};

// AI Green Screen: BGR u8 chunky na GPU -> máscara A u8 (mesma resolução).
class Segmenter {
public:
    Segmenter() = default;
    Segmenter(const Segmenter&) = delete;
    Segmenter& operator=(const Segmenter&) = delete;
    ~Segmenter();

    bool Create(int mode, unsigned w, unsigned h, std::string& err);
    bool Run(std::string& err);
    void ResetState();

    int mode() const { return mode_; }
    unsigned w() const { return w_; }
    unsigned h() const { return h_; }
    NvCVImage& In() { return in_; }
    NvCVImage& Out() { return out_; }

private:
    NvVFX_Handle fx_ = nullptr;
    NvVFX_StateObjectHandle state_ = nullptr;
    NvVFX_StateObjectHandle batch_[1] = {nullptr};
    NvCVImage in_, out_;
    int mode_ = 0;
    unsigned w_ = 0, h_ = 0;
};

// Desfoque de fundo do SDK (BackgroundBlur): imagem BGR u8 + máscara A u8 -> BGR u8.
class BgBlurEffect {
public:
    BgBlurEffect() = default;
    BgBlurEffect(const BgBlurEffect&) = delete;
    BgBlurEffect& operator=(const BgBlurEffect&) = delete;
    ~BgBlurEffect();

    bool Create(unsigned w, unsigned h, std::string& err);
    bool Run(float strength, std::string& err);

    unsigned w() const { return w_; }
    unsigned h() const { return h_; }
    NvCVImage& In() { return in_; }
    NvCVImage& Mask() { return mask_; }
    NvCVImage& Out() { return out_; }

private:
    NvVFX_Handle fx_ = nullptr;
    NvCVImage in_, mask_, out_;
    unsigned w_ = 0, h_ = 0;
    bool loaded_ = false;
};

}  // namespace eoscam
