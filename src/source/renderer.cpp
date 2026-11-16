#include "renderer.h"

#include <cmath>

#include "color.h"

namespace eoscam {

HRESULT FrameRenderer::Init(UINT width, UINT height) {
    w_ = width;
    h_ = height;
    nv12_.assign(size_t(w_) * h_ * 3 / 2, 0);
    lastMessage_.clear();
    return S_OK;
}

void FrameRenderer::RenderMessage(const std::wstring& title, const std::wstring& detail) {
    std::wstring key = title + L"\n" + detail;
    if (key == lastMessage_ || !w_ || !h_) return;  // already rendered
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
