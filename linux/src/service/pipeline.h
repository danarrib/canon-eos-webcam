// JPEG -> decode -> 16:9 crop -> [effects] -> image adjustments -> bilinear scale -> output format.
// Plain C++ (the compiler vectorizes the hot loops); the maths of the adjustments and the YUV
// coefficients are the ones of src/app/imaging.cpp (BuildAdjust / AdjustPixel / LumaOf / CbOf / CrOf).
#pragma once

#include "../common/jpeg.h"
#include "settings.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace canon_eos_webcam {

constexpr int kOutWidth = 1280;
constexpr int kOutHeight = 720;

struct RgbView {
    uint8_t* data = nullptr;  // R,G,B interleaved
    int width = 0, height = 0, stride = 0;
};

// Hook for the AI effects (Maxine, later): takes and returns the cropped RGB image, before scaling.
class EffectStage {
public:
    virtual ~EffectStage() = default;
    virtual void Apply(RgbView& img) = 0;
};

struct PipelineTimes {
    double decodeMs = 0, adjustMs = 0, scaleMs = 0, convertMs = 0, totalMs = 0;
};

size_t OutputFrameSize(OutFormat f);
// Plain dark gray frame in the given format (sent while there is no camera image).
void FillPlaceholder(OutFormat f, std::vector<uint8_t>& out);

class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void SetEffectStage(EffectStage* e) { effect_ = e; }

    // Decodes and processes one Live View JPEG. out receives the frame in the given format (resized as
    // needed); rgb, if not null, receives the final 1280x720 RGB frame (what the panel previews).
    // False if the JPEG can't be decoded (the frame is dropped).
    bool Process(const uint8_t* jpeg, size_t size, const Settings& s, std::vector<uint8_t>& out,
                 std::vector<uint8_t>* rgb, PipelineTimes* times = nullptr);

    // Width/height of the last decoded JPEG and the crop that was taken (for diagnostics).
    int SrcWidth() const { return srcW_; }
    int SrcHeight() const { return srcH_; }
    int CropY() const { return cropY_; }
    int CropHeight() const { return cropH_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EffectStage* effect_ = nullptr;
    int srcW_ = 0, srcH_ = 0, cropY_ = 0, cropH_ = 0;
};

}  // namespace canon_eos_webcam
