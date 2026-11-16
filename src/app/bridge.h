// The bridge: turns the camera on when someone uses the webcam (or the panel is open), fetches the Live
// View frames, processes them (FramePipeline) and publishes the NV12 1920x1080 on the shared channel.
//
// Two worker threads:
//   - capture (the one that calls Run): the only owner of the PTP session. Connects/disconnects, fetches
//     the JPEGs, reads the camera events and runs the panel's command queue between frames.
//   - processing: decodes, applies the GPU effects, adjustments and background, and publishes. It always
//     gets the most recent JPEG (frames that arrive while it is busy are dropped), so the camera keeps
//     going at ~29 fps even when processing is slow.
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
#include "i18n.h"
#include "pipeline.h"
#include "settings.h"
#include "shared.h"

namespace eoscam {

struct BridgeStatus {
    ProducerState state = ProducerState::Idle;
    bool cameraConnected = false;
    std::string cameraName;
    bool consumerActive = false;  // some program is using the webcam
    double fpsIn = 0;             // frames/s coming from the camera
    double fpsOut = 0;            // frames/s published
    PipelineStats pipeline;
};

class Bridge {
public:
    Bridge(SettingsStore& store);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Bridge loop: blocks until RequestStop(), g_stop or the stop event is signaled.
    // stopEvent may be nullptr. Returns 0.
    int Run(HANDLE stopEvent, const std::atomic<bool>* externalStop);
    void RequestStop() { stop_ = true; }

    // An open panel counts as a consumer: it keeps Live View on.
    void SetPanelOpen(bool open) { panelOpen_ = open; }

    BridgeStatus Status() const;
    FramePipeline& Pipeline() { return pipeline_; }
    SettingsStore& Store() { return store_; }

    // ---- camera settings
    // Asks the capture thread to write a property (label: name of the setting for the error message).
    void SetCameraProp(uint32_t prop, uint32_t value, Str label);
    void SetCameraPropWords(uint32_t prop, const std::vector<uint32_t>& words, Str label);
    // Copy from the property cache (false if the camera is not connected).
    bool CameraProp(uint32_t prop, PropState& out) const;
    uint32_t CameraPropsGeneration() const { return propsGen_; }
    // Last failure to change a camera setting (empty if none), in the given language; kept for a few seconds.
    std::string CameraError(Lang lang) const;

private:
    struct CamCommand {
        uint32_t prop;
        std::vector<uint32_t> words;  // a simple value = one word
        bool structured;
        Str label;
    };

    void ProcessLoop();
    void RunCameraCommands();
    void SetCameraError(const Msg& msg);

    SettingsStore& store_;
    FramePipeline pipeline_;
    SharedChannel chan_;
    CanonCamera cam_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> panelOpen_{false};
    std::atomic<bool> chanReady_{false};

    // mailbox: the most recent JPEG, from capture to processing
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
    Msg camError_;
    uint64_t camErrorTick_ = 0;

    // state exposed to the panel
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
