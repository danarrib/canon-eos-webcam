// Holds the NV12 frame the source delivers to the client and draws the warning screens (no frames,
// incompatible versions, etc.). The camera image arrives ready from the bridge (eoscam.exe), as NV12
// 1920x1080: this class decodes and scales nothing, so the DLL stays light inside the Frame Server
// (which must not load WIC, Maxine or CUDA).
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eoscam {

class FrameRenderer {
public:
    HRESULT Init(UINT width, UINT height);
    // Draws a warning screen (only redraws if the text changed since the last call).
    void RenderMessage(const std::wstring& title, const std::wstring& detail);
    // Call after writing a real frame into Nv12Data(): the next warning screen will be redrawn.
    void InvalidateMessage() { lastMessage_.clear(); }

    const std::vector<uint8_t>& Nv12() const { return nv12_; }
    uint8_t* Nv12Data() { return nv12_.data(); }
    uint32_t Nv12Bytes() const { return uint32_t(nv12_.size()); }
    UINT Width() const { return w_; }
    UINT Height() const { return h_; }

private:
    UINT w_ = 0, h_ = 0;
    std::vector<uint8_t> nv12_;
    std::wstring lastMessage_;
};

}  // namespace eoscam
