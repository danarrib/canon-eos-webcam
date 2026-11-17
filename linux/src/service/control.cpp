#include "control.h"

#include "../common/camera_values.h"
#include "../common/util.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace canon_eos_webcam {

namespace {

constexpr size_t kMaxLine = 64 * 1024;
constexpr size_t kBackedUp = 512 * 1024;  // output buffer size above which preview frames are dropped

std::string Hex(const std::vector<uint8_t>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}


// 1280x720 RGB -> width x (width*9/16) RGB (2x2 average for 640, nearest otherwise).
void Downsize(const std::vector<uint8_t>& src, int width, std::vector<uint8_t>& dst, int& w, int& h) {
    if (width >= kOutWidth) { dst = src; w = kOutWidth; h = kOutHeight; return; }
    w = width & ~1;
    h = (w * kOutHeight / kOutWidth) & ~1;
    dst.resize(size_t(w) * size_t(h) * 3);
    if (w == kOutWidth / 2) {
        for (int y = 0; y < h; y++) {
            const uint8_t* a = &src[size_t(y) * 2 * kOutWidth * 3];
            const uint8_t* b = a + kOutWidth * 3;
            uint8_t* o = &dst[size_t(y) * size_t(w) * 3];
            for (int x = 0; x < w; x++, o += 3, a += 6, b += 6)
                for (int k = 0; k < 3; k++) o[k] = uint8_t((a[k] + a[k + 3] + b[k] + b[k + 3] + 2) >> 2);
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        int sy = y * kOutHeight / h;
        for (int x = 0; x < w; x++) {
            int sx = x * kOutWidth / w;
            memcpy(&dst[(size_t(y) * size_t(w) + size_t(x)) * 3], &src[(size_t(sy) * kOutWidth + size_t(sx)) * 3], 3);
        }
    }
}

}  // namespace

Control::~Control() {
    for (Client& c : clients_) close(c.fd);
    if (listenFd_ >= 0) { close(listenFd_); unlink(path_.c_str()); }
    if (wakeFd_ >= 0) close(wakeFd_);
}

bool Control::Listen(std::string& error) {
    path_ = SocketPath();
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof addr.sun_path) { error = "socket path too long: " + path_; return false; }
    strcpy(addr.sun_path, path_.c_str());
    // is another service answering on it?
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
        close(probe);
        error = "another canon-eos-webcam service already owns " + path_;
        return false;
    }
    close(probe);
    unlink(path_.c_str());
    listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    mode_t old = umask(077);
    int r = bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    umask(old);
    if (r != 0 || listen(listenFd_, 8) != 0) {
        error = std::string("couldn't create the control socket ") + path_ + ": " + strerror(errno);
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    chmod(path_.c_str(), 0600);
    return true;
}

void Control::Wake() {
    if (wakeFd_ < 0) return;
    uint64_t one = 1;
    ssize_t r = write(wakeFd_, &one, sizeof one);
    (void)r;
}

void Control::Accept() {
    for (;;) {
        int fd = accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        Client c;
        c.id = nextId_++;
        c.fd = fd;
        clients_.push_back(std::move(c));
    }
}

Json Control::StatusJson() const {
    BridgeStats s = bridge_.Stats();
    Json j = Json::Object();
    j.Set("type", "status");
    j.Set("state", BridgeStateName(s.state));
    j.Set("camera", s.cameraConnected);
    j.Set("name", s.cameraName);
    j.Set("fps_in", std::round(s.fpsIn * 10) / 10);
    j.Set("fps_out", std::round(s.fpsOut * 10) / 10);
    j.Set("ms", std::round(s.msProcess * 10) / 10);
    j.Set("consumers", s.consumers);
    j.Set("panels", s.panels);
    j.Set("pipewire", pw_ ? pw_->Connected() : false);
    return j;
}

Json Control::ImageJson(uint32_t& version) const {
    Settings s = settings_.Get(&version);
    Json j = Json::Object();
    j.Set("type", "image");
    j.Set("brightness", s.image.brightness);
    j.Set("contrast", s.image.contrast);
    j.Set("gamma", std::round(double(s.image.gamma) * 100) / 100);
    j.Set("temperature", s.image.temperature);
    j.Set("tint", s.image.tint);
    j.Set("saturation", s.image.saturation);
    return j;
}

Json Control::CameraJson(const CameraSnapshot& snap) const {
    Json j = Json::Object();
    j.Set("type", "camera");
    j.Set("gen", snap.gen);
    j.Set("connected", snap.connected);
    j.Set("name", snap.name);
    Json props = Json::Object();
    for (auto& kv : snap.props) {
        char key[16];
        snprintf(key, sizeof key, "%04X", kv.first);
        Json p = Json::Object();
        if (kv.second.hasValue) {
            p.Set("value", kv.second.value);
            p.Set("raw", Hex(kv.second.raw));
        }
        if (kv.second.hasList) {
            Json l = Json::Array();
            for (uint32_t v : kv.second.list) l.Push(v);
            p.Set("list", std::move(l));
        }
        props.Set(key, std::move(p));
    }
    j.Set("props", std::move(props));
    return j;
}

void Control::Send(Client& c, const Json& j) {
    c.out += j.Dump();
    c.out += '\n';
}

void Control::Reply(Client& c, int64_t id, const std::string& error) {
    Json r = Json::Object();
    r.Set("type", "result");
    r.Set("id", id);
    r.Set("ok", error.empty());
    if (!error.empty()) r.Set("error", error);
    Send(c, r);
}

void Control::HandleSet(Client& c, const Json& msg) {
    int64_t id = int64_t(msg["id"].Num(0));
    std::string key = msg["key"].Str();
    // the value may arrive as a number or as text (names for the camera values)
    std::string text;
    double num = 0;
    bool isNum = msg["value"].IsNumber();
    if (isNum) { num = msg["value"].Num(); }
    else if (msg["value"].IsString()) { text = msg["value"].Str(); }
    else { Reply(c, id, "bad_value"); return; }

    if (key.rfind("image.", 0) == 0) {
        if (!isNum) {
            char* end = nullptr;
            std::string t = text;
            std::replace(t.begin(), t.end(), ',', '.');
            num = strtod(t.c_str(), &end);
            if (t.empty() || *end != 0) { Reply(c, id, "bad_value"); return; }
        }
        std::string name = key.substr(6);
        struct R { const char* n; double lo, hi; } ranges[] = {
            {"brightness", -100, 100}, {"contrast", -100, 100}, {"gamma", 0.5, 2.0},
            {"temperature", -100, 100}, {"tint", -100, 100}, {"saturation", 0, 200}};
        const R* r = nullptr;
        for (auto& x : ranges) if (name == x.n) r = &x;
        if (!r) { Reply(c, id, "bad_key"); return; }
        if (num < r->lo || num > r->hi) { Reply(c, id, "out_of_range"); return; }
        settings_.Update([&](Settings& s) {
            ImageSettings& i = s.image;
            if (name == "brightness") i.brightness = int(std::lround(num));
            else if (name == "contrast") i.contrast = int(std::lround(num));
            else if (name == "gamma") i.gamma = float(std::round(num * 100) / 100);
            else if (name == "temperature") i.temperature = int(std::lround(num));
            else if (name == "tint") i.tint = int(std::lround(num));
            else i.saturation = int(std::lround(num));
        });
        Reply(c, id, "");
        Wake();
        return;
    }

    if (key.rfind("cam.", 0) != 0) { Reply(c, id, "bad_key"); return; }
    uint32_t v;
    if (isNum) v = uint32_t(int32_t(std::lround(num)));
    else if (!ParseCamValue(key, text, v)) { Reply(c, id, "bad_value"); return; }

    CamCommand cmd;
    if (key == "cam.exp") { cmd.prop = kPropExpComp; cmd.value = v & 0xFF; }
    else if (key == "cam.iso") { cmd.prop = kPropISO; cmd.value = v; }
    else if (key == "cam.wb") { cmd.prop = kPropWhiteBalance; cmd.value = v; }
    else if (key == "cam.kelvin") {
        if (v < 2500 || v > 10000) { Reply(c, id, "out_of_range"); return; }
        cmd.prop = kPropColorTemp; cmd.value = v;
    }
    else if (key == "cam.style") { cmd.prop = kPropPictureStyle; cmd.value = v & 0xFF; }
    else if (key == "cam.alo") { cmd.prop = kPropAlo; cmd.value = v; }
    else if (key.rfind("cam.style.", 0) == 0) {
        std::string p = key.substr(10);
        int idx = p == "contrast" ? 0 : p == "sharpness" ? 1 : p == "saturation" ? 2 : p == "tone" ? 3 : -1;
        if (idx < 0) { Reply(c, id, "bad_key"); return; }
        int lo, hi;
        StyleParamRange(idx, lo, hi);
        int iv = int(int32_t(v));
        if (iv < lo || iv > hi) { Reply(c, id, "out_of_range"); return; }
        cmd.kind = CamCommand::Kind::StyleParam;
        cmd.index = idx;
        cmd.param = iv;
    } else {
        Reply(c, id, "bad_key");
        return;
    }
    int cid = c.id;
    bridge_.Submit(cmd, [this, cid, id](CmdResult r) {
        const char* e = r == CmdResult::Ok ? "" : r == CmdResult::Refused ? "refused"
                        : r == CmdResult::NotConnected ? "not_connected" : "failed";
        {
            std::lock_guard<std::mutex> lk(doneMu_);
            done_.push_back({cid, id, e});
        }
        Wake();
    });
}

void Control::HandleLine(Client& c, const std::string& line) {
    Json msg;
    if (!Json::Parse(line, msg) || !msg.IsObject()) return;
    std::string cmd = msg["cmd"].Str();
    if (cmd == "get") {
        uint32_t v;
        Send(c, StatusJson());
        Send(c, ImageJson(v));
        Send(c, CameraJson(bridge_.Camera()));
    } else if (cmd == "subscribe") {
        c.subscribed = true;
        c.preview = msg["preview"].Bool(false);
        c.previewSize = std::max(160, std::min(kOutWidth, msg["size"].Int(640)));
        c.sentCamValid = false;
        c.sentImageVersion = 0;
        c.lastStatusMs = 0;
        RefreshConsumers();
    } else if (cmd == "unsubscribe") {
        c.subscribed = false;
        c.preview = false;
        RefreshConsumers();
    } else if (cmd == "set") {
        HandleSet(c, msg);
    }
}

void Control::ReadClient(Client& c) {
    char buf[8192];
    for (;;) {
        ssize_t n = read(c.fd, buf, sizeof buf);
        if (n > 0) {
            c.in.append(buf, size_t(n));
            if (c.in.size() > kMaxLine * 2) { c.dead = true; return; }
            continue;
        }
        if (n == 0) c.dead = true;
        else if (errno != EAGAIN && errno != EINTR) c.dead = true;
        break;
    }
    size_t nl;
    while ((nl = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, nl);
        c.in.erase(0, nl + 1);
        if (!line.empty()) HandleLine(c, line);
    }
}

void Control::RefreshConsumers() {
    int panels = 0;
    bool preview = false;
    for (const Client& c : clients_) {
        if (c.dead || !c.subscribed) continue;
        panels++;
        preview = preview || c.preview;
    }
    bridge_.SetPanels(panels);
    bridge_.SetPreviewWanted(preview);
}

void Control::Broadcast() {
    uint64_t now = NowMs();
    CameraSnapshot cam;
    bool haveCam = false;
    for (Client& c : clients_) {
        if (!c.subscribed || c.dead) continue;
        uint32_t ver;
        Json img = ImageJson(ver);
        if (ver != c.sentImageVersion) {
            c.sentImageVersion = ver;
            Send(c, img);
        }
        if (!haveCam) { cam = bridge_.Camera(); haveCam = true; }
        if (!c.sentCamValid || c.sentCamGen != cam.gen || c.sentCamConnected != cam.connected) {
            c.sentCamValid = true;
            c.sentCamGen = cam.gen;
            c.sentCamConnected = cam.connected;
            Send(c, CameraJson(cam));
        }
        if (now - c.lastStatusMs >= 1000) {
            c.lastStatusMs = now;
            Send(c, StatusJson());
        }
    }
}

void Control::SendPreviews() {
    bool any = false;
    for (const Client& c : clients_) any = any || (c.subscribed && c.preview && !c.dead);
    if (!any) return;
    std::vector<uint8_t> rgb;
    uint64_t seq;
    if (!bridge_.TakePreview(rgb, seq)) return;
    std::map<int, std::string> encoded;  // by width
    for (Client& c : clients_) {
        if (!c.subscribed || !c.preview || c.dead) continue;
        if (c.out.size() > kBackedUp) { c.previewsDropped++; continue; }  // the panel isn't reading: drop
        auto it = encoded.find(c.previewSize);
        if (it == encoded.end()) {
            std::vector<uint8_t> small, jpeg;
            int w, h;
            Downsize(rgb, c.previewSize, small, w, h);
            std::string msg;
            if (EncodeJpegRgb(small.data(), w, h, c.previewSize > 700 ? 85 : 75, jpeg)) {
                Json j = Json::Object();
                j.Set("type", "preview");
                j.Set("w", w);
                j.Set("h", h);
                j.Set("jpeg", Base64Encode(jpeg.data(), jpeg.size()));
                msg = j.Dump() + "\n";
            }
            it = encoded.emplace(c.previewSize, std::move(msg)).first;
        }
        c.out += it->second;
    }
}

void Control::Run(const std::atomic<bool>& stop) {
    int inotifyFd = settings_.StartWatching();
    while (!stop) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakeFd_, POLLIN, 0});
        fds.push_back({inotifyFd, POLLIN, 0});  // a negative fd is ignored by poll
        for (Client& c : clients_) fds.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});

        bool anySub = false;
        for (const Client& c : clients_) anySub = anySub || c.subscribed;
        int timeout = anySub ? 250 : 2000;
        int flush = settings_.MsUntilFlush();
        if (flush >= 0) timeout = std::min(timeout, flush + 1);
        int r = poll(fds.data(), nfds_t(fds.size()), timeout);
        if (r < 0 && errno != EINTR) break;
        if (stop) break;

        if (fds[1].revents & POLLIN) { uint64_t v; ssize_t x = read(wakeFd_, &v, sizeof v); (void)x; }
        if (fds[2].revents & POLLIN) settings_.OnInotify();
        if (fds[0].revents & POLLIN) Accept();
        size_t nclients = fds.size() - 3;
        for (size_t i = 0; i < nclients && i < clients_.size(); i++) {
            if (fds[i + 3].revents & (POLLIN | POLLHUP | POLLERR)) ReadClient(clients_[i]);
        }
        settings_.FlushIfDue();

        // results of the camera commands
        {
            std::deque<Completed> d;
            { std::lock_guard<std::mutex> lk(doneMu_); d.swap(done_); }
            for (Completed& cp : d)
                for (Client& c : clients_)
                    if (c.id == cp.clientId && !c.dead) Reply(c, cp.reqId, cp.error);
        }
        Broadcast();
        SendPreviews();

        for (Client& c : clients_) {
            while (!c.out.empty() && !c.dead) {
                ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (n > 0) { c.out.erase(0, size_t(n)); continue; }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                c.dead = true;
            }
        }
        bool removed = false;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->dead) { close(it->fd); it = clients_.erase(it); removed = true; }
            else ++it;
        }
        if (removed) RefreshConsumers();
    }
    settings_.FlushIfDue(true);
}

}  // namespace canon_eos_webcam
