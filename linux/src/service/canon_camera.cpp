#include "canon_camera.h"

#include "../common/util.h"
#include "log.h"

#include <libusb.h>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace canon_eos_webcam {

namespace {

// While the camera focuses, the Live View may stall; the recovery leaves it alone for this long.
constexpr uint64_t kAutoFocusGraceMs = 5000;
// KeepDeviceOn counts as user activity for the camera (one of the two measures against the 30-minute limit).
constexpr uint64_t kKeepAliveMs = 30000;
// The camera ends each Live View session 30 min after it starts. Switching the Live View output away from the PC
// and back (~80 ms, no visible pause) restarts that timer. For experiments: CANON_EOS_WEBCAM_EVF_REFRESH_S
// (interval in seconds, 0 = off) and CANON_EOS_WEBCAM_EVF_ALT (the temporary EVFOutputDevice value).
constexpr uint32_t kEvfRefreshDefaultS = 300;
constexpr uint32_t kEvfAltDefault = 1;  // camera screen only: the PC output really goes away for an instant
// After a switch the frames may stall briefly; the recovery leaves the Live View alone for this long.
constexpr uint64_t kEvfRefreshGraceMs = 3000;

uint32_t EnvU32(const char* name, uint32_t def) {
    std::string v = Env(name);
    if (v.empty()) return def;
    char* end = nullptr;
    unsigned long x = strtoul(v.c_str(), &end, 0);
    return end && end != v.c_str() && *end == 0 ? uint32_t(x) : def;
}

enum : uint32_t {
    PTP_DPC_CANON_EOS_EVFOutputDevice = 0xD1B0,
    PTP_DPC_CANON_EOS_EVFMode = 0xD1B3,
};

uint32_t Rd32(const std::vector<uint8_t>& d, size_t i) {
    return uint32_t(d[i]) | uint32_t(d[i + 1]) << 8 | uint32_t(d[i + 2]) << 16 | uint32_t(d[i + 3]) << 24;
}
uint16_t Rd16(const std::vector<uint8_t>& d, size_t i) { return uint16_t(d[i] | d[i + 1] << 8); }
void Put32(std::vector<uint8_t>& d, size_t i, uint32_t v) {
    d[i] = uint8_t(v); d[i + 1] = uint8_t(v >> 8); d[i + 2] = uint8_t(v >> 16); d[i + 3] = uint8_t(v >> 24);
}

// Splits the GetViewFinderData blocks ([u32 size][u32 type][data]) and finds the JPEG (type 1).
bool ExtractJpeg(const std::vector<uint8_t>& d, size_t& off, size_t& len) {
    size_t i = 0;
    while (i + 8 <= d.size()) {
        uint32_t sz = Rd32(d, i), ty = Rd32(d, i + 4);
        if (sz < 8 || i + sz > d.size()) break;
        if (ty == 1 && sz > 10 && d[i + 8] == 0xFF && d[i + 9] == 0xD8) {
            off = i + 8;
            len = sz - 8;
            return true;
        }
        i += sz;
    }
    return false;
}

// PTP string: u8 character count (including the terminating NUL), then UCS-2 characters.
std::string PtpString(const std::vector<uint8_t>& d, size_t& i) {
    std::string s;
    if (i >= d.size()) return s;
    size_t n = d[i++];
    for (size_t k = 0; k < n && i + 1 < d.size(); k++, i += 2) {
        uint16_t c = Rd16(d, i);
        if (c == 0) continue;
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
        else { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    }
    return s;
}

void SkipArray16(const std::vector<uint8_t>& d, size_t& i) {
    if (i + 4 > d.size()) { i = d.size(); return; }
    uint32_t n = Rd32(d, i);
    i += 4 + size_t(n) * 2;
}

}  // namespace

void CanonCamera::ReadDeviceInfo() {
    // DeviceInfo dataset: StandardVersion u16, VendorExtensionID u32, VendorExtensionVersion u16,
    // VendorExtensionDesc str, FunctionalMode u16, 5 arrays of u16, Manufacturer str, Model str, ...
    PtpResult r = dev_.Read(PTP_OC_GetDeviceInfo, {}, 5000);
    if (!r.ok() || r.data.size() < 8) return;
    const auto& d = r.data;
    size_t i = 8;
    PtpString(d, i);
    i += 2;
    for (int k = 0; k < 5; k++) SkipArray16(d, i);
    std::string manufacturer = PtpString(d, i);
    std::string model = PtpString(d, i);
    if (!model.empty()) name_ = model;
    (void)manufacturer;
}

PtpResult CanonCamera::SetPropEx(uint32_t prop, uint32_t value) {
    std::vector<uint8_t> d(12);
    Put32(d, 0, 12);
    Put32(d, 4, prop);
    Put32(d, 8, value);
    return dev_.Write(PTP_OC_CANON_EOS_SetDevicePropValueEx, {}, d);
}

PtpResult CanonCamera::WriteProp(uint32_t prop, uint32_t value) {
    PtpResult r = SetPropEx(prop, value);
    if (r.ok()) {
        std::lock_guard<std::mutex> lk(propsMu_);
        PropState& p = props_[prop];
        p.hasValue = true;
        p.value = value;  // optimistic: the PropValueChanged event confirms (or corrects) it right after
        propsGen_++;
    } else if (!r.answered()) {
        OnUsbError(r.usbError);
    }
    return r;
}

PtpResult CanonCamera::WritePropWords(uint32_t prop, const std::vector<uint32_t>& words) {
    std::vector<uint8_t> d(8 + words.size() * 4);
    Put32(d, 0, uint32_t(d.size()));
    Put32(d, 4, prop);
    for (size_t i = 0; i < words.size(); i++) Put32(d, 8 + i * 4, words[i]);
    PtpResult r = dev_.Write(PTP_OC_CANON_EOS_SetDevicePropValueEx, {}, d);
    if (r.ok()) {
        std::lock_guard<std::mutex> lk(propsMu_);
        PropState& p = props_[prop];
        p.raw.assign(d.begin() + 8, d.end());
        p.hasValue = true;
        p.value = words.empty() ? 0 : words[0];
        propsGen_++;
    } else if (!r.answered()) {
        OnUsbError(r.usbError);
    }
    return r;
}

PtpResult CanonCamera::AutoFocus() {
    PtpResult r = dev_.Exec(PTP_OC_CANON_EOS_DoAf);
    if (r.ok()) afGraceUntilMs_ = NowMs() + kAutoFocusGraceMs;
    else if (!r.answered()) OnUsbError(r.usbError);
    return r;
}

void CanonCamera::RefreshEvfOutput(uint64_t now) {
    lastEvfRefreshMs_ = now;
    evfGraceUntilMs_ = now + kEvfRefreshGraceMs;
    uint64_t t0 = NowMs();
    PtpResult a = SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, evfAlt_);
    PtpResult b = SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, 2);
    uint64_t ms = NowMs() - t0;
    if (!evfRefreshLogged_ || !a.ok() || !b.ok()) {  // logs the first switch and every failure
        evfRefreshLogged_ = true;
        Log("camera: Live View output switched to %u and back to the PC every %llu s (responses 0x%04X, 0x%04X; "
            "%llu ms)",
            evfAlt_, static_cast<unsigned long long>(evfRefreshMs_ / 1000), a.code, b.code,
            static_cast<unsigned long long>(ms));
    }
    if (!b.answered()) OnUsbError(b.usbError);
    else if (!a.answered()) OnUsbError(a.usbError);
}

bool CanonCamera::GetProp(uint32_t prop, PropState& out) const {
    std::lock_guard<std::mutex> lk(propsMu_);
    auto it = props_.find(prop);
    if (it == props_.end()) return false;
    out = it->second;
    return it->second.hasValue || it->second.hasList;
}

// Parses the GetEvent buffer: a sequence of records [u32 size][u32 type][data...], terminated by an empty
// record (size 8, type 0). Only the two property types matter here:
//   0xC189 PropValueChanged: [8: property code][12: value (u32) or structure]
//   0xC18A AvailListChanged: [8: code][12: data type][16: count][20: values, 4 bytes each]
void CanonCamera::ParseEvents(const std::vector<uint8_t>& d) {
    constexpr uint32_t kPropValueChanged = 0xC189, kAvailListChanged = 0xC18A;
    bool changed = false;
    size_t i = 0;
    while (i + 8 <= d.size()) {
        uint32_t sz = Rd32(d, i), ty = Rd32(d, i + 4);
        if (sz == 8 && ty == 0) break;
        if (sz < 8 || i + sz > d.size()) break;
        if (dumpEvents_ && (ty == kPropValueChanged || ty == kAvailListChanged) && sz >= 12) {
            std::string hex;
            char b[4];
            for (size_t k = 12; k < sz && k < 160; k++) {
                snprintf(b, sizeof b, "%02X ", d[i + k]);
                hex += b;
            }
            Log("event %04X prop %04X (%u bytes): %s", ty, Rd32(d, i + 8), sz, hex.c_str());
        }
        if (ty == kPropValueChanged && sz >= 16) {
            uint32_t prop = Rd32(d, i + 8);
            std::lock_guard<std::mutex> lk(propsMu_);
            PropState& p = props_[prop];
            uint32_t v = Rd32(d, i + 12);
            std::vector<uint8_t> raw(d.begin() + ptrdiff_t(i) + 12, d.begin() + ptrdiff_t(i + sz));
            if (!p.hasValue || p.value != v || p.raw != raw) changed = true;
            p.raw = std::move(raw);
            p.hasValue = true;
            p.value = v;
        } else if (ty == kAvailListChanged && sz >= 20) {
            uint32_t prop = Rd32(d, i + 8);
            uint32_t count = Rd32(d, i + 16);
            if (count <= 4096 && size_t(20) + size_t(count) * 4 <= sz) {
                std::lock_guard<std::mutex> lk(propsMu_);
                PropState& p = props_[prop];
                std::vector<uint32_t> list(count);
                for (uint32_t k = 0; k < count; k++) list[k] = Rd32(d, i + 20 + size_t(k) * 4);
                if (!p.hasList || p.list != list) changed = true;
                p.hasList = true;
                p.list = std::move(list);
            }
        }
        i += sz;
    }
    if (changed) {
        std::lock_guard<std::mutex> lk(propsMu_);
        propsGen_++;
    }
}

bool CanonCamera::PollEvents() {
    if (!connected_) return false;
    uint32_t before = PropsGeneration();
    auto r = dev_.Read(PTP_OC_CANON_EOS_GetEvent);
    lastEventMs_ = NowMs();
    if (r.ok()) ParseEvents(r.data);
    else if (!r.answered()) OnUsbError(r.usbError);
    return PropsGeneration() != before;
}

void CanonCamera::OnUsbError(int err) {
    lastUsbError_ = err;
    if (dev_.Gone()) return;
    usbErrorsInRow_++;
    dev_.Recover(usbErrorsInRow_ >= 3);
}

bool CanonCamera::Connect(std::string& error) {
    Disconnect();
    if (!dev_.Open(error)) return false;
    name_ = dev_.Info().usbProduct.empty() ? "Canon camera" : dev_.Info().usbProduct;
    {
        std::lock_guard<std::mutex> lk(propsMu_);
        props_.clear();
        propsGen_++;
    }
    usbErrorsInRow_ = 0;

    // A previous process may have died mid-transaction: start from a clean pipe.
    PtpResult r = dev_.Exec(PTP_OC_OpenSession, {1}, 5000);
    if (!r.answered()) {
        dev_.Recover(true);
        r = dev_.Exec(PTP_OC_OpenSession, {1}, 5000);
    }
    if (!r.answered() || (r.code != PTP_RC_OK && r.code != PTP_RC_SessionAlreadyOpen)) {
        char b[160];
        if (!r.answered())
            snprintf(b, sizeof b, "the camera did not answer OpenSession (%s)", UsbErrorName(r.usbError));
        else
            snprintf(b, sizeof b, "OpenSession refused (response 0x%04X)", r.code);
        error = b;
        dev_.Close();
        return false;
    }
    ReadDeviceInfo();

    if (!EnableLiveView()) {
        char b[160];
        if (lastUsbError_)
            snprintf(b, sizeof b, "couldn't turn Live View on (%s)", UsbErrorName(lastUsbError_));
        else
            snprintf(b, sizeof b, "couldn't turn Live View on (response 0x%04X)", lastCode_);
        error = b;
        if (!dev_.Gone()) {
            dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {0});
            dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {0});
            dev_.Exec(PTP_OC_CloseSession);
        }
        dev_.Close();
        return false;
    }
    connected_ = true;
    consecutiveErrors_ = 0;
    recoveriesSinceOk_ = 0;
    lastEventMs_ = lastOkMs_ = lastRecoveryMs_ = connectMs_ = NowMs();
    afGraceUntilMs_ = 0;
    lastKeepAliveMs_ = 0;  // the first one goes out right away (and its answer is logged)
    keepAliveResult_ = -1;
    keepAlive_ = Env("CANON_EOS_WEBCAM_KEEPALIVE") != "0";  // debug switch, to reproduce the 30-minute drop
    evfRefreshMs_ = uint64_t(EnvU32("CANON_EOS_WEBCAM_EVF_REFRESH_S", kEvfRefreshDefaultS)) * 1000;
    evfAlt_ = EnvU32("CANON_EOS_WEBCAM_EVF_ALT", kEvfAltDefault);
    lastEvfRefreshMs_ = connectMs_;
    evfGraceUntilMs_ = 0;
    evfRefreshLogged_ = false;
    if (!keepAlive_) Log("camera: KeepDeviceOn disabled (CANON_EOS_WEBCAM_KEEPALIVE=0)");
    if (!evfRefreshMs_) Log("camera: Live View output switch disabled (CANON_EOS_WEBCAM_EVF_REFRESH_S=0)");
    return true;
}

// Remote mode + Live View with output to the PC. Idempotent: also turns Live View back on when something
// turned it off.
bool CanonCamera::EnableLiveView() {
    lastUsbError_ = 0;
    auto r = dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {1});
    lastCode_ = r.code;
    if (!r.ok()) {
        if (!r.answered()) lastUsbError_ = r.usbError;
        return false;
    }
    dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {1});
    {
        auto ev = dev_.Read(PTP_OC_CANON_EOS_GetEvent);  // the first GetEvent dumps the complete state
        if (ev.ok()) ParseEvents(ev.data);
    }
    SetPropEx(PTP_DPC_CANON_EOS_EVFMode, 1);
    r = SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, 2);
    lastCode_ = r.code;
    if (!r.answered()) lastUsbError_ = r.usbError;
    return r.ok();
}

void CanonCamera::Disconnect() {
    if (!dev_.IsOpen()) return;
    if (connected_ && !dev_.Gone()) {
        SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, 0);
        dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {0});
        dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {0});
        dev_.Exec(PTP_OC_CloseSession);
    }
    dev_.Close();
    connected_ = false;
}

CanonCamera::Frame CanonCamera::GetFrame(std::vector<uint8_t>& jpeg) {
    if (!connected_) return Frame::Error;
    if (dev_.Gone()) return Frame::Error;
    uint64_t now = NowMs();
    if (now - lastEventMs_ > uint64_t(eventIntervalMs_)) PollEvents();  // drains the camera event queue
    if (keepAlive_ && now - lastKeepAliveMs_ >= kKeepAliveMs) {
        lastKeepAliveMs_ = now;
        PtpResult k = dev_.Exec(PTP_OC_CANON_EOS_KeepDeviceOn);
        int result = k.answered() ? int(k.code) : -2;
        if (result != keepAliveResult_) {  // logs the first answer and every change
            keepAliveResult_ = result;
            if (k.answered())
                Log("camera: KeepDeviceOn every %llu s, response 0x%04X",
                    static_cast<unsigned long long>(kKeepAliveMs / 1000), k.code);
            else
                Log("camera: KeepDeviceOn failed (%s)", UsbErrorName(k.usbError));
        }
        if (!k.answered()) {
            OnUsbError(k.usbError);
            if (dev_.Gone()) return Frame::Error;
        }
    }
    if (evfRefreshMs_ && now - lastEvfRefreshMs_ >= evfRefreshMs_) {
        RefreshEvfOutput(now);
        if (dev_.Gone()) return Frame::Error;
    }

    // No frames for too long: something turned Live View off. Turn it back on; after 3 attempts in a row
    // without success, give up so the caller reconnects from scratch.
    // (the first frame takes ~1.2 s after Live View is turned on, hence the initial grace period; an autofocus
    // or an output switch may also hold the frames back for a while)
    if (now - connectMs_ > 2500 && now - lastOkMs_ > 800 && now - lastRecoveryMs_ > 1000 &&
        now >= afGraceUntilMs_ && now >= evfGraceUntilMs_) {
        if (recoveriesSinceOk_ >= 3) return Frame::Error;
        lastRecoveryMs_ = now;
        recoveries_++;
        recoveriesSinceOk_++;
        Log("camera: no frames for %llu ms, turning Live View back on",
            static_cast<unsigned long long>(now - lastOkMs_));
        EnableLiveView();
        lastEvfRefreshMs_ = now;  // a new Live View session: its 30-minute timer starts over anyway
        if (dev_.Gone()) return Frame::Error;
    }

    auto r = dev_.Read(PTP_OC_CANON_EOS_GetViewFinderData, {0x00100000, 0, 0});
    if (!r.ok()) {
        lastCode_ = r.code;
        if (!r.answered()) {
            OnUsbError(r.usbError);
            if (dev_.Gone()) return Frame::Error;
        }
        if (r.code == PTP_RC_CANON_NOT_READY || r.code == PTP_RC_DeviceBusy) {
            consecutiveErrors_ = 0;
            return Frame::NotReady;
        }
        return ++consecutiveErrors_ > 10 ? Frame::Error : Frame::NotReady;
    }
    usbErrorsInRow_ = 0;
    size_t off = 0, len = 0;
    if (!ExtractJpeg(r.data, off, len)) {
        lastCode_ = r.code;
        return Frame::NotReady;
    }
    consecutiveErrors_ = 0;
    recoveriesSinceOk_ = 0;
    lastOkMs_ = NowMs();
    jpeg.assign(r.data.begin() + ptrdiff_t(off), r.data.begin() + ptrdiff_t(off + len));
    return Frame::Ok;
}

}  // namespace canon_eos_webcam
