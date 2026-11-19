// AMD FidelityFX Super Resolution 1 (EASU upscale + RCAS sharpen), CPU port of the reference ffx_fsr1.h
// (GPUOpen FidelityFX-FSR, MIT License). The port is original CPU code: the 32-bit float scalar path of the
// reference (FsrEasuF / FsrRcasF, no FSR_RCAS_DENOISE), written so the compiler can vectorize the per-pixel loops.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software,
// and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Usage (all in 8-bit sRGB / gamma space, as FSR 1 expects):
//   Fsr1 f; f.Configure(inW, inH, outW, outH, sharpnessStops);
//   f.LoadRows(rgb, stride, y0, y1);        // any partition of [0, inH), in parallel
//   ... barrier ...
//   f.RenderBand(worker, y0, y1, dst, ...); // any partition of [0, outH), in parallel, one worker id per thread
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace canon_eos_webcam {

class Fsr1 {
public:
    // Rebuilds the tables when the sizes change; sharpness (RCAS, in stops: 0 = sharpest) is always updated.
    // workers = number of distinct worker ids that will call RenderBand.
    void Configure(int inW, int inH, int outW, int outH, float sharpnessStops, int workers);

    // 8-bit RGB (interleaved) -> padded planar float, source rows [y0, y1).
    void LoadRows(const uint8_t* rgb, int stride, int y0, int y1);

    // EASU + RCAS for output rows [y0, y1) -> 8-bit RGB (interleaved) at dst (row y0 first, stride bytes).
    void RenderBand(int worker, int y0, int y1, uint8_t* dst, int stride);

    // "avx2" or "generic" (chosen at runtime).
    static const char* KernelName();

private:
    static constexpr int kPadL = 2, kPadR = 3;
    int inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0, planeStride_ = 0, bandRows_ = 0;
    float sharp_ = 1.0f;
    std::vector<float> plane_[3];     // R, G, B in 0..1, rows of planeStride_ floats, kPadL/kPadR replicated edges
    std::vector<int> colIdx_;         // per output column: index of 'f' in a plane row (already includes kPadL)
    std::vector<float> colPp_;        // per output column: fractional position
    std::vector<int> rowF_;           // per output row: floor() of the source row position
    std::vector<float> rowPp_;
    std::vector<std::vector<float>> scratch_;  // per worker: EASU output rows (3 planes), band + RCAS halo
    float scaleX_ = 1, scaleY_ = 1, offX_ = 0, offY_ = 0;
};

}  // namespace canon_eos_webcam
