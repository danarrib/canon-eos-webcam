// Image pipeline of the bridge: Live View JPEG -> NV12 1920x1080.
//
//   JPEG -> decode (WIC, CPU) -> 16:9 crop anchored at the bottom (1056x594)
//        -> [Denoise] -> [Artifact Reduction]     (GPU, Maxine, at the source resolution)
//        -> [Green Screen: mask]                  (GPU, Maxine, at the source resolution)
//        -> Super Resolution 2x (2112x1188) or, without it, plain upscaling
//        -> downscale/upscale to 1920x1080 + image adjustments + background compositing + NV12 (CPU)
//
// If the Maxine runtime is missing or the GPU fails, the AI effects become unavailable and the
// pipeline carries on on the CPU only (decode, crop, scale, adjust, convert).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "i18n.h"
#include "settings.h"

namespace eoscam {

enum class FxState { Off, Loading, Active, Failed, Unavailable };

struct FxInfo {
    FxState state = FxState::Off;
    Msg text;  // e.g. "loading the model...", "active", "failed: ..."; for Unavailable, the reason
};

// Text shown for an effect ("unavailable: <reason>" when the Maxine runtime is missing).
inline std::string FxText(const FxInfo& i, Lang l = CurrentLang()) {
    std::string t = Text(i.text, l);
    return i.state == FxState::Unavailable ? Tr(Str::UnavailablePrefix, l) + t : t;
}

struct PipelineStats {
    bool maxineAvailable = false;
    Msg maxineReason;  // why it is unavailable
    std::string maxineVersion;
    FxInfo denoise, artifact, superRes, greenScreen, blur;
    Msg backgroundNote;  // problems with the background image, etc.
    // moving averages, in ms per frame
    double msDecode = 0;    // CPU: JPEG decoding + crop
    double msUpload = 0;    // GPU: upload to the device
    double msDenoise = 0;   // GPU
    double msArtifact = 0;  // GPU
    double msSuperRes = 0;  // GPU
    double msSegment = 0;   // GPU: mask (Green Screen) + SDK blur
    double msDownload = 0;  // GPU: download of the image and the mask
    double msFinal = 0;     // CPU: scaling + adjustments + background + NV12
    double msTotal = 0;     // total per frame
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

    // Initializes WIC and the Maxine runtime (does not load the models). Call on the processing thread.
    bool Init();

    // Preloads the effects enabled in the settings (call when the webcam is opened), so the first frame
    // already comes out processed. Runs in the background.
    void Prewarm();

    // Resets the temporal state of the effects (Denoise, Green Screen): call when the stream restarts.
    void StreamRestarted();

    // Processes a Live View JPEG frame and writes the NV12 1920x1080 (kFrameBytes) to nv12.
    bool Process(const uint8_t* jpeg, size_t size, uint8_t* nv12);

    PipelineStats Stats() const;

    // Asks (for the next time a frame is processed) for a copy of the NV12 for the panel preview.
    void RequestPreview() { previewWanted_ = true; }
    bool GetPreview(std::vector<uint8_t>& nv12, uint64_t& frameNo);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    std::atomic<bool> previewWanted_{false};
};

}  // namespace eoscam
