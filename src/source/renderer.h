// Converte os quadros JPEG do Live View (1056x704, 3:2) em NV12 no tamanho pedido
// pelo cliente: corta no centro para a proporção de saída e redimensiona.
#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eoscam {

class FrameRenderer {
public:
    HRESULT Init(UINT width, UINT height);
    bool RenderJpeg(const uint8_t* jpeg, size_t size);
    void RenderMessage(const std::wstring& title, const std::wstring& detail);

    const std::vector<uint8_t>& Nv12() const { return nv12_; }
    UINT Width() const { return w_; }
    UINT Height() const { return h_; }
    HRESULT LastError() const { return lastError_; }
    const char* LastStep() const { return lastStep_; }

private:
    UINT w_ = 0, h_ = 0;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    std::vector<uint8_t> bgra_, nv12_;
    std::wstring lastMessage_;
    HRESULT lastError_ = S_OK;
    const char* lastStep_ = "";
};

}  // namespace eoscam
