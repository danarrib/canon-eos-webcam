#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace eoscam {

namespace {

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

int ReadInt(const wchar_t* sec, const wchar_t* key, int def, const wchar_t* path) {
    wchar_t b[64];
    GetPrivateProfileStringW(sec, key, L"", b, 64, path);
    if (!b[0]) return def;
    wchar_t* end = nullptr;
    long v = wcstol(b, &end, 10);
    return (end && end != b) ? int(v) : def;
}

float ReadFloat(const wchar_t* sec, const wchar_t* key, float def, const wchar_t* path) {
    wchar_t b[64];
    GetPrivateProfileStringW(sec, key, L"", b, 64, path);
    if (!b[0]) return def;
    for (wchar_t* c = b; *c; c++)
        if (*c == L',') *c = L'.';
    wchar_t* end = nullptr;
    double v = wcstod(b, &end);
    return (end && end != b) ? float(v) : def;
}

std::wstring ReadStr(const wchar_t* sec, const wchar_t* key, const wchar_t* path) {
    std::wstring s(1024, L'\0');
    DWORD n = GetPrivateProfileStringW(sec, key, L"", s.data(), DWORD(s.size()), path);
    s.resize(n);
    return s;
}

FxLevel ParseLevel(const std::wstring& v, FxLevel def) {
    if (_wcsicmp(v.c_str(), L"off") == 0 || v == L"0") return FxLevel::Off;
    if (_wcsicmp(v.c_str(), L"con") == 0 || v == L"1") return FxLevel::Con;
    if (_wcsicmp(v.c_str(), L"agg") == 0 || v == L"2") return FxLevel::Agg;
    return def;
}
const wchar_t* LevelName(FxLevel l) { return l == FxLevel::Con ? L"con" : l == FxLevel::Agg ? L"agg" : L"off"; }

BgMode ParseBgMode(const std::wstring& v, BgMode def) {
    if (_wcsicmp(v.c_str(), L"solid") == 0 || _wcsicmp(v.c_str(), L"cor") == 0) return BgMode::Solid;
    if (_wcsicmp(v.c_str(), L"blur") == 0 || _wcsicmp(v.c_str(), L"desfoque") == 0) return BgMode::Blur;
    if (_wcsicmp(v.c_str(), L"image") == 0 || _wcsicmp(v.c_str(), L"imagem") == 0) return BgMode::Image;
    return def;
}
const wchar_t* BgModeName(BgMode m) { return m == BgMode::Solid ? L"solid" : m == BgMode::Image ? L"image" : L"blur"; }

void WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v, const wchar_t* path) {
    WritePrivateProfileStringW(sec, key, v.c_str(), path);
}
void WriteInt(const wchar_t* sec, const wchar_t* key, int v, const wchar_t* path) {
    WriteStr(sec, key, std::to_wstring(v), path);
}

}  // namespace

std::wstring SettingsStore::DefaultPath() {
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"EOSCAM_SETTINGS", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return env;
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) dir = appdata;
    CoTaskMemFree(appdata);
    if (dir.empty()) dir = L".";
    return dir + L"\\EosCam\\settings.ini";
}

SettingsStore::SettingsStore() = default;

SettingsStore::~SettingsStore() {
    {
        std::lock_guard<std::mutex> lk(wmu_);
        stop_ = true;
    }
    wcv_.notify_all();
    if (writer_.joinable()) writer_.join();
    FlushNow();
}

uint64_t SettingsStore::FileStamp() const {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32 | fa.ftLastWriteTime.dwLowDateTime) ^
           (uint64_t(fa.nFileSizeLow) << 1);
}

bool SettingsStore::ReadFile(Settings& s) const {
    const wchar_t* p = path_.c_str();
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return false;
    s = Settings{};
    s.image.brightness = ClampInt(ReadInt(L"image", L"brightness", 0, p), -100, 100);
    s.image.contrast = ClampInt(ReadInt(L"image", L"contrast", 0, p), -100, 100);
    s.image.gamma = std::max(0.5f, std::min(2.0f, ReadFloat(L"image", L"gamma", 1.0f, p)));
    s.image.temperature = ClampInt(ReadInt(L"image", L"temperature", 0, p), -100, 100);
    s.image.tint = ClampInt(ReadInt(L"image", L"tint", 0, p), -100, 100);
    s.image.saturation = ClampInt(ReadInt(L"image", L"saturation", 100, p), 0, 200);

    s.fx.superRes = ParseLevel(ReadStr(L"effects", L"superres", p), FxLevel::Off);
    s.fx.artifact = ParseLevel(ReadStr(L"effects", L"artifact", p), FxLevel::Off);
    s.fx.denoise = ParseLevel(ReadStr(L"effects", L"denoise", p), FxLevel::Off);

    s.bg.greenScreen = ReadInt(L"background", L"greenscreen", 0, p) != 0;
    s.bg.mode = ParseBgMode(ReadStr(L"background", L"mode", p), BgMode::Blur);
    {
        std::wstring c = ReadStr(L"background", L"color", p);
        if (!c.empty()) {
            if (c[0] == L'#') c.erase(0, 1);
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(c.c_str(), &end, 16);
            if (end && end != c.c_str()) s.bg.color = uint32_t(v & 0xFFFFFF);
        }
    }
    s.bg.blur = ClampInt(ReadInt(L"background", L"blur", 60, p), 0, 100);
    s.bg.imagePath = ReadStr(L"background", L"image", p);

    s.ui.x = ReadInt(L"ui", L"x", -1, p);
    s.ui.y = ReadInt(L"ui", L"y", -1, p);
    return true;
}

void SettingsStore::Load(const std::wstring& path) {
    path_ = path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Settings s;
        if (ReadFile(s)) cur_ = s;
        lastStamp_ = FileStamp();
        version_++;
    }
    writer_ = std::thread([this] { WriterLoop(); });
}

Settings SettingsStore::Get(uint32_t* version) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (version) *version = version_;
    return cur_;
}

uint32_t SettingsStore::Version() const {
    std::lock_guard<std::mutex> lk(mu_);
    return version_;
}

void SettingsStore::Update(const std::function<void(Settings&)>& fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn(cur_);
        version_++;
    }
    {
        std::lock_guard<std::mutex> lk(wmu_);
        dirty_ = true;
        dirtySince_ = GetTickCount64();
    }
    wcv_.notify_all();
}

void SettingsStore::ReloadIfChangedOnDisk() {
    if (path_.empty()) return;
    {
        std::lock_guard<std::mutex> lk(wmu_);
        if (dirty_) return;  // há gravação nossa pendente: não sobrescreve
    }
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t st = FileStamp();
    if (st == 0 || st == lastStamp_) return;
    Settings s;
    if (ReadFile(s)) {
        cur_ = s;
        version_++;
    }
    lastStamp_ = st;
}

void SettingsStore::SaveLocked(const Settings& s) {
    const wchar_t* p = path_.c_str();
    // Garante a pasta e um arquivo UTF-16 (com BOM), para os caminhos com acentos sobreviverem.
    size_t slash = path_.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path_.substr(0, slash);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const unsigned char bom[2] = {0xFF, 0xFE};
            DWORD w = 0;
            WriteFile(h, bom, 2, &w, nullptr);
            CloseHandle(h);
        }
    }
    WriteInt(L"image", L"brightness", s.image.brightness, p);
    WriteInt(L"image", L"contrast", s.image.contrast, p);
    {
        wchar_t g[32];
        swprintf_s(g, L"%.2f", s.image.gamma);
        WriteStr(L"image", L"gamma", g, p);
    }
    WriteInt(L"image", L"temperature", s.image.temperature, p);
    WriteInt(L"image", L"tint", s.image.tint, p);
    WriteInt(L"image", L"saturation", s.image.saturation, p);

    WriteStr(L"effects", L"superres", LevelName(s.fx.superRes), p);
    WriteStr(L"effects", L"artifact", LevelName(s.fx.artifact), p);
    WriteStr(L"effects", L"denoise", LevelName(s.fx.denoise), p);

    WriteInt(L"background", L"greenscreen", s.bg.greenScreen ? 1 : 0, p);
    WriteStr(L"background", L"mode", BgModeName(s.bg.mode), p);
    {
        wchar_t c[16];
        swprintf_s(c, L"%06X", s.bg.color & 0xFFFFFF);
        WriteStr(L"background", L"color", c, p);
    }
    WriteInt(L"background", L"blur", s.bg.blur, p);
    WriteStr(L"background", L"image", s.bg.imagePath, p);

    WriteInt(L"ui", L"x", s.ui.x, p);
    WriteInt(L"ui", L"y", s.ui.y, p);
    lastStamp_ = FileStamp();
}

void SettingsStore::FlushNow() {
    bool d;
    {
        std::lock_guard<std::mutex> lk(wmu_);
        d = dirty_;
        dirty_ = false;
    }
    if (!d || path_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    SaveLocked(cur_);
}

void SettingsStore::WriterLoop() {
    std::unique_lock<std::mutex> lk(wmu_);
    while (!stop_) {
        if (!dirty_) {
            wcv_.wait(lk);
            continue;
        }
        // debounce: espera ~500 ms sem novas alterações
        uint64_t now = GetTickCount64();
        if (now - dirtySince_ < 500) {
            wcv_.wait_for(lk, std::chrono::milliseconds(500 - (now - dirtySince_) + 5));
            continue;
        }
        dirty_ = false;
        lk.unlock();
        {
            std::lock_guard<std::mutex> g(mu_);
            SaveLocked(cur_);
        }
        lk.lock();
    }
}

}  // namespace eoscam
