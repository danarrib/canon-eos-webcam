#include "panel.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "applog.h"
#include "autostart.h"
#include "color.h"

// Controles com visual moderno (Common Controls 6) sem precisar de um arquivo de manifesto.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace eoscam {

namespace {

constexpr wchar_t kPanelClass[] = L"EosCamPanelWindow";
constexpr UINT kMsgTray = WM_APP + 2;
constexpr UINT_PTR kTimerPreview = 1, kTimerStatus = 2;

enum Id : int {
    ID_IMG_BRIGHT = 100, ID_IMG_CONTRAST, ID_IMG_GAMMA, ID_IMG_TEMP, ID_IMG_TINT, ID_IMG_SAT, ID_IMG_RESET,
    ID_FX_SR = 120, ID_FX_AR, ID_FX_DN,
    ID_BG_ENABLE = 130, ID_BG_MODE, ID_BG_COLOR, ID_BG_BLUR, ID_BG_IMAGE, ID_BG_NOTE,
    ID_CAM_EXP = 150, ID_CAM_ISO, ID_CAM_WB, ID_CAM_KELVIN, ID_CAM_STYLE, ID_CAM_CONTRAST, ID_CAM_SHARP,
    ID_CAM_SAT, ID_CAM_TONE, ID_CAM_ALO, ID_CAM_INFO,
    ID_STATUS = 190, ID_ZOOM = 195, ID_ZOOM_HINT = 196,
    ID_VALUE_BASE = 300,  // rótulo de valor de um controle deslizante: ID_VALUE_BASE + id
    ID_LABEL_BASE = 500,  // rótulo de texto de um controle: ID_LABEL_BASE + id
    ID_NOTE_BASE = 700,   // texto de estado ao lado de um controle: ID_NOTE_BASE + id
    ID_TRAY_OPEN = 1, ID_TRAY_AUTOSTART, ID_TRAY_EXIT,
};

// ---------------------------------------------------------------- tabelas de nomes

struct NamedValue { uint32_t value; const wchar_t* name; };

const NamedValue kIsoNames[] = {
    {0x0000, L"Auto"}, {0x0001, L"Auto"}, {0x0028, L"6"}, {0x0030, L"12"}, {0x0038, L"25"}, {0x0040, L"50"},
    {0x0043, L"64"}, {0x0045, L"80"}, {0x0048, L"100"}, {0x004B, L"125"}, {0x004D, L"160"}, {0x0050, L"200"},
    {0x0053, L"250"}, {0x0055, L"320"}, {0x0058, L"400"}, {0x005B, L"500"}, {0x005D, L"640"}, {0x0060, L"800"},
    {0x0063, L"1000"}, {0x0065, L"1250"}, {0x0068, L"1600"}, {0x006B, L"2000"}, {0x006D, L"2500"},
    {0x0070, L"3200"}, {0x0073, L"4000"}, {0x0075, L"5000"}, {0x0078, L"6400"}, {0x007B, L"8000"},
    {0x007D, L"10000"}, {0x0080, L"12800"}, {0x0083, L"16000"}, {0x0085, L"20000"}, {0x0088, L"25600"},
};
const NamedValue kWbNames[] = {
    {0, L"Automático"}, {1, L"Luz do dia"}, {8, L"Sombra"}, {2, L"Nublado"}, {3, L"Tungstênio"},
    {4, L"Fluorescente"}, {5, L"Flash"}, {6, L"Personalizado"}, {9, L"Temperatura de cor (K)"},
    {10, L"Personalizado PC-1"}, {11, L"Personalizado PC-2"}, {12, L"Personalizado PC-3"},
};
const NamedValue kStyleNames[] = {
    {0x81, L"Padrão"}, {0x82, L"Retrato"}, {0x83, L"Paisagem"}, {0x84, L"Neutro"}, {0x85, L"Fiel"},
    {0x86, L"Monocromático"}, {0x87, L"Automático"}, {0x88, L"Detalhe fino"}, {0x21, L"Definido pelo usuário 1"},
    {0x22, L"Definido pelo usuário 2"}, {0x23, L"Definido pelo usuário 3"},
};
// Auto Lighting Optimizer nos modelos mais simples (T3): 0 padrão, 1 baixa, 2 forte, 3 desativada.
const NamedValue kAloNames[] = {{0, L"Padrão"}, {1, L"Baixa"}, {2, L"Forte"}, {3, L"Desativada"}};

template <size_t N>
std::wstring NameOf(const NamedValue (&t)[N], uint32_t v) {
    for (auto& e : t) if (e.value == v) return e.name;
    wchar_t b[32];
    swprintf_s(b, L"0x%X", v);
    return b;
}

// Compensação de exposição: byte com sinal em 1/8 de EV; a câmera usa 3 e 5 para 1/3 e 2/3.
std::wstring ExpCompText(uint32_t raw) {
    int n = int(int8_t(raw & 0xFF));
    int thirds = int(std::lround(n * 3.0 / 8.0));
    int a = std::abs(thirds);
    wchar_t b[48];
    if (thirds == 0) return L"0";
    const wchar_t* sign = thirds < 0 ? L"−" : L"+";
    if (a % 3 == 0) swprintf_s(b, L"%ls%d EV", sign, a / 3);
    else if (a < 3) swprintf_s(b, L"%ls%d/3 EV", sign, a % 3);
    else swprintf_s(b, L"%ls%d %d/3 EV", sign, a / 3, a % 3);
    return b;
}

uint32_t StylePropFor(uint32_t style) {
    switch (style & 0xFF) {
        case 0x81: return 0xD150;
        case 0x82: return 0xD151;
        case 0x83: return 0xD152;
        case 0x84: return 0xD153;
        case 0x85: return 0xD154;
        case 0x86: return 0xD155;
        case 0x21: return 0xD160;
        case 0x22: return 0xD161;
        case 0x23: return 0xD162;
    }
    return 0;
}

uint32_t Word(const std::vector<uint8_t>& r, size_t i) {
    return uint32_t(r[i * 4]) | uint32_t(r[i * 4 + 1]) << 8 | uint32_t(r[i * 4 + 2]) << 16 | uint32_t(r[i * 4 + 3]) << 24;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// Ícone da bandeja desenhado na hora (corpo de câmera com a lente), sem arquivo de recurso.
HICON MakeAppIcon(int size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* px = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!color || !px) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return nullptr;
    }
    double s = size / 32.0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double fx = (x + 0.5) / s, fy = (y + 0.5) / s;  // coordenadas em 32x32
            uint32_t argb = 0;
            bool body = fx >= 2 && fx <= 30 && fy >= 8 && fy <= 28;
            bool bump = fx >= 9 && fx <= 15 && fy >= 4 && fy < 8;
            if (body || bump) argb = 0xFF2B2F36;
            double dx = fx - 16, dy = fy - 18, d = std::sqrt(dx * dx + dy * dy);
            if (d <= 8.5) argb = 0xFFC9CED6;
            if (d <= 7) argb = 0xFF1B6FD1;
            if (d <= 4.2) argb = 0xFF0C2F63;
            if (d <= 2.0) argb = 0xFF7FB6FF;
            px[y * size + x] = argb;
        }
    }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace

// ================================================================= implementação

struct PanelApp::Impl {
    Impl(Bridge& b, SettingsStore& s) : bridge(b), store(s) {}

    // ---- infraestrutura
    Bridge& bridge;
    SettingsStore& store;
    DWORD uiThread = 0;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND trayWnd = nullptr, panel = nullptr;
    HFONT font = nullptr;
    HICON iconBig = nullptr, iconSmall = nullptr;
    UINT taskbarCreated = 0;
    int dpi = 96;
    bool updating = false;  // true enquanto os controles são preenchidos pelo código (ignora as notificações)
    uint32_t seenVersion = 0;
    uint32_t seenCamGen = ~0u;
    bool camWasConnected = false;
    std::string lastCamErr;
    std::wstring statusTexts[3];
    COLORREF customColors[16] = {};

    // ---- prévia
    RECT previewRc{};
    std::vector<uint8_t> frameNv12, previewBgra;
    bool havePreview = false;
    uint64_t previewFrame = 0;
    RECT swatchRc{};
    // zoom: 0 = Ajustar, 1..3 = 100%/200%/300% (pixels de tela por pixel de vídeo)
    int zoom = 0;
    double viewX = 0, viewY = 0;  // canto superior esquerdo da janela de visão, em coordenadas do vídeo
    bool dragging = false;
    POINT dragStart{};
    double dragViewX = 0, dragViewY = 0;
    int bufW = 0, bufH = 0;
    std::vector<int> xb, yb;  // limites da caixa de média por coluna/linha de destino (modo Ajustar)
    int perfCount = 0;
    double perfSum = 0, perfMax = 0;

    // ---- estado dos controles da câmera (para reconstruir só quando muda)
    std::vector<uint32_t> expList, isoList, wbList, styleList, aloList;

    int S(int v) const { return MulDiv(v, dpi, 96); }

    HWND Child(int id) const { return GetDlgItem(panel, id); }

    // ---------------------------------------------------------------- criação da janela do painel
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
              DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), panel,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), inst, nullptr);
        if (c && font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }
    void Group(const wchar_t* title, int x, int y, int w, int h) {
        Make(L"BUTTON", title, BS_GROUPBOX, x, y, w, h, -1);
    }
    void Label(int id, const wchar_t* text, int x, int y, int w) {
        Make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y + 3, w, 18, ID_LABEL_BASE + id);
    }
    void Note(int id, int x, int y, int w) {
        Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y + 3, w, 18, ID_NOTE_BASE + id);
    }
    HWND Slider(int id, int x, int y, int w, int lo, int hi, int pos, bool withValue = true, int valueW = 56) {
        HWND t = Make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 26, id);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
        SendMessageW(t, TBM_SETPOS, TRUE, pos);
        SendMessageW(t, TBM_SETPAGESIZE, 0, std::max(1, (hi - lo) / 20));
        SetWindowSubclass(t, TrackProc, UINT_PTR(id), reinterpret_cast<DWORD_PTR>(this));
        if (withValue) Make(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX, x + w + 4, y + 4, valueW, 18, ID_VALUE_BASE + id);
        return t;
    }
    HWND Combo(int id, int x, int y, int w) {
        return Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 220, id);
    }

    void BuildControls() {
        const int rowH = 26;
        // ---- esquerda: prévia (640x360 em x=12,y=12), efeitos de IA e fundo
        previewRc = {S(12), S(12), S(12 + 640), S(12 + 360)};

        // zoom logo abaixo da prévia
        Label(ID_ZOOM, L"Zoom:", 12, 378, 44);
        HWND zc = Combo(ID_ZOOM, 58, 378, 100);
        for (const wchar_t* n : {L"Ajustar", L"100%", L"200%", L"300%"})
            SendMessageW(zc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
        SendMessageW(zc, CB_SETCURSEL, WPARAM(zoom), 0);
        Make(L"STATIC", L"Roda do mouse: zoom · arrastar: mover · duplo clique: ajustar", SS_LEFT | SS_NOPREFIX, 168,
             381, 484, 18, ID_ZOOM_HINT);

        int gy = 380 + 30;
        Group(L"Efeitos de IA (GPU, NVIDIA Maxine)", 12, gy, 640, 104);
        const wchar_t* fxNames[] = {L"Super Resolution 2×", L"Redução de artefatos", L"Redução de ruído"};
        const int fxIds[] = {ID_FX_SR, ID_FX_AR, ID_FX_DN};
        for (int i = 0; i < 3; i++) {
            int y = gy + 20 + i * rowH;
            Label(fxIds[i], fxNames[i], 24, y, 170);
            HWND c = Combo(fxIds[i], 200, y, 150);
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Desligado"));
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Conservador"));
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Agressivo"));
            Note(fxIds[i], 360, y, 280);
        }

        int by = gy + 104 + 8;
        Group(L"Fundo virtual (AI Green Screen)", 12, by, 640, 134);
        Make(L"BUTTON", L"Remover o fundo", BS_AUTOCHECKBOX | WS_TABSTOP, 24, by + 20, 170, 22, ID_BG_ENABLE);
        Note(ID_BG_ENABLE, 200, by + 20, 440);
        Label(ID_BG_MODE, L"Substituir por", 24, by + 20 + rowH, 170);
        HWND m = Combo(ID_BG_MODE, 200, by + 20 + rowH, 150);
        SendMessageW(m, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cor sólida"));
        SendMessageW(m, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Desfoque"));
        SendMessageW(m, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Imagem"));
        Make(L"BUTTON", L"Cor…", BS_PUSHBUTTON | WS_TABSTOP, 360, by + 19 + rowH, 70, 24, ID_BG_COLOR);
        swatchRc = {S(438), S(by + 22 + rowH), S(438 + 40), S(by + 22 + rowH + 18)};
        Label(ID_BG_BLUR, L"Intensidade do desfoque", 24, by + 20 + 2 * rowH, 170);
        Slider(ID_BG_BLUR, 200, by + 19 + 2 * rowH, 380, 0, 100, 60);
        Make(L"BUTTON", L"Imagem…", BS_PUSHBUTTON | WS_TABSTOP, 24, by + 19 + 3 * rowH, 100, 24, ID_BG_IMAGE);
        Note(ID_BG_IMAGE, 132, by + 19 + 3 * rowH, 508);

        // ---- direita: imagem e câmera
        int rx = 664, rw = 440;
        Group(L"Imagem", rx, 12, rw, 20 + 6 * rowH + 38);
        struct S6 { int id; const wchar_t* label; int lo, hi, def; } img[] = {
            {ID_IMG_BRIGHT, L"Brilho", -100, 100, 0},   {ID_IMG_CONTRAST, L"Contraste", -100, 100, 0},
            {ID_IMG_GAMMA, L"Gamma", 50, 200, 100},      {ID_IMG_TEMP, L"Temperatura", -100, 100, 0},
            {ID_IMG_TINT, L"Tom (verde/magenta)", -100, 100, 0}, {ID_IMG_SAT, L"Saturação", 0, 200, 100}};
        for (int i = 0; i < 6; i++) {
            int y = 12 + 20 + i * rowH;
            Label(img[i].id, img[i].label, rx + 12, y, 118);
            Slider(img[i].id, rx + 130, y - 1, 250, img[i].lo, img[i].hi, img[i].def);
        }
        Make(L"BUTTON", L"Restaurar padrões", BS_PUSHBUTTON | WS_TABSTOP, rx + 12, 12 + 20 + 6 * rowH + 4, 150, 26,
             ID_IMG_RESET);

        int cy = 12 + 20 + 6 * rowH + 38 + 8;
        Group(L"Câmera (ajustes guardados pela própria câmera)", rx, cy, rw, 20 + 10 * rowH + 6 + 40);
        int y = cy + 20;
        Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, rx + 12, y + 1, rw - 24, 18, ID_CAM_INFO);
        y += 22;
        Label(ID_CAM_EXP, L"Compensação de exposição", rx + 12, y, 170);
        Combo(ID_CAM_EXP, rx + 186, y, 160);
        y += rowH;
        Label(ID_CAM_ISO, L"ISO", rx + 12, y, 170);
        Combo(ID_CAM_ISO, rx + 186, y, 160);
        Note(ID_CAM_ISO, rx + 352, y, 80);
        y += rowH;
        Label(ID_CAM_WB, L"Balanço de branco", rx + 12, y, 170);
        Combo(ID_CAM_WB, rx + 186, y, 160);
        y += rowH;
        Label(ID_CAM_KELVIN, L"Temperatura de cor", rx + 12, y, 170);
        Slider(ID_CAM_KELVIN, rx + 186, y - 1, 190, 2500, 10000, 5200, true, 60);
        y += rowH;
        Label(ID_CAM_STYLE, L"Picture Style", rx + 12, y, 170);
        Combo(ID_CAM_STYLE, rx + 186, y, 160);
        y += rowH;
        struct P4 { int id; const wchar_t* label; int lo, hi; } ps[] = {
            {ID_CAM_CONTRAST, L"  Contraste", -4, 4}, {ID_CAM_SHARP, L"  Nitidez", 0, 7},
            {ID_CAM_SAT, L"  Saturação", -4, 4}, {ID_CAM_TONE, L"  Tom de cor", -4, 4}};
        for (auto& p : ps) {
            Label(p.id, p.label, rx + 12, y, 170);
            Slider(p.id, rx + 186, y - 1, 190, p.lo, p.hi, 0, true, 60);
            y += rowH;
        }
        Label(ID_CAM_ALO, L"Auto Lighting Optimizer", rx + 12, y, 170);
        Combo(ID_CAM_ALO, rx + 186, y, 160);
        y += rowH + 2;
        Make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, rx + 12, y, rw - 24, 34, ID_CAM_INFO + 1);

        // ---- rodapé
        HWND sb = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, panel,
                                  reinterpret_cast<HMENU>(INT_PTR(ID_STATUS)), inst, nullptr);
        if (sb && font) SendMessageW(sb, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (sb) SendMessageW(sb, WM_SIZE, 0, 0);  // a barra de estado se posiciona sozinha no rodapé
    }

    // Só mexe no controle se o texto mudou (evita piscar a cada atualização do estado).
    void SetText(int id, const std::wstring& t) {
        HWND c = Child(id);
        if (!c) return;
        wchar_t cur[600] = {};
        GetWindowTextW(c, cur, 600);
        if (t != cur) SetWindowTextW(c, t.c_str());
    }

    // ---------------------------------------------------------------- sincronização controles <-> configurações
    static const wchar_t* FxText(FxLevel l) { return l == FxLevel::Agg ? L"Agressivo" : l == FxLevel::Con ? L"Conservador" : L"Desligado"; }

    void SetSlider(int id, int v) {
        SendMessageW(Child(id), TBM_SETPOS, TRUE, v);
        UpdateValueLabel(id);
    }
    int GetSlider(int id) const { return int(SendMessageW(Child(id), TBM_GETPOS, 0, 0)); }

    void UpdateValueLabel(int id) {
        int v = GetSlider(id);
        wchar_t b[40];
        switch (id) {
            case ID_IMG_GAMMA: swprintf_s(b, L"%.2f", v / 100.0); break;
            case ID_IMG_SAT: swprintf_s(b, L"%d%%", v); break;
            case ID_BG_BLUR: swprintf_s(b, L"%d", v); break;
            case ID_CAM_KELVIN: swprintf_s(b, L"%d K", v); break;
            case ID_CAM_CONTRAST: case ID_CAM_SAT: case ID_CAM_TONE: swprintf_s(b, L"%+d", v); break;
            case ID_CAM_SHARP: swprintf_s(b, L"%d", v); break;
            default: swprintf_s(b, L"%+d", v); break;
        }
        if (v == 0 && id != ID_IMG_GAMMA && id != ID_CAM_SHARP && id != ID_BG_BLUR && id != ID_IMG_SAT &&
            id != ID_CAM_KELVIN)
            wcscpy_s(b, L"0");
        SetText(ID_VALUE_BASE + id, b);
    }

    void EnableCtl(int id, bool on) {
        if (HWND c = Child(id)) EnableWindow(c, on ? TRUE : FALSE);
        if (HWND l = Child(ID_LABEL_BASE + id)) EnableWindow(l, on ? TRUE : FALSE);
        if (HWND v = Child(ID_VALUE_BASE + id)) EnableWindow(v, on ? TRUE : FALSE);
    }

    void SyncFromSettings() {
        updating = true;
        uint32_t ver = 0;
        Settings s = store.Get(&ver);
        seenVersion = ver;
        SetSlider(ID_IMG_BRIGHT, s.image.brightness);
        SetSlider(ID_IMG_CONTRAST, s.image.contrast);
        SetSlider(ID_IMG_GAMMA, int(std::lround(s.image.gamma * 100)));
        SetSlider(ID_IMG_TEMP, s.image.temperature);
        SetSlider(ID_IMG_TINT, s.image.tint);
        SetSlider(ID_IMG_SAT, s.image.saturation);
        SendMessageW(Child(ID_FX_SR), CB_SETCURSEL, int(s.fx.superRes), 0);
        SendMessageW(Child(ID_FX_AR), CB_SETCURSEL, int(s.fx.artifact), 0);
        SendMessageW(Child(ID_FX_DN), CB_SETCURSEL, int(s.fx.denoise), 0);
        CheckDlgButton(panel, ID_BG_ENABLE, s.bg.greenScreen ? BST_CHECKED : BST_UNCHECKED);
        SendMessageW(Child(ID_BG_MODE), CB_SETCURSEL, int(s.bg.mode), 0);
        SetSlider(ID_BG_BLUR, s.bg.blur);
        std::wstring name = s.bg.imagePath.empty() ? L"(nenhuma imagem escolhida)" : PathFindFileNameW(s.bg.imagePath.c_str());
        SetText(ID_NOTE_BASE + ID_BG_IMAGE, name.c_str());
        UpdateBgEnables(s);
        updating = false;
        InvalidateRect(panel, &swatchRc, FALSE);
    }

    void UpdateBgEnables(const Settings& s) {
        bool maxine = bridge.Pipeline().Stats().maxineAvailable;
        EnableCtl(ID_BG_ENABLE, maxine);
        bool on = maxine && s.bg.greenScreen;
        EnableCtl(ID_BG_MODE, on);
        EnableCtl(ID_BG_COLOR, on && s.bg.mode == BgMode::Solid);
        EnableCtl(ID_BG_BLUR, on && s.bg.mode == BgMode::Blur);
        EnableCtl(ID_BG_IMAGE, on && s.bg.mode == BgMode::Image);
    }

    void ResetImage() {
        store.Update([](Settings& s) { s.image = ImageSettings{}; });
        SyncFromSettings();
        seenVersion = store.Version();
    }

    // ---------------------------------------------------------------- reações dos controles
    void OnImageSlider(int id) {
        int v = GetSlider(id);
        UpdateValueLabel(id);
        store.Update([&](Settings& s) {
            switch (id) {
                case ID_IMG_BRIGHT: s.image.brightness = v; break;
                case ID_IMG_CONTRAST: s.image.contrast = v; break;
                case ID_IMG_GAMMA: s.image.gamma = v / 100.0f; break;
                case ID_IMG_TEMP: s.image.temperature = v; break;
                case ID_IMG_TINT: s.image.tint = v; break;
                case ID_IMG_SAT: s.image.saturation = v; break;
                case ID_BG_BLUR: s.bg.blur = v; break;
            }
        });
        seenVersion = store.Version();
    }

    void ResetSlider(int id) {
        static const struct { int id, def; } defs[] = {
            {ID_IMG_BRIGHT, 0}, {ID_IMG_CONTRAST, 0}, {ID_IMG_GAMMA, 100}, {ID_IMG_TEMP, 0}, {ID_IMG_TINT, 0},
            {ID_IMG_SAT, 100}, {ID_BG_BLUR, 60}};
        for (auto& d : defs)
            if (d.id == id) {
                SetSlider(id, d.def);
                OnImageSlider(id);
            }
    }

    void OnFxCombo(int id) {
        int sel = int(SendMessageW(Child(id), CB_GETCURSEL, 0, 0));
        if (sel < 0) return;
        store.Update([&](Settings& s) {
            FxLevel l = FxLevel(sel);
            if (id == ID_FX_SR) s.fx.superRes = l;
            else if (id == ID_FX_AR) s.fx.artifact = l;
            else s.fx.denoise = l;
        });
        seenVersion = store.Version();
    }

    void OnBackground(int id) {
        store.Update([&](Settings& s) {
            if (id == ID_BG_ENABLE) s.bg.greenScreen = IsDlgButtonChecked(panel, ID_BG_ENABLE) == BST_CHECKED;
            else if (id == ID_BG_MODE) {
                int sel = int(SendMessageW(Child(ID_BG_MODE), CB_GETCURSEL, 0, 0));
                if (sel >= 0) s.bg.mode = BgMode(sel);
            }
        });
        seenVersion = store.Version();
        UpdateBgEnables(store.Get());
    }

    void PickColor() {
        Settings s = store.Get();
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = panel;
        cc.rgbResult = RGB((s.bg.color >> 16) & 0xFF, (s.bg.color >> 8) & 0xFF, s.bg.color & 0xFF);
        cc.lpCustColors = customColors;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) return;
        uint32_t rgb = (uint32_t(GetRValue(cc.rgbResult)) << 16) | (uint32_t(GetGValue(cc.rgbResult)) << 8) |
                       GetBValue(cc.rgbResult);
        store.Update([&](Settings& st) { st.bg.color = rgb; });
        seenVersion = store.Version();
        InvalidateRect(panel, &swatchRc, FALSE);
    }

    void PickImage() {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        COMDLG_FILTERSPEC filters[] = {{L"Imagens (PNG, JPG, BMP)", L"*.png;*.jpg;*.jpeg;*.bmp"}, {L"Todos os arquivos", L"*.*"}};
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(L"Escolher a imagem de fundo");
        if (FAILED(dlg->Show(panel))) return;
        ComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return;
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        std::wstring p = path;
        CoTaskMemFree(path);
        store.Update([&](Settings& st) { st.bg.imagePath = p; st.bg.mode = BgMode::Image; });
        SyncFromSettings();
    }

    // ---- câmera
    uint32_t ComboValue(int id) const {
        HWND c = Child(id);
        int sel = int(SendMessageW(c, CB_GETCURSEL, 0, 0));
        if (sel < 0) return ~0u;
        return uint32_t(SendMessageW(c, CB_GETITEMDATA, WPARAM(sel), 0));
    }

    void OnCameraCombo(int id) {
        uint32_t v = ComboValue(id);
        if (v == ~0u) return;
        switch (id) {
            case ID_CAM_EXP: bridge.SetCameraProp(kPropExpComp, v, "compensação de exposição"); break;
            case ID_CAM_ISO: bridge.SetCameraProp(kPropISO, v, "ISO"); break;
            case ID_CAM_WB: bridge.SetCameraProp(kPropWhiteBalance, v, "balanço de branco"); break;
            case ID_CAM_STYLE: bridge.SetCameraProp(kPropPictureStyle, v, "Picture Style"); break;
            case ID_CAM_ALO: bridge.SetCameraProp(kPropAlo, v, "Auto Lighting Optimizer"); break;
        }
    }

    void OnCameraSlider(int id, int code) {
        UpdateValueLabel(id);
        if (code == TB_THUMBTRACK) return;  // só envia à câmera quando o usuário solta o controle
        int v = GetSlider(id);
        if (id == ID_CAM_KELVIN) {
            bridge.SetCameraProp(kPropColorTemp, uint32_t(v), "temperatura de cor");
            return;
        }
        PropState st;
        if (!bridge.CameraProp(kPropPictureStyle, st) || !st.hasValue) return;
        uint32_t sp = StylePropFor(st.value);
        PropState ps;
        if (!sp || !bridge.CameraProp(sp, ps) || ps.raw.size() < 28) return;
        size_t n = ps.raw.size() / 4;
        std::vector<uint32_t> words(n);
        for (size_t i = 0; i < n; i++) words[i] = Word(ps.raw, i);
        size_t base = (ps.raw.size() == 32) ? 2 : 1;  // estilos do usuário têm uma palavra a mais (estilo base)
        size_t idx = base + (id == ID_CAM_CONTRAST ? 0 : id == ID_CAM_SHARP ? 1 : id == ID_CAM_SAT ? 2 : 3);
        if (idx >= n) return;
        words[idx] = uint32_t(int32_t(v));
        bridge.SetCameraPropWords(sp, words, "parâmetro do Picture Style");
    }

    void FillCombo(int id, const std::vector<uint32_t>& values, std::wstring (*namer)(uint32_t), uint32_t current,
                   bool hasCurrent) {
        HWND c = Child(id);
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (size_t i = 0; i < values.size(); i++) {
            std::wstring t = namer(values[i]);
            int idx = int(SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str())));
            SendMessageW(c, CB_SETITEMDATA, WPARAM(idx), LPARAM(values[i]));
            if (hasCurrent && values[i] == current) sel = idx;
        }
        SendMessageW(c, CB_SETCURSEL, WPARAM(sel), 0);
    }

    static std::wstring NameIso(uint32_t v) { return NameOf(kIsoNames, v); }
    static std::wstring NameWb(uint32_t v) { return NameOf(kWbNames, v); }
    static std::wstring NameStyle(uint32_t v) { return NameOf(kStyleNames, v); }
    static std::wstring NameAlo(uint32_t v) { return NameOf(kAloNames, v); }
    static std::wstring NameExp(uint32_t v) { return ExpCompText(v); }

    void RefreshCamera(const BridgeStatus& bs) {
        updating = true;
        bool conn = bs.cameraConnected;
        std::wstring info;
        if (!conn) {
            info = L"Câmera desconectada ou desligada: ajustes indisponíveis.";
        } else {
            info = Widen(bs.cameraName) + L" (só os ajustes que o modo atual permite)";
        }
        SetText(ID_CAM_INFO, info.c_str());

        PropState p;
        // compensação de exposição
        bool has = conn && bridge.CameraProp(kPropExpComp, p) && p.hasList && !p.list.empty();
        if (has) FillCombo(ID_CAM_EXP, p.list, NameExp, p.value, p.hasValue);
        else SendMessageW(Child(ID_CAM_EXP), CB_RESETCONTENT, 0, 0);
        EnableCtl(ID_CAM_EXP, has);
        // ISO
        has = conn && bridge.CameraProp(kPropISO, p) && p.hasList && !p.list.empty();
        if (has) FillCombo(ID_CAM_ISO, p.list, NameIso, p.value, p.hasValue);
        else SendMessageW(Child(ID_CAM_ISO), CB_RESETCONTENT, 0, 0);
        EnableCtl(ID_CAM_ISO, has);
        if (conn && !has) {
            bool aut = bridge.CameraProp(kPropISO, p) && p.hasValue;
            SetText(ID_NOTE_BASE + ID_CAM_ISO, aut ? L"(automático)" : L"");
        } else {
            SetText(ID_NOTE_BASE + ID_CAM_ISO, L"");
        }
        // balanço de branco
        PropState wb;
        has = conn && bridge.CameraProp(kPropWhiteBalance, wb) && wb.hasList && !wb.list.empty();
        if (has) FillCombo(ID_CAM_WB, wb.list, NameWb, wb.value, wb.hasValue);
        else SendMessageW(Child(ID_CAM_WB), CB_RESETCONTENT, 0, 0);
        EnableCtl(ID_CAM_WB, has);
        // temperatura de cor (Kelvin): só se a câmera oferece o modo "temperatura de cor" (valor 9)
        bool kelvinOk = has && std::find(wb.list.begin(), wb.list.end(), 9u) != wb.list.end();
        PropState kt;
        bool haveK = kelvinOk && bridge.CameraProp(kPropColorTemp, kt) && kt.hasValue;
        if (haveK) SetSlider(ID_CAM_KELVIN, int(std::min<uint32_t>(10000, std::max<uint32_t>(2500, kt.value))));
        EnableCtl(ID_CAM_KELVIN, kelvinOk && wb.hasValue && wb.value == 9);
        if (!kelvinOk) SetText(ID_VALUE_BASE + ID_CAM_KELVIN, conn ? L"n/d" : L"");
        // Picture Style e seus parâmetros
        PropState st;
        has = conn && bridge.CameraProp(kPropPictureStyle, st) && st.hasList && !st.list.empty();
        if (has) {
            std::vector<uint32_t> styles;
            for (uint32_t v : st.list) styles.push_back(v & 0xFF);
            FillCombo(ID_CAM_STYLE, styles, NameStyle, st.value & 0xFF, st.hasValue);
        } else {
            SendMessageW(Child(ID_CAM_STYLE), CB_RESETCONTENT, 0, 0);
        }
        EnableCtl(ID_CAM_STYLE, has);
        PropState sp;
        uint32_t spProp = (has && st.hasValue) ? StylePropFor(st.value) : 0;
        bool haveParams = spProp && bridge.CameraProp(spProp, sp) && sp.raw.size() >= 28;
        const int pids[] = {ID_CAM_CONTRAST, ID_CAM_SHARP, ID_CAM_SAT, ID_CAM_TONE};
        for (int i = 0; i < 4; i++) {
            EnableCtl(pids[i], haveParams);
            if (haveParams) {
                size_t base = (sp.raw.size() == 32) ? 2 : 1;
                if (base + size_t(i) < sp.raw.size() / 4) SetSlider(pids[i], int(int32_t(Word(sp.raw, base + size_t(i)))));
            } else {
                SetText(ID_VALUE_BASE + pids[i], conn ? L"n/d" : L"");
            }
        }
        // Auto Lighting Optimizer (só se a câmera informa o valor; o modelo informa 0..3)
        has = conn && bridge.CameraProp(kPropAlo, p) && p.hasValue && p.value < 0x100;
        if (has) {
            std::vector<uint32_t> vals = {0, 1, 2, 3};
            FillCombo(ID_CAM_ALO, vals, NameAlo, p.value, true);
        } else {
            SendMessageW(Child(ID_CAM_ALO), CB_RESETCONTENT, 0, 0);
        }
        EnableCtl(ID_CAM_ALO, has);
        updating = false;
    }

    // ---------------------------------------------------------------- estado e prévia
    void UpdateStatus(const BridgeStatus& bs) {
        HWND sb = Child(ID_STATUS);
        RECT rc;
        GetClientRect(panel, &rc);
        int w = rc.right;
        int parts[] = {w * 40 / 100, w * 72 / 100, -1};
        SendMessageW(sb, WM_SIZE, 0, 0);
        SendMessageW(sb, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
        std::wstring st;
        switch (bs.state) {
            case ProducerState::Streaming: st = bs.fpsIn > 0.5 ? L"ao vivo" : L"ligando o Live View"; break;
            case ProducerState::Starting: st = L"ligando a câmera"; break;
            case ProducerState::NoCamera: st = L"sem câmera (ligada? USB?)"; break;
            case ProducerState::Idle: st = L"aguardando"; break;
            default: st = L"parada"; break;
        }
        std::wstring t0 = L"Ponte: " + st;
        if (bs.cameraConnected && !bs.cameraName.empty()) t0 += L"  |  " + Widen(bs.cameraName);
        wchar_t b[200];
        swprintf_s(b, L"Entrada %.1f fps  |  saída %.1f fps", bs.fpsIn, bs.fpsOut);
        std::wstring t1 = b;
        const PipelineStats& ps = bs.pipeline;
        double gpu = ps.msUpload + ps.msDenoise + ps.msArtifact + ps.msSuperRes + ps.msSegment + ps.msDownload;
        double cpu = ps.msDecode + ps.msFinal;
        swprintf_s(b, L"Processamento %.1f ms/quadro (GPU %.1f, CPU %.1f)", ps.msTotal, gpu, cpu);
        std::wstring t2 = b;
        const std::wstring* texts[] = {&t0, &t1, &t2};
        for (int i = 0; i < 3; i++) {
            if (statusTexts[i] == *texts[i]) continue;
            statusTexts[i] = *texts[i];
            SendMessageW(sb, SB_SETTEXTW, WPARAM(i), reinterpret_cast<LPARAM>(texts[i]->c_str()));
        }

        // estado de cada efeito
        auto fx = [&](int id, const FxInfo& i) {
            std::wstring t = Widen(i.text);
            SetText(ID_NOTE_BASE + id, t.c_str());
        };
        bool maxine = ps.maxineAvailable;
        for (int id : {ID_FX_SR, ID_FX_AR, ID_FX_DN}) EnableCtl(id, maxine);
        if (!maxine) {
            std::wstring why = L"indisponível: " + Widen(ps.maxineReason);
            for (int id : {ID_FX_SR, ID_FX_AR, ID_FX_DN}) SetText(ID_NOTE_BASE + id, why.c_str());
            SetText(ID_NOTE_BASE + ID_BG_ENABLE, why.c_str());
        } else {
            fx(ID_FX_SR, ps.superRes);
            fx(ID_FX_AR, ps.artifact);
            fx(ID_FX_DN, ps.denoise);
            std::wstring g = Widen(ps.greenScreen.text);
            if (!ps.backgroundNote.empty()) g += L"  —  " + Widen(ps.backgroundNote);
            SetText(ID_NOTE_BASE + ID_BG_ENABLE, g.c_str());
        }
        std::string err = bridge.CameraError();
        if (err != lastCamErr) {
            lastCamErr = err;
            SetText(ID_CAM_INFO + 1, Widen(err).c_str());
        }
    }

    void UpdatePreview() {
        bridge.Pipeline().RequestPreview();
        uint64_t fn = 0;
        if (!bridge.Pipeline().GetPreview(frameNv12, fn) || frameNv12.size() < size_t(1920) * 1080 * 3 / 2 ||
            fn == previewFrame)
            return;
        previewFrame = fn;
        havePreview = true;
        RenderPreview();
    }

    // Limites [0, n] divididos em w partes iguais (caixas de média do modo Ajustar).
    static void FillBounds(std::vector<int>& b, int w, int n) {
        b.resize(size_t(w) + 1);
        for (int i = 0; i <= w; i++) b[size_t(i)] = int(int64_t(i) * n / w);
    }

    static void Yuv(uint8_t* o, int y, int u, int v) {
        int c = y - 16, d = u - 128, e = v - 128;
        o[0] = Clamp8((298 * c + 516 * d + 128) >> 8);
        o[1] = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
        o[2] = Clamp8((298 * c + 409 * e + 128) >> 8);
        o[3] = 255;
    }

    // Monta o buffer BGRA exatamente do tamanho da área da prévia (pixels de tela), para ser copiado 1:1.
    void RenderPreview() {
        if (!havePreview || frameNv12.size() < size_t(1920) * 1080 * 3 / 2) return;
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        const int W = 1920, H = 1080;
        int w = previewRc.right - previewRc.left, h = previewRc.bottom - previewRc.top;
        if (w <= 0 || h <= 0) return;
        if (w != bufW || h != bufH) {  // só realoca quando o tamanho muda
            bufW = w;
            bufH = h;
            previewBgra.resize(size_t(w) * h * 4);
            FillBounds(xb, w, W);
            FillBounds(yb, h, H);
        }
        const uint8_t* yp = frameNv12.data();
        const uint8_t* uv = yp + size_t(W) * H;
        if (zoom == 0) {
            // média por área (filtro de caixa) em Y e na crominância
            for (int y = 0; y < h; y++) {
                int sy0 = yb[size_t(y)], sy1 = std::max(sy0 + 1, yb[size_t(y) + 1]);
                int cy0 = sy0 >> 1, cy1 = std::min(H / 2, std::max(cy0 + 1, (sy1 + 1) >> 1));
                uint8_t* o = &previewBgra[size_t(y) * w * 4];
                for (int x = 0; x < w; x++, o += 4) {
                    int sx0 = xb[size_t(x)], sx1 = std::max(sx0 + 1, xb[size_t(x) + 1]);
                    int cx0 = sx0 >> 1, cx1 = std::min(W / 2, std::max(cx0 + 1, (sx1 + 1) >> 1));
                    unsigned ys = 0;
                    for (int j = sy0; j < sy1; j++) {
                        const uint8_t* r = yp + size_t(j) * W;
                        for (int i = sx0; i < sx1; i++) ys += r[i];
                    }
                    unsigned us = 0, vs = 0;
                    for (int j = cy0; j < cy1; j++) {
                        const uint8_t* r = uv + size_t(j) * W;
                        for (int i = cx0; i < cx1; i++) {
                            us += r[i * 2];
                            vs += r[i * 2 + 1];
                        }
                    }
                    unsigned ny = unsigned(sy1 - sy0) * unsigned(sx1 - sx0), nc = unsigned(cy1 - cy0) * unsigned(cx1 - cx0);
                    Yuv(o, int((ys + ny / 2) / ny), int((us + nc / 2) / nc), int((vs + nc / 2) / nc));
                }
            }
        } else {
            // vizinho mais próximo: pixels nítidos para inspecionar detalhes
            int ox, oy;
            ViewOrigin(ox, oy);
            for (int y = 0; y < h; y++) {
                int sy = std::min(H - 1, oy + y / zoom);
                const uint8_t* yr = yp + size_t(sy) * W;
                const uint8_t* ur = uv + size_t(sy >> 1) * W;
                uint8_t* o = &previewBgra[size_t(y) * w * 4];
                for (int x = 0; x < w; x++, o += 4) {
                    int sx = std::min(W - 1, ox + x / zoom);
                    Yuv(o, yr[sx], ur[sx & ~1], ur[(sx & ~1) + 1]);
                }
            }
        }
        InvalidateRect(panel, &previewRc, FALSE);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart);
        perfSum += ms;
        perfMax = std::max(perfMax, ms);
        if (++perfCount == 60) {  // medição única: custo médio/máximo de 60 atualizações
            Log("prévia %dx%d (zoom %d): média %.2f ms, máximo %.2f ms por atualização", w, h, zoom, perfSum / 60,
                perfMax);
        }
    }

    // Tamanho da janela de visão (em pixels do vídeo) para o zoom atual.
    void ViewSize(int& vw, int& vh) const {
        int w = previewRc.right - previewRc.left, h = previewRc.bottom - previewRc.top;
        vw = std::min(1920, (w + zoom - 1) / zoom);
        vh = std::min(1080, (h + zoom - 1) / zoom);
    }

    // Origem da janela de visão, limitada para nunca sair da imagem.
    void ViewOrigin(int& ox, int& oy) {
        int vw, vh;
        ViewSize(vw, vh);
        viewX = std::clamp(viewX, 0.0, double(1920 - vw));
        viewY = std::clamp(viewY, 0.0, double(1080 - vh));
        ox = int(viewX);
        oy = int(viewY);
    }

    // Muda o nível de zoom mantendo fixo o ponto da imagem sob (cx, cy), em coordenadas de cliente.
    void SetZoom(int level, int cx, int cy) {
        level = std::clamp(level, 0, 3);
        if (level != zoom) {
            int w = previewRc.right - previewRc.left, h = previewRc.bottom - previewRc.top;
            double px = double(std::clamp(cx - int(previewRc.left), 0, w)), py = double(std::clamp(cy - int(previewRc.top), 0, h));
            double srcX, srcY;
            if (zoom == 0) {
                srcX = px * 1920.0 / w;
                srcY = py * 1080.0 / h;
            } else {
                srcX = viewX + px / zoom;
                srcY = viewY + py / zoom;
            }
            zoom = level;
            if (zoom > 0) {
                viewX = srcX - px / zoom;
                viewY = srcY - py / zoom;
            }
            dragging = false;
            updating = true;
            SendMessageW(Child(ID_ZOOM), CB_SETCURSEL, WPARAM(zoom), 0);
            updating = false;
        }
        RenderPreview();
    }

    bool InPreview(int x, int y) const {
        POINT p{x, y};
        return PtInRect(&previewRc, p) != 0;
    }

    void PaintPreview(HDC dc) {
        if (havePreview && bufW > 0) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = bufW;
            bi.bmiHeader.biHeight = -bufH;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            // cópia 1:1 (o buffer já está no tamanho da tela)
            SetDIBitsToDevice(dc, previewRc.left, previewRc.top, DWORD(bufW), DWORD(bufH), 0, 0, 0, UINT(bufH),
                              previewBgra.data(), &bi, DIB_RGB_COLORS);
        } else {
            HBRUSH br = CreateSolidBrush(RGB(32, 34, 38));
            FillRect(dc, &previewRc, br);
            DeleteObject(br);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(190, 195, 202));
            HGDIOBJ old = SelectObject(dc, font);
            RECT t = previewRc;
            DrawTextW(dc, L"Aguardando imagem da câmera...", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        FrameRect(dc, &previewRc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    }

    void PaintSwatch(HDC dc) {
        Settings s = store.Get();
        HBRUSH br = CreateSolidBrush(RGB((s.bg.color >> 16) & 0xFF, (s.bg.color >> 8) & 0xFF, s.bg.color & 0xFF));
        FillRect(dc, &swatchRc, br);
        DeleteObject(br);
        FrameRect(dc, &swatchRc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    }

    // ---------------------------------------------------------------- ciclo de vida do painel
    void ShowPanel() {
        if (!panel) CreatePanel();
        if (!panel) return;
        // EOSCAM_PANEL_NOACTIVATE=1 (testes automatizados): mostra a janela sem roubar o foco
        wchar_t na[4];
        bool noActivate = GetEnvironmentVariableW(L"EOSCAM_PANEL_NOACTIVATE", na, 4) > 0 && na[0] == L'1';
        if (noActivate) {
            ShowWindow(panel, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(panel, IsIconic(panel) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(panel);
        }
        bridge.SetPanelOpen(true);
        SetTimer(panel, kTimerPreview, 66, nullptr);
        SetTimer(panel, kTimerStatus, 250, nullptr);
        SyncFromSettings();
        seenCamGen = ~0u;
        previewFrame = 0;
    }

    void HidePanel() {
        if (!panel) return;
        RECT r;
        if (GetWindowRect(panel, &r) && !IsIconic(panel))
            store.Update([&](Settings& s) { s.ui.x = r.left; s.ui.y = r.top; });
        KillTimer(panel, kTimerPreview);
        KillTimer(panel, kTimerStatus);
        ShowWindow(panel, SW_HIDE);
        bridge.SetPanelOpen(false);  // volta à regra normal: o Live View desliga 5 s depois de ninguém usar
    }

    void CreatePanel() {
        NONCLIENTMETRICSW ncm{sizeof ncm};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        font = CreateFontIndirectW(&ncm.lfMessageFont);

        const int clientW = 12 + 640 + 12 + 440 + 12, clientH = 12 + 360 + 30 + 8 + 104 + 8 + 134 + 8 + 22;
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rc{0, 0, S(clientW), S(clientH)};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, UINT(dpi));
        Settings s = store.Get();
        int x = s.ui.x, y = s.ui.y;
        if (x < -2000 || y < -2000 || x > 20000 || y > 20000) x = y = CW_USEDEFAULT;
        if (x == -1 && y == -1) x = y = CW_USEDEFAULT;
        panel = CreateWindowExW(0, kPanelClass, L"eoscam — painel de controle", style, x, y, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, inst, this);
        if (!panel) return;
        if (iconBig) SendMessageW(panel, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
        if (iconSmall) SendMessageW(panel, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
        BuildControls();
        SyncFromSettings();
    }

    // ---------------------------------------------------------------- procedimentos de janela
    static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Impl* d = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            d->panel = h;
        }
        return d ? d->OnPanelMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    LRESULT OnPanelMessage(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_HSCROLL: {
                HWND ctl = reinterpret_cast<HWND>(l);
                int id = GetDlgCtrlID(ctl);
                if (updating) return 0;
                if (id >= ID_IMG_BRIGHT && id <= ID_IMG_SAT) OnImageSlider(id);
                else if (id == ID_BG_BLUR) OnImageSlider(id);
                else if (id >= ID_CAM_KELVIN && id <= ID_CAM_TONE) OnCameraSlider(id, LOWORD(w));
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(w), code = HIWORD(w);
                if (updating) return 0;
                if (code == CBN_SELCHANGE) {
                    if (id == ID_ZOOM) {
                        int w0 = previewRc.right - previewRc.left, h0 = previewRc.bottom - previewRc.top;
                        // zoom pelo combo: mantém o centro da prévia fixo
                        SetZoom(int(SendMessageW(Child(ID_ZOOM), CB_GETCURSEL, 0, 0)), previewRc.left + w0 / 2,
                                previewRc.top + h0 / 2);
                    } else if (id == ID_FX_SR || id == ID_FX_AR || id == ID_FX_DN) OnFxCombo(id);
                    else if (id == ID_BG_MODE) OnBackground(id);
                    else OnCameraCombo(id);
                } else if (code == BN_CLICKED) {
                    if (id == ID_IMG_RESET) ResetImage();
                    else if (id == ID_BG_ENABLE) OnBackground(id);
                    else if (id == ID_BG_COLOR) PickColor();
                    else if (id == ID_BG_IMAGE) PickImage();
                }
                return 0;
            }
            case WM_TIMER:
                if (w == kTimerPreview) {
                    if (!IsIconic(h)) UpdatePreview();
                } else if (w == kTimerStatus) {
                    BridgeStatus bs = bridge.Status();
                    UpdateStatus(bs);
                    if (store.Version() != seenVersion) SyncFromSettings();
                    uint32_t gen = bridge.CameraPropsGeneration();
                    if (gen != seenCamGen || bs.cameraConnected != camWasConnected) {
                        seenCamGen = gen;
                        camWasConnected = bs.cameraConnected;
                        RefreshCamera(bs);
                    }
                }
                return 0;
            case WM_MOUSEWHEEL: {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(h, &p);
                if (!InPreview(p.x, p.y)) break;
                int delta = GET_WHEEL_DELTA_WPARAM(w);
                if (delta != 0) SetZoom(zoom + (delta > 0 ? 1 : -1), p.x, p.y);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
                if (zoom > 0 && InPreview(x, y)) {
                    dragging = true;
                    dragStart = {x, y};
                    dragViewX = viewX;
                    dragViewY = viewY;
                    SetCapture(h);
                }
                return 0;
            }
            case WM_MOUSEMOVE:
                if (dragging && zoom > 0) {
                    viewX = dragViewX - double(GET_X_LPARAM(l) - dragStart.x) / zoom;
                    viewY = dragViewY - double(GET_Y_LPARAM(l) - dragStart.y) / zoom;
                    RenderPreview();
                }
                return 0;
            case WM_LBUTTONUP:
            case WM_CAPTURECHANGED:
                if (dragging) {
                    dragging = false;
                    if (m == WM_LBUTTONUP) ReleaseCapture();
                }
                return 0;
            case WM_LBUTTONDBLCLK:
                if (InPreview(GET_X_LPARAM(l), GET_Y_LPARAM(l))) SetZoom(0, 0, 0);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(l) == HTCLIENT && zoom > 0) {
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(h, &p);
                    if (dragging || InPreview(p.x, p.y)) {
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                        return TRUE;
                    }
                }
                break;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                PaintPreview(dc);
                PaintSwatch(dc);
                EndPaint(h, &ps);
                return 0;
            }
            case WM_PRINTCLIENT:  // PrintWindow: captura de tela do painel
                PaintPreview(reinterpret_cast<HDC>(w));
                PaintSwatch(reinterpret_cast<HDC>(w));
                return 0;
            case WM_CTLCOLORSTATIC: {
                HWND c = reinterpret_cast<HWND>(l);
                if (GetDlgCtrlID(c) == ID_ZOOM_HINT) {
                    SetTextColor(reinterpret_cast<HDC>(w), RGB(120, 124, 130));
                    SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
                    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
                }
                if (GetDlgCtrlID(c) == ID_CAM_INFO + 1) {
                    SetTextColor(reinterpret_cast<HDC>(w), RGB(180, 30, 30));
                }
                break;
            }
            case WM_CLOSE:
                HidePanel();
                return 0;
            case WM_DESTROY:
                panel = nullptr;
                return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    // Duplo clique num controle deslizante o devolve ao valor padrão (o controle do Windows não repassa o
    // WM_LBUTTONDBLCLK, então o duplo clique é detectado pelo intervalo entre dois cliques).
    static LRESULT CALLBACK TrackProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
        static DWORD lastTick = 0;
        static UINT_PTR lastId = 0;
        if (m == WM_LBUTTONDOWN) {
            DWORD now = GetTickCount();
            bool dbl = lastId == id && now - lastTick <= GetDoubleClickTime();
            lastTick = now;
            lastId = id;
            if (dbl) {
                lastTick = 0;
                Impl* d = reinterpret_cast<Impl*>(ref);
                int sid = int(id);
                if (sid <= ID_IMG_SAT || sid == ID_BG_BLUR) {
                    d->ResetSlider(sid);
                    return 0;
                }
            }
        } else if (m == WM_NCDESTROY) {
            RemoveWindowSubclass(h, TrackProc, id);
        }
        return DefSubclassProc(h, m, w, l);
    }

    static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Impl* d = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        }
        return d ? d->OnTrayMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    void AddTrayIcon() {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = trayWnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kMsgTray;
        nid.hIcon = iconSmall;
        wcscpy_s(nid.szTip, L"eoscam — Canon EOS como webcam");
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
    void RemoveTrayIcon() {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = trayWnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void TrayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Abrir painel");
        AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"Iniciar com o Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Sair");
        SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(trayWnd);  // necessário para o menu fechar ao clicar fora
        UINT cmd = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, trayWnd, nullptr));
        DestroyMenu(menu);
        PostMessageW(trayWnd, WM_NULL, 0, 0);
        if (cmd == ID_TRAY_OPEN) {
            ShowPanel();
        } else if (cmd == ID_TRAY_AUTOSTART) {
            LSTATUS r = SetAutostart(!AutostartEnabled());
            if (r != ERROR_SUCCESS) Log("não consegui alterar \"Iniciar com o Windows\" (erro %ld)", (long)r);
        } else if (cmd == ID_TRAY_EXIT) {
            Log("Saindo pelo menu da bandeja.");
            bridge.RequestStop();
            PostQuitMessage(0);
        }
    }

    LRESULT OnTrayMessage(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == kMsgTray) {
            switch (LOWORD(l)) {
                case WM_LBUTTONDBLCLK: ShowPanel(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: TrayMenu(); break;
            }
            return 0;
        }
        if (m == kMsgOpenPanel) {
            ShowPanel();
            return 0;
        }
        if (m == taskbarCreated && taskbarCreated) {  // o Explorer reiniciou: recoloca o ícone
            AddTrayIcon();
            return 0;
        }
        if (m == WM_DESTROY) {
            RemoveTrayIcon();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

PanelApp::PanelApp(Bridge& bridge, SettingsStore& store) : d_(std::make_unique<Impl>(bridge, store)) {}

PanelApp::~PanelApp() = default;

int PanelApp::Run() {
    Impl& d = *d_;
    d.uiThread = GetCurrentThreadId();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    d.dpi = int(GetDpiForSystem());
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    d.iconBig = MakeAppIcon(32);
    d.iconSmall = MakeAppIcon(16);
    d.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = Impl::TrayProc;
    wc.hInstance = d.inst;
    wc.lpszClassName = kBridgeWindowClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW pc{sizeof pc};
    pc.style = CS_DBLCLKS;  // duplo clique na prévia volta ao modo Ajustar
    pc.lpfnWndProc = Impl::PanelProc;
    pc.hInstance = d.inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    pc.lpszClassName = kPanelClass;
    pc.hIcon = d.iconBig;
    pc.hIconSm = d.iconSmall;
    RegisterClassExW(&pc);

    // janela oculta (de nível superior, para o "eoscam panel" achá-la com FindWindow) que recebe a bandeja
    d.trayWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kBridgeWindowClass, L"eoscam", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                d.inst, &d);
    if (!d.trayWnd) {
        Log("painel: não consegui criar a janela da bandeja (erro %lu)", GetLastError());
        return 1;
    }
    d.AddTrayIcon();
    Log("Painel: ícone na bandeja criado (\"eoscam panel\" abre a janela).");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (d.panel && IsDialogMessageW(d.panel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    d.bridge.SetPanelOpen(false);
    if (d.panel) {
        d.HidePanel();
        DestroyWindow(d.panel);
    }
    DestroyWindow(d.trayWnd);
    if (d.font) DeleteObject(d.font);
    if (d.iconBig) DestroyIcon(d.iconBig);
    if (d.iconSmall) DestroyIcon(d.iconSmall);
    return 0;
}

void PanelApp::RequestQuit() {
    if (d_->uiThread) PostThreadMessageW(d_->uiThread, WM_QUIT, 0, 0);
}

}  // namespace eoscam
