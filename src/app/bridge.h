// A ponte: liga a câmera quando alguém usa a webcam (ou o painel está aberto), busca os quadros do
// Live View, processa (FramePipeline) e publica o NV12 1920x1080 no canal compartilhado.
//
// Duas threads de trabalho:
//   - captura (a que chama Run): única dona da sessão PTP. Conecta/desconecta, busca os JPEGs, lê os
//     eventos da câmera e executa a fila de comandos do painel entre um quadro e outro.
//   - processamento: decodifica, aplica os efeitos na GPU, ajustes e fundo, e publica. Ela recebe
//     sempre o JPEG mais recente (quadros que chegarem com ela ocupada são descartados), para que a
//     câmera continue a ~29 fps mesmo quando o processamento demora.
#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "canon_camera.h"
#include "pipeline.h"
#include "settings.h"
#include "shared.h"

namespace eoscam {

struct BridgeStatus {
    ProducerState state = ProducerState::Idle;
    bool cameraConnected = false;
    std::string cameraName;
    bool consumerActive = false;  // algum programa usando a webcam
    double fpsIn = 0;             // quadros/s vindos da câmera
    double fpsOut = 0;            // quadros/s publicados
    PipelineStats pipeline;
};

class Bridge {
public:
    Bridge(SettingsStore& store);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Laço da ponte: bloqueia até RequestStop(), g_stop ou o evento de parada ser sinalizado.
    // stopEvent pode ser nullptr. Devolve 0.
    int Run(HANDLE stopEvent, const std::atomic<bool>* externalStop);
    void RequestStop() { stop_ = true; }

    // O painel aberto conta como consumidor: mantém o Live View ligado.
    void SetPanelOpen(bool open) { panelOpen_ = open; }

    BridgeStatus Status() const;
    FramePipeline& Pipeline() { return pipeline_; }
    SettingsStore& Store() { return store_; }

    // ---- ajustes da câmera
    // Pede para a thread de captura escrever uma propriedade (label: nome para a mensagem de erro).
    void SetCameraProp(uint32_t prop, uint32_t value, const std::string& label);
    void SetCameraPropWords(uint32_t prop, const std::vector<uint32_t>& words, const std::string& label);
    // Cópia do cache de propriedades (false se a câmera não está conectada).
    bool CameraProp(uint32_t prop, PropState& out) const;
    uint32_t CameraPropsGeneration() const { return propsGen_; }
    // Última falha ao ajustar a câmera (vazio se não houve); fica disponível por alguns segundos.
    std::string CameraError() const;

private:
    struct CamCommand {
        uint32_t prop;
        std::vector<uint32_t> words;  // um valor simples = uma palavra
        bool structured;
        std::string label;
    };

    void ProcessLoop();
    void RunCameraCommands();
    void SetCameraError(const std::string& msg);

    SettingsStore& store_;
    FramePipeline pipeline_;
    SharedChannel chan_;
    CanonCamera cam_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> panelOpen_{false};
    std::atomic<bool> chanReady_{false};

    // caixa de correio: o JPEG mais recente, da captura para o processamento
    std::mutex mboxMu_;
    std::vector<uint8_t> mboxJpeg_;
    uint64_t mboxTick_ = 0;
    uint64_t mboxSeq_ = 0, mboxTaken_ = 0;
    HANDLE mboxEvent_ = nullptr;
    std::atomic<bool> prewarm_{false};
    std::atomic<bool> restarted_{false};

    std::thread proc_;

    mutable std::mutex cmdMu_;
    std::deque<CamCommand> cmds_;
    std::string camError_;
    uint64_t camErrorTick_ = 0;

    // estado exposto ao painel
    std::atomic<int> state_{int(ProducerState::Idle)};
    std::atomic<bool> camConnected_{false};
    std::atomic<bool> consumer_{false};
    std::atomic<uint32_t> propsGen_{0};
    mutable std::mutex nameMu_;
    std::string camName_;
    std::atomic<double> fpsIn_{0};
    std::atomic<uint64_t> framesOut_{0};
    std::atomic<double> fpsOut_{0};
};

}  // namespace eoscam
