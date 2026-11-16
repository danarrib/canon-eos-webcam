// "Start with Windows": the CanonEosWebcam value in HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
#pragma once

#include <windows.h>

#include <string>

namespace canon_eos_webcam {

inline constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValue[] = L"CanonEosWebcam";
// Value written by the versions released under the old name (eoscam); removed whenever autostart changes.
inline constexpr wchar_t kLegacyRunValue[] = L"EosCam";

// Folder of the current executable, with the trailing backslash.
inline std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L'\\') + 1);
}

inline bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

// Command line stored in the Run key: the background bridge next to the current executable.
inline std::wstring AutostartCommand() { return L"\"" + ExeDirectory() + L"canon-eos-webcam-bg.exe\""; }

// Turns starting canon-eos-webcam-bg with Windows on/off. Returns the registry error code (0 = ok).
inline LSTATUS SetAutostart(bool on) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kLegacyRunValue);
    if (on) {
        std::wstring value = AutostartCommand();
        return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValue, REG_SZ, value.c_str(),
                               DWORD((value.size() + 1) * sizeof(wchar_t)));
    }
    LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKeyPath, kRunValue);
    return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : s;
}

}  // namespace canon_eos_webcam
