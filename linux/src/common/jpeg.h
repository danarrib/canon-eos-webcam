// TurboJPEG helpers shared by the service (preview, debug output) and the panel (preview decoding).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace canon_eos_webcam {

bool DecodeJpegRgb(const uint8_t* jpeg, size_t size, std::vector<uint8_t>& rgb, int& w, int& h);
bool EncodeJpegRgb(const uint8_t* rgb, int w, int h, int quality, std::vector<uint8_t>& out);

}  // namespace canon_eos_webcam
