// Canon EOS Live View and camera properties over PTP (Canon vendor operations), on top of PtpUsb.
// The command sequence, the parameters and the parsing are the same as the Windows version
// (src/app/canon_camera.cpp); only the transport differs.
#pragma once

#include "ptp_usb.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace canon_eos_webcam {

enum : uint16_t {
    PTP_OC_CANON_EOS_SetDevicePropValueEx = 0x9110,
    PTP_OC_CANON_EOS_SetRemoteMode = 0x9114,
    PTP_OC_CANON_EOS_SetEventMode = 0x9115,
    PTP_OC_CANON_EOS_GetEvent = 0x9116,
    PTP_OC_CANON_EOS_KeepDeviceOn = 0x911D,
    PTP_OC_CANON_EOS_GetViewFinderData = 0x9153,
    PTP_OC_CANON_EOS_DoAf = 0x9154,
    PTP_OC_CANON_EOS_AfCancel = 0x9160,
};

// Last reading of a camera property, fed by the PropValueChanged (0xC189) and AvailListChanged (0xC18A)
// events of GetEvent (0x9116).
struct PropState {
    bool hasValue = false;
    uint32_t value = 0;
    bool hasList = false;
    std::vector<uint32_t> list;  // allowed values announced by the camera
    std::vector<uint8_t> raw;    // bytes of the last PropValueChanged, from offset 12 (structured properties)
};

class CanonCamera {
public:
    enum class Frame { Ok, NotReady, Error };

    ~CanonCamera() { Disconnect(); }

    // Finds the first Canon camera, opens the session and turns Live View on (output to the PC).
    // error is English text for the log.
    bool Connect(std::string& error);
    // Turns Live View off, leaves remote mode and releases the device (skips the commands if it is gone).
    void Disconnect();
    bool IsConnected() const { return connected_; }
    // Model name (from GetDeviceInfo), e.g. "Canon EOS REBEL T3".
    const std::string& Name() const { return name_; }
    const UsbCameraInfo& UsbInfo() const { return dev_.Info(); }
    // Diagnostics: last response that was not a frame.
    uint16_t LastCode() const { return lastCode_; }
    int LastUsbError() const { return lastUsbError_; }
    int Recoveries() const { return recoveries_; }

    // Fetches the most recent Live View frame (JPEG). Also drains the event queue every eventIntervalMs and
    // turns Live View back on when the frames stop.
    Frame GetFrame(std::vector<uint8_t>& jpeg);

    // ---- properties
    void SetEventInterval(int ms) { eventIntervalMs_ = ms; }
    void SetDumpEvents(bool on) { dumpEvents_ = on; }
    // Drains the camera event queue and updates the cache. Returns true if something changed.
    bool PollEvents();
    bool GetProp(uint32_t prop, PropState& out) const;
    std::map<uint32_t, PropState> AllProps() const {
        std::lock_guard<std::mutex> lk(propsMu_);
        return props_;
    }
    uint32_t PropsGeneration() const {
        std::lock_guard<std::mutex> lk(propsMu_);
        return propsGen_;
    }
    // SetDevicePropValueEx. 0x2001 = accepted; anything else = the camera refused.
    PtpResult WriteProp(uint32_t prop, uint32_t value);
    // Structured property (list of 32-bit words, e.g. the Picture Style parameters).
    PtpResult WritePropWords(uint32_t prop, const std::vector<uint32_t>& words);
    // Starts an autofocus (DoAf), like a half-press of the shutter button. In Live View the camera uses its Live
    // View AF mode; "Quick mode" lowers the mirror while focusing. The camera keeps the button "half-pressed" (and
    // answers 0x2019 Device Busy to every property write, including turning Live View on) until AfCancel, which
    // GetFrame sends once the focusing had time to finish.
    PtpResult AutoFocus();

private:
    PtpResult SetPropEx(uint32_t prop, uint32_t value);
    bool EnableLiveView();
    // Switches the Live View output away from the PC and back, which restarts the camera's 30-minute Live View
    // session timer without lowering the mirror.
    void RefreshEvfOutput(uint64_t now);
    // Releases the half-press left by AutoFocus (AfCancel), if there is one. The focus stays where it is.
    void ReleaseAutoFocus();
    void ParseEvents(const std::vector<uint8_t>& d);
    void ReadDeviceInfo();
    // Handles a USB-level failure: clears the halts (and resets the device after repeated failures).
    void OnUsbError(int err);

    PtpUsb dev_;
    bool connected_ = false;
    std::string name_;
    uint64_t lastEventMs_ = 0;
    int consecutiveErrors_ = 0;
    int usbErrorsInRow_ = 0;
    uint16_t lastCode_ = 0;
    int lastUsbError_ = 0;
    uint64_t lastOkMs_ = 0;
    uint64_t lastRecoveryMs_ = 0;
    uint64_t connectMs_ = 0;
    uint64_t afGraceUntilMs_ = 0;
    uint64_t afReleaseAtMs_ = 0;  // 0 = no autofocus waiting for its AfCancel
    uint64_t lastKeepAliveMs_ = 0;
    bool keepAlive_ = true;
    int keepAliveResult_ = -1;  // last KeepDeviceOn response, or -2 for a USB failure (only changes are logged)
    uint64_t evfRefreshMs_ = 0, lastEvfRefreshMs_ = 0, evfGraceUntilMs_ = 0;
    uint32_t evfAlt_ = 1;
    bool evfRefreshLogged_ = false;
    int recoveries_ = 0;
    int recoveriesSinceOk_ = 0;

    int eventIntervalMs_ = 1000;
    mutable std::mutex propsMu_;
    std::map<uint32_t, PropState> props_;
    uint32_t propsGen_ = 0;
    bool dumpEvents_ = false;
};

}  // namespace canon_eos_webcam
