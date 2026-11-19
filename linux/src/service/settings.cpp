#include "settings.h"

#include "../common/util.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <sys/inotify.h>
#include <unistd.h>

namespace canon_eos_webcam {

namespace {

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const char* FormatName(OutFormat f) { return f == OutFormat::I420 ? "i420" : f == OutFormat::Nv12 ? "nv12" : "yuy2"; }

Settings SettingsStore::FromDoc(const IniDoc& d) {
    Settings s;
    s.image.brightness = ClampInt(d.GetInt("image", "brightness", 0), -100, 100);
    s.image.contrast = ClampInt(d.GetInt("image", "contrast", 0), -100, 100);
    s.image.gamma = float(std::max(0.5, std::min(2.0, d.GetDouble("image", "gamma", 1.0))));
    s.image.temperature = ClampInt(d.GetInt("image", "temperature", 0), -100, 100);
    s.image.tint = ClampInt(d.GetInt("image", "tint", 0), -100, 100);
    s.image.saturation = ClampInt(d.GetInt("image", "saturation", 100), 0, 200);
    std::string crop = Lower(d.Get("linux", "crop", "bottom"));
    s.crop = crop == "center" ? CropAnchor::Center : crop == "top" ? CropAnchor::Top : CropAnchor::Bottom;
    std::string fmt = Lower(d.Get("linux", "format", "yuy2"));
    s.format = fmt == "i420" ? OutFormat::I420 : fmt == "nv12" ? OutFormat::Nv12 : OutFormat::Yuy2;
    s.upscale = Lower(d.Get("linux", "upscale", "fsr")) == "bilinear" ? Upscale::Bilinear : Upscale::Fsr;
    s.sharpness = float(std::max(0.0, std::min(2.0, d.GetDouble("linux", "sharpness", 0.2))));
    s.debugLog = Lower(d.Get("linux", "loglevel", "info")) == "debug";
    return s;
}

void SettingsStore::ToDoc(const Settings& s) {
    doc_.Set("image", "brightness", std::to_string(s.image.brightness));
    doc_.Set("image", "contrast", std::to_string(s.image.contrast));
    char g[32];
    snprintf(g, sizeof g, "%.2f", double(s.image.gamma));
    doc_.Set("image", "gamma", g);
    doc_.Set("image", "temperature", std::to_string(s.image.temperature));
    doc_.Set("image", "tint", std::to_string(s.image.tint));
    doc_.Set("image", "saturation", std::to_string(s.image.saturation));
    const char* crop = s.crop == CropAnchor::Center ? "center" : s.crop == CropAnchor::Top ? "top" : "bottom";
    doc_.Set("linux", "crop", crop);
    doc_.Set("linux", "format", FormatName(s.format));
    doc_.Set("linux", "upscale", s.upscale == Upscale::Bilinear ? "bilinear" : "fsr");
    char sh[32];
    snprintf(sh, sizeof sh, "%.2f", double(s.sharpness));
    doc_.Set("linux", "sharpness", sh);
}

SettingsStore::~SettingsStore() {
    FlushIfDue(true);
    if (inotifyFd_ >= 0) close(inotifyFd_);
}

void SettingsStore::Load(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    std::string text;
    if (ReadTextFile(path, text)) {
        doc_.Parse(text);
        cur_ = FromDoc(doc_);
        lastContent_ = text;
    } else {
        doc_.Parse("");
        cur_ = Settings{};
    }
    version_++;
}

int SettingsStore::StartWatching() {
    if (inotifyFd_ >= 0) return inotifyFd_;
    inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ < 0) return -1;
    std::string dir = path_.substr(0, path_.find_last_of('/'));
    if (dir.empty()) dir = ".";
    // the directory may not exist yet (first run): create it so the watch can be placed
    WriteFileAtomic(dir + "/.keep", "");
    unlink((dir + "/.keep").c_str());
    if (inotify_add_watch(inotifyFd_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
        Log("settings: couldn't watch %s for changes", dir.c_str());
        close(inotifyFd_);
        inotifyFd_ = -1;
    }
    return inotifyFd_;
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
    std::lock_guard<std::mutex> lk(mu_);
    Settings before = cur_;
    fn(cur_);
    (void)before;
    version_++;
    dirty_ = true;
    dirtySince_ = NowMs();
}

void SettingsStore::OnInotify() {
    char buf[4096];
    bool relevant = false;
    const std::string base = path_.substr(path_.find_last_of('/') + 1);
    for (;;) {
        ssize_t n = read(inotifyFd_, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            if (ev->len && base == ev->name) relevant = true;
            p += sizeof(inotify_event) + ev->len;
        }
    }
    if (!relevant) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (dirty_) return;  // a change of our own is pending: it wins (and the write overwrites the file)
    std::string text;
    if (!ReadTextFile(path_, text) || text == lastContent_) return;  // also ignores our own writes
    lastContent_ = text;
    doc_.Parse(text);
    Settings s = FromDoc(doc_);
    if (!(s.image == cur_.image) || s.crop != cur_.crop || s.format != cur_.format ||
        s.upscale != cur_.upscale || s.sharpness != cur_.sharpness) {
        cur_ = s;
        version_++;
        Log("settings: reloaded %s", path_.c_str());
    } else {
        cur_ = s;
    }
}

int SettingsStore::MsUntilFlush() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dirty_) return -1;
    int64_t left = int64_t(dirtySince_) + 500 - int64_t(NowMs());
    return left < 0 ? 0 : int(left);
}

void SettingsStore::FlushIfDue(bool force) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!dirty_) return;
    if (!force && NowMs() - dirtySince_ < 500) return;
    dirty_ = false;
    ToDoc(cur_);
    std::string text = doc_.Serialize();
    if (text == lastContent_) return;
    if (WriteFileAtomic(path_, text)) lastContent_ = text;
    else Log("settings: couldn't write %s", path_.c_str());
}

}  // namespace canon_eos_webcam
