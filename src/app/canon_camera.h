// Access to the Live View of Canon EOS cameras over PTP, through the standard Windows WPD/MTP driver.
#pragma once

#include <windows.h>
#include <PortableDeviceApi.h>
#include <wrl/client.h>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace canon_eos_webcam {

struct PtpResult {
    HRESULT hr = E_FAIL;
    DWORD code = 0;
    std::vector<DWORD> params;
    std::vector<BYTE> data;
    bool ok() const { return SUCCEEDED(hr) && code == 0x2001; }
};

class PtpDevice {
public:
    HRESULT Open(const std::wstring& id);
    void Close();
    bool IsOpen() const { return dev_ != nullptr; }
    PtpResult Exec(WORD op, std::initializer_list<DWORD> p = {});
    PtpResult Read(WORD op, std::initializer_list<DWORD> p = {});
    PtpResult Write(WORD op, std::initializer_list<DWORD> p, const std::vector<BYTE>& data);

private:
    HRESULT Send(IPortableDeviceValues* in, Microsoft::WRL::ComPtr<IPortableDeviceValues>& out);
    HRESULT EndTransfer(PCWSTR ctx, PtpResult& r);
    Microsoft::WRL::ComPtr<IPortableDevice> dev_;
};

// Canon EOS property codes (PTP). Values and encodings checked against libgphoto2
// (camlibs/ptp2/ptp.h and config.c), used only as a reference.
enum : uint32_t {
    kPropISO = 0xD103,
    kPropExpComp = 0xD104,
    kPropWhiteBalance = 0xD109,
    kPropColorTemp = 0xD10A,
    kPropPictureStyle = 0xD110,
    kPropAlo = 0xD1C1,
};

// Last reading of a camera property, fed by the PropValueChanged (0xC189) and AvailListChanged (0xC18A)
// events of GetEvent (0x9116).
struct PropState {
    bool hasValue = false;
    uint32_t value = 0;
    bool hasList = false;
    std::vector<uint32_t> list;  // allowed values announced by the camera
    std::vector<uint8_t> raw;    // bytes of the last PropValueChanged (for structured properties)
};

class CanonCamera {
public:
    enum class Frame { Ok, NotReady, Error };

    ~CanonCamera() { Disconnect(); }

    // Finds the first Canon camera, opens it and turns on Live View to the PC. error is English text.
    bool Connect(std::string& error);
    // Turns Live View off, leaves remote mode and closes the device.
    void Disconnect();
    bool IsConnected() const { return connected_; }
    const std::string& Name() const { return name_; }
    // Diagnostics: last GetViewFinderData response that was not a frame.
    HRESULT LastHr() const { return lastHr_; }
    DWORD LastCode() const { return lastCode_; }
    size_t LastSize() const { return lastSize_; }
    // How many times Live View had to be turned back on because the camera stopped sending frames
    // (happens when other software, such as the EOS Webcam Utility driver, touches the camera).
    int Recoveries() const { return recoveries_; }

    // Fetches the most recent Live View frame (JPEG).
    Frame GetFrame(std::vector<uint8_t>& jpeg);

    // ---- camera properties (panel settings)
    // Interval between event reads (ms); an open panel asks for a shorter interval.
    void SetEventInterval(DWORD ms) { eventIntervalMs_ = ms; }
    // Drains the camera event queue and updates the cache. Returns true if something changed.
    bool PollEvents();
    // Copy of the state of a property (false if the camera never announced it). Thread-safe.
    bool GetProp(uint32_t prop, PropState& out) const;
    // Copy of every known property (diagnostics: "canon-eos-webcam cam").
    std::map<uint32_t, PropState> AllProps() const {
        std::lock_guard<std::mutex> lk(propsMu_);
        return props_;
    }
    // How many times the cache changed (the panel only redraws when it changes).
    uint32_t PropsGeneration() const { return propsGen_; }
    // Writes a property (SetDevicePropValueEx). 0x2001 = accepted; anything else = the camera refused.
    PtpResult WriteProp(uint32_t prop, uint32_t value);
    // Writes a structured property (list of 32-bit words).
    PtpResult WritePropWords(uint32_t prop, const std::vector<uint32_t>& words);
    // Starts an autofocus (DoAf), like a half-press of the shutter button. In Live View the camera uses
    // its Live View AF mode; "Quick mode" lowers the mirror while focusing.
    PtpResult AutoFocus();

private:
    PtpResult SetPropEx(uint32_t prop, uint32_t value);
    bool EnableLiveView();
    // Switches the Live View output away from the PC and back (tries to restart the 30-min session timer).
    void RefreshEvfOutput(uint64_t now);
    void ParseEvents(const std::vector<BYTE>& d);

    PtpDevice dev_;
    bool connected_ = false;
    std::string name_;
    uint64_t lastEventTick_ = 0;
    int consecutiveErrors_ = 0;
    HRESULT lastHr_ = S_OK;
    DWORD lastCode_ = 0;
    size_t lastSize_ = 0;
    uint64_t lastOkTick_ = 0;
    uint64_t lastRecoveryTick_ = 0;
    uint64_t connectTick_ = 0;
    uint64_t afGraceUntil_ = 0;
    uint64_t lastKeepAliveTick_ = 0;
    DWORD keepAliveCode_ = ~0u;  // last KeepDeviceOn response (only changes are logged)
    uint64_t evfRefreshMs_ = 0, lastEvfRefreshTick_ = 0, evfGraceUntil_ = 0;
    uint32_t evfAlt_ = 1;
    bool evfRefreshLogged_ = false;
    int recoveries_ = 0;
    int recoveriesSinceOk_ = 0;

    DWORD eventIntervalMs_ = 1000;
    mutable std::mutex propsMu_;
    std::map<uint32_t, PropState> props_;
    volatile uint32_t propsGen_ = 0;
    bool dumpEvents_ = false;
};

std::string W2U(const wchar_t* w);

}  // namespace canon_eos_webcam
