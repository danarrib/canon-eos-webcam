// User settings, persisted in $XDG_CONFIG_HOME/canon-eos-webcam/settings.ini (or CANON_EOS_WEBCAM_SETTINGS).
//
// One object shared between the control thread (which changes it) and the processing thread (which reads it
// once per frame). Each change bumps a version counter and schedules the write with a ~500 ms debounce.
// Changes made straight to the file are picked up through inotify on its directory. The file is kept as an
// IniDoc, so sections this program doesn't own ([effects], [background], [ui]...) survive the writes.
#pragma once

#include "../common/ini.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace canon_eos_webcam {

struct ImageSettings {
    int brightness = 0;    // -100..+100
    int contrast = 0;      // -100..+100
    float gamma = 1.0f;    // 0.5..2.0
    int temperature = 0;   // -100..+100 (negative = cool/bluish, positive = warm/orange)
    int tint = 0;          // -100..+100 (negative = green, positive = magenta)
    int saturation = 100;  // 0..200 (%)

    bool IsIdentity() const {
        return brightness == 0 && contrast == 0 && gamma == 1.0f && temperature == 0 && tint == 0 &&
               saturation == 100;
    }
    bool operator==(const ImageSettings& o) const {
        return brightness == o.brightness && contrast == o.contrast && gamma == o.gamma &&
               temperature == o.temperature && tint == o.tint && saturation == o.saturation;
    }
};

enum class CropAnchor : int { Bottom = 0, Center = 1, Top = 2 };
enum class OutFormat : int { Yuy2 = 0, I420 = 1, Nv12 = 2 };
enum class Upscale : int { Fsr = 0, Bilinear = 1 };

struct Settings {
    ImageSettings image;
    CropAnchor crop = CropAnchor::Bottom;  // [linux] crop (live)
    OutFormat format = OutFormat::Yuy2;    // [linux] format (read at startup)
    Upscale upscale = Upscale::Fsr;        // [linux] upscale = fsr | bilinear (live)
    float sharpness = 0.2f;                // [linux] sharpness: FSR RCAS, in stops, 0.0 (sharpest)..2.0 (live)
    bool debugLog = false;                 // [linux] loglevel = debug
};

const char* FormatName(OutFormat f);

class SettingsStore {
public:
    SettingsStore() = default;
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // Reads the file (missing or invalid values keep their defaults).
    void Load(const std::string& path);
    // Starts watching the directory (control thread). Returns the inotify fd for poll(), or -1.
    int StartWatching();

    Settings Get(uint32_t* version = nullptr) const;
    uint32_t Version() const;
    // Applies a change, bumps the version and schedules the write.
    void Update(const std::function<void(Settings&)>& fn);

    // Control thread: call when the inotify fd is readable.
    void OnInotify();
    // Milliseconds until the pending write is due (-1 if nothing is pending).
    int MsUntilFlush() const;
    // Writes if the debounce expired (or always, with force).
    void FlushIfDue(bool force = false);

    const std::string& Path() const { return path_; }

private:
    static Settings FromDoc(const IniDoc& d);
    void ToDoc(const Settings& s);

    mutable std::mutex mu_;
    Settings cur_;
    uint32_t version_ = 1;
    std::string path_;
    IniDoc doc_;
    std::string lastContent_;   // what the file contained when we last read or wrote it
    bool dirty_ = false;
    uint64_t dirtySince_ = 0;
    int inotifyFd_ = -1;
};

}  // namespace canon_eos_webcam
