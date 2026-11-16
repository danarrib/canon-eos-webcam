// "Start with Windows": the EosCam value in HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
#pragma once

#include <windows.h>

#include <string>

namespace eoscam {

inline constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Folder of the current executable, with the trailing backslash.
inline std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L'\\') + 1);
}

inline bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKeyPath, L"EosCam", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

// Turns starting eoscam-bg with Windows on/off. Returns the registry error code (0 = ok).
inline LSTATUS SetAutostart(bool on) {
    if (on) {
        std::wstring value = L"\"" + ExeDirectory() + L"eoscam-bg.exe\"";
        return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, L"EosCam", REG_SZ, value.c_str(),
                               DWORD((value.size() + 1) * sizeof(wchar_t)));
    }
    LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, L"EosCam");
    return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : s;
}

}  // namespace eoscam
