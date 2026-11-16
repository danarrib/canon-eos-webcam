// User settings (image adjustments, AI effects, virtual background and panel window), persisted in
// %APPDATA%\EosCam\settings.ini (or in the file pointed to by EOSCAM_SETTINGS).
//
// A single object shared between the panel thread (which changes it) and the processing thread (which
// reads it once per frame). Each change bumps a version counter and schedules the write to disk with a
// ~500 ms debounce. Changes made straight to the file (by hand, for instance) are reloaded automatically,
// which makes testing without the panel easy.
//
// The [ui] language key (auto|en|pt) is read only at startup, by InitLang() (i18n.h); this class leaves it
// untouched in the file.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace eoscam {

enum class FxLevel : int { Off = 0, Con = 1, Agg = 2 };
enum class BgMode : int { Solid = 0, Blur = 1, Image = 2 };

// Image adjustments (applied to the camera image, before compositing with the background).
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

struct EffectSettings {
    FxLevel superRes = FxLevel::Off;
    FxLevel artifact = FxLevel::Off;
    FxLevel denoise = FxLevel::Off;
    bool operator==(const EffectSettings& o) const {
        return superRes == o.superRes && artifact == o.artifact && denoise == o.denoise;
    }
};

struct BackgroundSettings {
    bool greenScreen = false;
    BgMode mode = BgMode::Blur;
    uint32_t color = 0x00FF00;  // 0xRRGGBB
    int blur = 60;              // 0..100
    std::wstring imagePath;
    bool operator==(const BackgroundSettings& o) const {
        return greenScreen == o.greenScreen && mode == o.mode && color == o.color && blur == o.blur &&
               imagePath == o.imagePath;
    }
};

struct UiSettings {
    int x = -1, y = -1;     // position of the panel window (-1 = Windows default)
};

struct Settings {
    ImageSettings image;
    EffectSettings fx;
    BackgroundSettings bg;
    UiSettings ui;
};

class SettingsStore {
public:
    SettingsStore();
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // File path: EOSCAM_SETTINGS, otherwise %APPDATA%\EosCam\settings.ini.
    static std::wstring DefaultPath();

    // Reads the file (missing or invalid values keep their defaults) and starts the writer thread.
    void Load(const std::wstring& path);

    // Consistent copy of the settings and of the version counter.
    Settings Get(uint32_t* version = nullptr) const;
    uint32_t Version() const;

    // Applies a change (under the mutex), bumps the version and schedules the write.
    void Update(const std::function<void(Settings&)>& fn);

    // Reloads if the file changed on disk from the outside (call every now and then, e.g. once a second).
    void ReloadIfChangedOnDisk();

    // Writes now, if there is a pending change (used when shutting down).
    void FlushNow();

    const std::wstring& Path() const { return path_; }

private:
    void SaveLocked(const Settings& s);
    bool ReadFile(Settings& out) const;
    void WriterLoop();
    uint64_t FileStamp() const;

    mutable std::mutex mu_;
    Settings cur_;
    uint32_t version_ = 1;
    std::wstring path_;
    uint64_t lastStamp_ = 0;

    std::mutex wmu_;
    std::condition_variable wcv_;
    bool dirty_ = false;
    bool stop_ = false;
    uint64_t dirtySince_ = 0;
    std::thread writer_;
};

}  // namespace eoscam
