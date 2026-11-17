// PTP (ISO 15740) over USB bulk transfers, through libusb. Only what the Canon EOS Live View needs:
// operations with no data phase, with data to the camera and with data from the camera.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

struct libusb_context;
struct libusb_device_handle;

namespace canon_eos_webcam {

enum : uint16_t {
    PTP_RC_OK = 0x2001,
    PTP_RC_DeviceBusy = 0x2019,
    PTP_RC_SessionAlreadyOpen = 0x201E,
    PTP_RC_CANON_NOT_READY = 0xA102,
    PTP_OC_GetDeviceInfo = 0x1001,
    PTP_OC_OpenSession = 0x1002,
    PTP_OC_CloseSession = 0x1003,
};

struct PtpResult {
    int usbError = 0;    // libusb error (0 = the transaction completed at the USB level)
    uint16_t code = 0;   // PTP response code
    std::vector<uint32_t> params;
    std::vector<uint8_t> data;
    bool ok() const { return usbError == 0 && code == PTP_RC_OK; }
    // The camera answered (successfully or not) - as opposed to a USB failure.
    bool answered() const { return usbError == 0 && code != 0; }
};

struct UsbCameraInfo {
    uint8_t bus = 0, address = 0;
    uint16_t vendor = 0, product = 0;
    uint8_t interface = 0, epIn = 0, epOut = 0, epInt = 0;
    uint16_t maxPacket = 512;
    std::string usbProduct;
};

class PtpUsb {
public:
    PtpUsb();
    ~PtpUsb();
    PtpUsb(const PtpUsb&) = delete;
    PtpUsb& operator=(const PtpUsb&) = delete;

    // Finds the first Canon (0x04a9) device with a Still Image interface and claims it. error is English text
    // meant for the log (says in plain words when another process holds the camera).
    bool Open(std::string& error);
    void Close();
    bool IsOpen() const { return h_ != nullptr; }
    // True after a USB failure that means the device is gone (unplugged or turned off).
    bool Gone() const { return gone_; }
    const UsbCameraInfo& Info() const { return info_; }

    PtpResult Exec(uint16_t op, std::initializer_list<uint32_t> p = {}, int timeoutMs = 2000);
    PtpResult Read(uint16_t op, std::initializer_list<uint32_t> p = {}, int timeoutMs = 2000);
    PtpResult Write(uint16_t op, std::initializer_list<uint32_t> p, const std::vector<uint8_t>& data,
                    int timeoutMs = 2000);

    // After a timeout or I/O error: clears the endpoint halts and, if asked, sends the PTP Device Reset class
    // request. Returns false if the device is gone.
    bool Recover(bool deviceReset);

    // Whether a camera is plugged in (cheap: does not open it).
    static bool Present(libusb_context* ctx);
    libusb_context* Context() const { return ctx_; }

private:
    enum class Phase { None, In, Out };
    PtpResult Transaction(uint16_t op, std::initializer_list<uint32_t> p, Phase phase,
                          const std::vector<uint8_t>* out, int timeoutMs);
    int BulkOut(const uint8_t* d, size_t n, int timeoutMs);
    // Reads one container (data or response) into buf; returns the libusb error or 0.
    int ReadContainer(std::vector<uint8_t>& buf, int timeoutMs);
    void NoteError(int err);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* h_ = nullptr;
    UsbCameraInfo info_;
    uint32_t tid_ = 0;
    bool gone_ = false;
    std::vector<uint8_t> rx_;
};

const char* UsbErrorName(int err);

}  // namespace canon_eos_webcam
