#include "renderer.h"

#include <cmath>

#include "color.h"

using Microsoft::WRL::ComPtr;

namespace eoscam {

HRESULT FrameRenderer::Init(UINT width, UINT height) {
    w_ = width;
    h_ = height;
    bgra_.assign(size_t(w_) * h_ * 4, 0);
    nv12_.assign(size_t(w_) * h_ * 3 / 2, 0);
    lastMessage_.clear();
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
}

bool FrameRenderer::RenderJpeg(const uint8_t* jpeg, size_t size) {
    lastError_ = E_NOT_VALID_STATE;
    lastStep_ = "init";
    if (!wic_ || !w_ || !h_) return false;
    HRESULT hr;
#define STEP(name, expr) do { lastStep_ = name; hr = (expr); if (FAILED(hr)) { lastError_ = hr; return false; } } while (0)
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    STEP("CreateStream", wic_->CreateStream(&stream));
    STEP("InitializeFromMemory", stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), DWORD(size)));
    STEP("CreateDecoderFromStream", wic_->CreateDecoderFromStream(stream.Get(), &GUID_ContainerFormatJpeg,
                                                                  WICDecodeMetadataCacheOnDemand, &decoder));
    STEP("GetFrame", decoder->GetFrame(0, &frame));

    UINT sw = 0, sh = 0;
    frame->GetSize(&sw, &sh);
    if (!sw || !sh) return false;

    // Recorte na proporção de saída: na horizontal, centralizado; na vertical, ancorado na
    // base (descarta só o topo da imagem, que costuma ser parede/teto acima da cabeça).
    double target = double(w_) / h_;
    UINT cw = sw, ch = sh;
    if (double(sw) / sh > target) cw = UINT(std::lround(sh * target)) & ~1u;
    else ch = UINT(std::lround(sw / target)) & ~1u;
    WICRect rc{INT((sw - cw) / 2), INT(sh - ch), INT(cw), INT(ch)};

    ComPtr<IWICBitmapSource> src = frame;
    if (cw != sw || ch != sh) {
        ComPtr<IWICBitmapClipper> clip;
        STEP("CreateBitmapClipper", wic_->CreateBitmapClipper(&clip));
        STEP("Clipper::Initialize", clip->Initialize(src.Get(), &rc));
        src = clip;
    }
    if (cw != w_ || ch != h_) {
        ComPtr<IWICBitmapScaler> scaler;
        auto mode = (w_ > cw) ? WICBitmapInterpolationModeLinear : WICBitmapInterpolationModeFant;
        STEP("CreateBitmapScaler", wic_->CreateBitmapScaler(&scaler));
        STEP("Scaler::Initialize", scaler->Initialize(src.Get(), w_, h_, mode));
        src = scaler;
    }
    ComPtr<IWICFormatConverter> conv;
    STEP("CreateFormatConverter", wic_->CreateFormatConverter(&conv));
    STEP("Converter::Initialize", conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                   nullptr, 0, WICBitmapPaletteTypeCustom));
    STEP("CopyPixels", conv->CopyPixels(nullptr, w_ * 4, UINT(bgra_.size()), bgra_.data()));
#undef STEP

    BgraToNv12(bgra_.data(), int(w_ * 4), int(w_), int(h_), nv12_.data());
    lastMessage_.clear();
    lastError_ = S_OK;
    return true;
}

void FrameRenderer::RenderMessage(const std::wstring& title, const std::wstring& detail) {
    std::wstring key = title + L"\n" + detail;
    if (key == lastMessage_ || !w_ || !h_) return;  // já renderizado
    lastMessage_ = key;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = LONG(w_);
    bi.bmiHeader.biHeight = -LONG(h_);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) {
        if (dc) DeleteDC(dc);
        std::fill(nv12_.begin(), nv12_.begin() + size_t(w_) * h_, uint8_t(40));
        std::fill(nv12_.begin() + size_t(w_) * h_, nv12_.end(), uint8_t(128));
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    RECT all{0, 0, LONG(w_), LONG(h_)};
    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 38));
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    int titleSize = std::max(14, int(h_ / 16));
    int detailSize = std::max(11, int(h_ / 28));
    HFONT fTitle = CreateFontW(-titleSize, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Segoe UI");
    HFONT fDetail = CreateFontW(-detailSize, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                0, L"Segoe UI");

    RECT rt{0, LONG(h_ / 2) - titleSize - titleSize / 4, LONG(w_), LONG(h_ / 2)};
    SelectObject(dc, fTitle);
    SetTextColor(dc, RGB(235, 235, 235));
    DrawTextW(dc, title.c_str(), -1, &rt, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    RECT rd{LONG(w_ / 10), LONG(h_ / 2) + detailSize / 2, LONG(w_ - w_ / 10), LONG(h_)};
    SelectObject(dc, fDetail);
    SetTextColor(dc, RGB(160, 165, 172));
    DrawTextW(dc, detail.c_str(), -1, &rd, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    GdiFlush();
    BgraToNv12(static_cast<const uint8_t*>(bits), int(w_ * 4), int(w_), int(h_), nv12_.data());

    SelectObject(dc, oldBmp);
    DeleteObject(fTitle);
    DeleteObject(fDetail);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace eoscam
