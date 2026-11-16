// CPU image kernels: image adjustments (brightness, contrast, gamma, temperature, tint and saturation),
// conversion to YUV, resampling to 1920x1080, compositing with the virtual background and NV12 output
// (BT.601, limited range). Once converted to Y/U/V planes at the source resolution, the image is resampled
// and composited plane by plane (alpha blending is linear, so it works in YUV just as in RGB): this avoids
// working with 3 channels at the output resolution. Bands of rows run on a small thread pool.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "settings.h"

namespace eoscam {

// Simple pool: ParallelFor(n, fn) calls fn(i) for i in [0, n) on the pool threads (and on the caller).
class ThreadPool {
public:
    explicit ThreadPool(int threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    int Size() const { return int(workers_.size()) + 1; }
    // parallel = false runs everything on the calling thread: waking threads costs more CPU than it gains
    // when the work is small (the path without AI effects, at ~29 frames/s).
    void ParallelFor(int n, const std::function<void(int)>& fn, bool parallel = true);

private:
    void Work();
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_, doneCv_;
    const std::function<void(int)>* fn_ = nullptr;
    std::atomic<int> next_{0};
    int count_ = 0;
    int active_ = 0;
    uint64_t gen_ = 0;
    bool stop_ = false;
};

// Image adjustment tables (one 256-entry LUT per channel + saturation).
struct AdjustLut {
    uint8_t b[256], g[256], r[256];
    int sat = 128;  // saturation in 1.7 fixed point (128 = 100%)
    bool identity = true;
};
void BuildAdjust(const ImageSettings& s, AdjustLut& lut);

// Slightly smooths the mask (3x3) to avoid jagged edges when it is upscaled.
void FeatherMask(uint8_t* mask, int w, int h, int pitch, std::vector<uint8_t>& tmp);

// Blur (box, 3 passes ~ Gaussian) of a small BGR u8 image, in place.
// radius in image pixels.
void BoxBlurBgr(uint8_t* img, int w, int h, int radius, std::vector<uint8_t>& tmp);

// "Mask-aware" blur: only background pixels (low mask) contribute, so the person's color doesn't bleed into
// the blurred background (no halo). mask has the same size as the image (255 = person).
void MaskedBoxBlurBgr(uint8_t* img, const uint8_t* mask, int w, int h, int radius);

// Downscales a BGR u8 image by an integer factor (block average).
void DownscaleBgr(const uint8_t* src, int sw, int sh, int spitch, int factor, std::vector<uint8_t>& dst, int& dw,
                  int& dh);

// Crops and resizes a BGR u8 image ("cover" mode: fills 1920x1080 with a center crop).
void CoverBgr(const uint8_t* src, int sw, int sh, int spitch, int dw, int dh, std::vector<uint8_t>& dst);

// Image in Y, U and V planes (BT.601, limited range); U and V have half the resolution in each direction.
struct YuvImage {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
};

// BGR u8 -> YuvImage (w and h even), applying the image adjustments (adj may be nullptr).
void BgrToYuv(const uint8_t* bgr, int w, int h, int pitch, const AdjustLut* adj, YuvImage& out, ThreadPool& pool);

enum class BgKind { None, Solid, Image, Blur };

struct FinalParams {
    // camera image (BGR u8), already with the AI effects
    const uint8_t* src = nullptr;
    int sw = 0, sh = 0, spitch = 0;
    // optional mask (u8, 255 = person), with the same aspect ratio as the image
    const uint8_t* mask = nullptr;
    int mw = 0, mh = 0, mpitch = 0;
    const AdjustLut* adj = nullptr;  // nullptr or identity: no adjustments
    BgKind bg = BgKind::None;
    uint8_t solidB = 0, solidG = 0, solidR = 0;
    const YuvImage* bgImage = nullptr;  // 1920x1080 (Image mode)
    const uint8_t* plate = nullptr;    // blurred background image, packed BGR pw x ph (Blur mode)
    int pw = 0, ph = 0;
    // NV12 1920x1080 output
    uint8_t* out = nullptr;
};

constexpr int kOutW = 1920;
constexpr int kOutH = 1080;

void RenderFinal(const FinalParams& p, ThreadPool& pool);

}  // namespace eoscam
