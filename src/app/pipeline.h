// Pipeline de imagem da ponte: JPEG do Live View -> NV12 1920x1080.
//
//   JPEG -> decodifica (WIC, CPU) -> recorte 16:9 ancorado na base (1056x594)
//        -> [Denoise] -> [Artifact Reduction]     (GPU, Maxine, na resolução de origem)
//        -> [Green Screen: máscara]               (GPU, Maxine, na resolução de origem)
//        -> Super Resolution 2x (2112x1188) ou, sem ela, aumento simples
//        -> reduz/amplia para 1920x1080 + ajustes de imagem + composição do fundo + NV12 (CPU)
//
// Se o runtime do Maxine não existir ou a GPU falhar, os efeitos de IA ficam indisponíveis e o
// pipeline segue só na CPU (decodifica, recorta, escala, ajusta, converte).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "settings.h"

namespace eoscam {

enum class FxState { Off, Loading, Active, Failed, Unavailable };

struct FxInfo {
    FxState state = FxState::Off;
    std::string text;  // ex.: "carregando...", "ativo", "falhou: ..."
};

struct PipelineStats {
    bool maxineAvailable = false;
    std::string maxineReason;   // por que está indisponível
    std::string maxineVersion;
    FxInfo denoise, artifact, superRes, greenScreen, blur;
    std::string backgroundNote;  // problemas com a imagem de fundo, etc.
    // médias móveis, em ms por quadro
    double msDecode = 0;    // CPU: decodificação JPEG + recorte
    double msUpload = 0;    // GPU: envio ao dispositivo
    double msDenoise = 0;   // GPU
    double msArtifact = 0;  // GPU
    double msSuperRes = 0;  // GPU
    double msSegment = 0;   // GPU: máscara (Green Screen) + desfoque do SDK
    double msDownload = 0;  // GPU: retorno da imagem e da máscara
    double msFinal = 0;     // CPU: escala + ajustes + fundo + NV12
    double msTotal = 0;     // total por quadro
    double fpsOut = 0;
    uint64_t frames = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
};

class ThreadPool;

class FramePipeline {
public:
    explicit FramePipeline(SettingsStore& store);
    ~FramePipeline();
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    // Inicializa WIC e o runtime do Maxine (não carrega os modelos). Chamar na thread de processamento.
    bool Init();

    // Pré-carrega os efeitos ativos nas configurações (chamar quando a webcam é aberta), para o
    // primeiro quadro já sair processado. Roda em segundo plano.
    void Prewarm();

    // Zera o estado temporal dos efeitos (Denoise, Green Screen): chamar quando o fluxo recomeça.
    void StreamRestarted();

    // Processa um quadro JPEG do Live View e escreve o NV12 1920x1080 (kFrameBytes) em nv12.
    bool Process(const uint8_t* jpeg, size_t size, uint8_t* nv12);

    PipelineStats Stats() const;

    // Pede (para a próxima vez em que um quadro for processado) uma cópia do NV12 para a prévia do painel.
    void RequestPreview() { previewWanted_ = true; }
    bool GetPreview(std::vector<uint8_t>& nv12, uint64_t& frameNo);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    std::atomic<bool> previewWanted_{false};
};

}  // namespace eoscam
