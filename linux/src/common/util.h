// Small helpers shared by the service and the panel.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace canon_eos_webcam {

inline uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

inline double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::string Env(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

inline bool EnvFlag(const char* name) { return Env(name) == "1"; }

// $XDG_CONFIG_HOME/canon-eos-webcam/settings.ini, or the file in CANON_EOS_WEBCAM_SETTINGS.
inline std::string SettingsPath() {
    std::string p = Env("CANON_EOS_WEBCAM_SETTINGS");
    if (!p.empty()) return p;
    std::string base = Env("XDG_CONFIG_HOME");
    if (base.empty()) base = Env("HOME") + "/.config";
    return base + "/canon-eos-webcam/settings.ini";
}

// $XDG_RUNTIME_DIR/canon-eos-webcam.sock (CANON_EOS_WEBCAM_SOCKET overrides it, for tests).
inline std::string SocketPath() {
    std::string p = Env("CANON_EOS_WEBCAM_SOCKET");
    if (!p.empty()) return p;
    std::string dir = Env("XDG_RUNTIME_DIR");
    if (dir.empty()) dir = "/tmp";
    return dir + "/canon-eos-webcam.sock";
}

}  // namespace canon_eos_webcam
