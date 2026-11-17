#include "ptp_usb.h"

#include <libusb.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <unistd.h>

namespace canon_eos_webcam {

namespace {

constexpr uint16_t kCanonVendor = 0x04a9;
constexpr size_t kRxSize = 1024 * 1024;  // a Live View frame is ~60-150 KiB

enum : uint16_t { kTypeCommand = 1, kTypeData = 2, kTypeResponse = 3 };

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

bool FindStillImageInterface(libusb_device* dev, UsbCameraInfo& info) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return false;
    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const libusb_interface& itf = cfg->interface[i];
        for (int a = 0; a < itf.num_altsetting && !found; a++) {
            const libusb_interface_descriptor& d = itf.altsetting[a];
            if (d.bInterfaceClass != LIBUSB_CLASS_IMAGE) continue;
            UsbCameraInfo r = info;
            r.interface = d.bInterfaceNumber;
            for (int e = 0; e < d.bNumEndpoints; e++) {
                const libusb_endpoint_descriptor& ep = d.endpoint[e];
                int type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                bool in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                if (type == LIBUSB_TRANSFER_TYPE_BULK && in) { r.epIn = ep.bEndpointAddress; r.maxPacket = ep.wMaxPacketSize; }
                else if (type == LIBUSB_TRANSFER_TYPE_BULK) r.epOut = ep.bEndpointAddress;
                else if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in) r.epInt = ep.bEndpointAddress;
            }
            if (r.epIn && r.epOut) {
                info = r;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

// Names of the processes that have the given USB device node open (to say who holds the camera).
std::string HoldersOf(uint8_t bus, uint8_t addr) {
    char node[64];
    snprintf(node, sizeof node, "/dev/bus/usb/%03u/%03u", unsigned(bus), unsigned(addr));
    std::string out;
    DIR* proc = opendir("/proc");
    if (!proc) return out;
    while (dirent* e = readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string fdDir = std::string("/proc/") + e->d_name + "/fd";
        DIR* fds = opendir(fdDir.c_str());
        if (!fds) continue;  // other users' processes: no permission
        bool holds = false;
        while (dirent* f = readdir(fds)) {
            char target[256];
            std::string link = fdDir + "/" + f->d_name;
            ssize_t n = readlink(link.c_str(), target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (strcmp(target, node) == 0) { holds = true; break; }
        }
        closedir(fds);
        if (!holds || std::string(e->d_name) == std::to_string(getpid())) continue;
        std::ifstream comm(std::string("/proc/") + e->d_name + "/comm");
        std::string name;
        std::getline(comm, name);
        if (!out.empty()) out += ", ";
        out += name + " (pid " + e->d_name + ")";
    }
    closedir(proc);
    return out;
}

}  // namespace

const char* UsbErrorName(int err) { return libusb_error_name(err); }

PtpUsb::PtpUsb() : rx_(kRxSize) { libusb_init(&ctx_); }

PtpUsb::~PtpUsb() {
    Close();
    if (ctx_) libusb_exit(ctx_);
}

bool PtpUsb::Present(libusb_context* ctx) {
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < n && !found; i++) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == kCanonVendor) {
            UsbCameraInfo tmp;
            found = FindStillImageInterface(list[i], tmp);
        }
    }
    if (list) libusb_free_device_list(list, 1);
    return found;
}

bool PtpUsb::Open(std::string& error) {
    Close();
    gone_ = false;
    if (!ctx_) {
        error = "libusb could not be initialized";
        return false;
    }
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx_, &list);
    if (n < 0) {
        error = std::string("couldn't list the USB devices: ") + libusb_error_name(int(n));
        return false;
    }
    libusb_device* sel = nullptr;
    UsbCameraInfo info;
    for (ssize_t i = 0; i < n && !sel; i++) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != kCanonVendor) continue;
        UsbCameraInfo cand;
        cand.vendor = dd.idVendor;
        cand.product = dd.idProduct;
        cand.bus = libusb_get_bus_number(list[i]);
        cand.address = libusb_get_device_address(list[i]);
        if (FindStillImageInterface(list[i], cand)) {
            sel = list[i];
            info = cand;
        }
    }
    if (!sel) {
        libusb_free_device_list(list, 1);
        error = "no Canon camera found on USB (turned off or disconnected?)";
        return false;
    }
    libusb_device_handle* h = nullptr;
    int r = libusb_open(sel, &h);
    libusb_free_device_list(list, 1);
    if (r != 0) {
        char b[256];
        if (r == LIBUSB_ERROR_ACCESS)
            snprintf(b, sizeof b,
                     "no permission to open the camera (/dev/bus/usb/%03u/%03u); install the udev rule "
                     "(linux/udev/70-canon-eos-webcam.rules) and replug the camera",
                     unsigned(info.bus), unsigned(info.address));
        else
            snprintf(b, sizeof b, "couldn't open the camera: %s", libusb_error_name(r));
        error = b;
        return false;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    r = libusb_claim_interface(h, info.interface);
    if (r != 0) {
        if (r == LIBUSB_ERROR_BUSY) {
            std::string who = HoldersOf(info.bus, info.address);
            error = "the camera is in use by another program" +
                    (who.empty() ? std::string(" (usually gvfsd-gphoto2 or a leftover gphoto2)") : ": " + who) +
                    "; close it and the camera will be picked up automatically";
        } else {
            error = std::string("couldn't claim the camera's USB interface: ") + libusb_error_name(r);
        }
        libusb_close(h);
        return false;
    }
    unsigned char s[128];
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(libusb_get_device(h), &dd) == 0 && dd.iProduct &&
        libusb_get_string_descriptor_ascii(h, dd.iProduct, s, sizeof s) > 0)
        info.usbProduct = reinterpret_cast<char*>(s);
    h_ = h;
    info_ = info;
    tid_ = 0;
    return true;
}

void PtpUsb::Close() {
    if (!h_) return;
    libusb_release_interface(h_, info_.interface);
    libusb_close(h_);
    h_ = nullptr;
}

void PtpUsb::NoteError(int err) {
    if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_NOT_FOUND) gone_ = true;
}

int PtpUsb::BulkOut(const uint8_t* d, size_t n, int timeoutMs) {
    int sent = 0;
    int r = libusb_bulk_transfer(h_, info_.epOut, const_cast<uint8_t*>(d), int(n), &sent, unsigned(timeoutMs));
    if (r == 0 && size_t(sent) != n) r = LIBUSB_ERROR_IO;
    if (r == 0 && n % info_.maxPacket == 0) {  // a transfer that is a multiple of the packet size ends with a ZLP
        r = libusb_bulk_transfer(h_, info_.epOut, const_cast<uint8_t*>(d), 0, &sent, unsigned(timeoutMs));
    }
    return r;
}

int PtpUsb::ReadContainer(std::vector<uint8_t>& buf, int timeoutMs) {
    const size_t mps = info_.maxPacket ? info_.maxPacket : 512;
    // First transfer: as much as the buffer holds. It ends at the first short (or zero-length) packet, which
    // means the whole container when it fits.
    int got = 0;
    int r = libusb_bulk_transfer(h_, info_.epIn, rx_.data(), int(rx_.size()), &got, unsigned(timeoutMs));
    if (r != 0) return r;
    if (got == 0) {  // a stray zero-length packet left over from a previous transfer
        r = libusb_bulk_transfer(h_, info_.epIn, rx_.data(), int(rx_.size()), &got, unsigned(timeoutMs));
        if (r != 0) return r;
    }
    if (got < 12) return LIBUSB_ERROR_IO;
    size_t total = Rd32(rx_.data());
    if (total < 12 || total > 64u * 1024 * 1024) return LIBUSB_ERROR_IO;
    buf.assign(rx_.begin(), rx_.begin() + got);
    while (buf.size() < total) {
        size_t remaining = total - buf.size();
        // Asks for the rest rounded up to whole packets, plus one more packet when the rest is an exact multiple,
        // so the transfer always ends with the short packet or the ZLP the camera sends.
        size_t want = (remaining + mps - 1) / mps * mps;
        if (remaining % mps == 0) want += mps;
        if (want > rx_.size()) want = rx_.size() / mps * mps;
        r = libusb_bulk_transfer(h_, info_.epIn, rx_.data(), int(want), &got, unsigned(timeoutMs));
        if (r != 0) return r;
        if (got == 0) break;
        buf.insert(buf.end(), rx_.begin(), rx_.begin() + got);
    }
    if (buf.size() < total) return LIBUSB_ERROR_IO;
    buf.resize(total);
    return 0;
}

PtpResult PtpUsb::Transaction(uint16_t op, std::initializer_list<uint32_t> p, Phase phase,
                              const std::vector<uint8_t>* out, int timeoutMs) {
    PtpResult res;
    if (!h_) {
        res.usbError = LIBUSB_ERROR_NO_DEVICE;
        return res;
    }
    uint32_t tid = ++tid_;
    if (op == PTP_OC_OpenSession) tid = tid_ = 0;  // OpenSession uses transaction id 0

    uint8_t cmd[32];
    size_t np = std::min<size_t>(p.size(), 5);
    size_t len = 12 + 4 * np;
    Put32(cmd, uint32_t(len));
    Put16(cmd + 4, kTypeCommand);
    Put16(cmd + 6, op);
    Put32(cmd + 8, tid);
    size_t k = 0;
    for (uint32_t v : p) {
        if (k >= np) break;
        Put32(cmd + 12 + 4 * k++, v);
    }
    int r = BulkOut(cmd, len, timeoutMs);
    if (r != 0) {
        NoteError(r);
        res.usbError = r;
        return res;
    }

    if (phase == Phase::Out && out) {
        std::vector<uint8_t> d(12 + out->size());
        Put32(d.data(), uint32_t(d.size()));
        Put16(d.data() + 4, kTypeData);
        Put16(d.data() + 6, op);
        Put32(d.data() + 8, tid);
        if (!out->empty()) memcpy(d.data() + 12, out->data(), out->size());
        r = BulkOut(d.data(), d.size(), timeoutMs);
        if (r != 0) {
            NoteError(r);
            res.usbError = r;
            return res;
        }
    }

    std::vector<uint8_t> c;
    r = ReadContainer(c, timeoutMs);
    if (r != 0) {
        NoteError(r);
        res.usbError = r;
        return res;
    }
    if (Rd16(c.data() + 4) == kTypeData) {
        res.data.assign(c.begin() + 12, c.end());
        r = ReadContainer(c, timeoutMs);
        if (r != 0) {
            NoteError(r);
            res.usbError = r;
            return res;
        }
    }
    // Here c must be the response (the camera skips the data phase when it refuses the operation).
    if (Rd16(c.data() + 4) != kTypeResponse) {
        res.usbError = LIBUSB_ERROR_IO;
        return res;
    }
    res.code = Rd16(c.data() + 6);
    for (size_t i = 12; i + 4 <= c.size() && res.params.size() < 5; i += 4) res.params.push_back(Rd32(c.data() + i));
    (void)phase;
    return res;
}

PtpResult PtpUsb::Exec(uint16_t op, std::initializer_list<uint32_t> p, int timeoutMs) {
    return Transaction(op, p, Phase::None, nullptr, timeoutMs);
}
PtpResult PtpUsb::Read(uint16_t op, std::initializer_list<uint32_t> p, int timeoutMs) {
    return Transaction(op, p, Phase::In, nullptr, timeoutMs);
}
PtpResult PtpUsb::Write(uint16_t op, std::initializer_list<uint32_t> p, const std::vector<uint8_t>& data,
                        int timeoutMs) {
    return Transaction(op, p, Phase::Out, &data, timeoutMs);
}

bool PtpUsb::Recover(bool deviceReset) {
    if (!h_ || gone_) return false;
    int r1 = libusb_clear_halt(h_, info_.epIn);
    int r2 = libusb_clear_halt(h_, info_.epOut);
    NoteError(r1);
    NoteError(r2);
    if (deviceReset && !gone_) {
        // PTP class request "Device Reset" (Still Image Capture Device Definition, 5.2.3).
        int r = libusb_control_transfer(h_, 0x21, 0x66, 0, info_.interface, nullptr, 0, 2000);
        NoteError(r);
    }
    if (!gone_) {  // drains whatever was left in the IN pipe
        int got = 0;
        for (int i = 0; i < 4; i++) {
            int r = libusb_bulk_transfer(h_, info_.epIn, rx_.data(), int(rx_.size()), &got, 50);
            if (r != 0 || got == 0) break;
        }
    }
    return !gone_;
}

}  // namespace canon_eos_webcam
