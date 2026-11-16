// Wrapper for the NVIDIA Maxine Video Effects SDK (runtime 0.7.x), loaded dynamically.
//
// Uses only the SDK API (NvVFX_* and NvCVImage_*): no dependency on the CUDA Toolkit or nvcc.
// All images live on the GPU; CPU<->GPU and format conversions (u8<->f32, chunky<->planar) are done by
// NvCVImage_Transfer.
//
// Mode conventions (confirmed in the SDK headers and documentation):
//   - Artifact Reduction and Super Resolution: NVVFX_MODE 0 = "con" (conservative),
//     1 = "agg" (aggressive); these are the suffixes of the AR-con/agg and SR_2x_con/agg models.
//   - Denoise: NVVFX_STRENGTH 0 = weak ("con"), 1 = strong ("agg").
//   - Green Screen: NVVFX_MODE 0 = quality (used here).
//   - Denoise: the image width and height must be multiples of 8 (otherwise the output is black).
//   - Super Resolution 2x: input from 90p to 2160p; Artifact Reduction: from 90p to 1080p.
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "i18n.h"
#include "nvVideoEffects.h"

namespace eoscam {

// Global state of the runtime: located only once, on the first Init().
class MaxineRuntime {
public:
    // Looks for the runtime (NV_VIDEO_EFFECTS_PATH or the default path in Program Files), loads the DLL and
    // creates the CUDA stream. Returns false (and fills in Reason()) if something fails. Idempotent.
    static bool Init();
    static bool Available();
    static const Msg& Reason();             // why it is unavailable
    static const std::string& Version();    // "0.8.1" etc., if available
    static const std::string& ModelDir();
    static CUstream Stream();
};

// Short text for an NvCV_Status (e.g. "NVCV_ERR_MODEL (-10)").
std::string NvStatusText(NvCV_Status s);

// Effect that transforms a planar BGR f32 image on the GPU: Denoise, Artifact Reduction or
// Super Resolution 2x. Each instance has its own input and output buffers.
class PixelEffect {
public:
    enum class Kind { Denoise, ArtifactReduction, SuperRes };

    PixelEffect() = default;
    PixelEffect(const PixelEffect&) = delete;
    PixelEffect& operator=(const PixelEffect&) = delete;
    ~PixelEffect();

    // mode: 0 = conservative, 1 = aggressive. Loads the model (may take several seconds).
    bool Create(Kind kind, int mode, unsigned w, unsigned h, std::string& err);
    bool Run(std::string& err);
    // Resets the temporal state (only Denoise has state).
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
    NvCVImage stateBuf_;                          // Denoise: state allocated as a GPU image (fallback)
    void* stateArray_[1] = {nullptr};
    NvCVImage in_, out_;
    Kind kind_ = Kind::Denoise;
    int mode_ = 0;
    unsigned w_ = 0, h_ = 0, outW_ = 0, outH_ = 0;
    unsigned stateBytes_ = 0;
};

// AI Green Screen: chunky BGR u8 on the GPU -> A u8 mask (same resolution).
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

// SDK background blur (BackgroundBlur): BGR u8 image + A u8 mask -> BGR u8.
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
