#include "canon_camera.h"

#include <PortableDevice.h>
#include <initguid.h>  // instantiates the WpdMtpExtensions.h keys in this file
#include <WpdMtpExtensions.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace canon_eos_webcam {

namespace {

enum : uint16_t {
    PTP_RC_OK = 0x2001,
    PTP_RC_DeviceBusy = 0x2019,
    PTP_RC_CANON_NOT_READY = 0xA102,
    PTP_OC_CANON_EOS_SetDevicePropValueEx = 0x9110,
    PTP_OC_CANON_EOS_SetRemoteMode = 0x9114,
    PTP_OC_CANON_EOS_SetEventMode = 0x9115,
    PTP_OC_CANON_EOS_GetEvent = 0x9116,
    PTP_OC_CANON_EOS_GetViewFinderData = 0x9153,
};
enum : uint32_t {
    PTP_DPC_CANON_EOS_EVFOutputDevice = 0xD1B0,
    PTP_DPC_CANON_EOS_EVFMode = 0xD1B3,
};

uint32_t Rd32(const std::vector<BYTE>& d, size_t i) {
    return uint32_t(d[i]) | uint32_t(d[i + 1]) << 8 | uint32_t(d[i + 2]) << 16 | uint32_t(d[i + 3]) << 24;
}

HRESULT NewCmd(const PROPERTYKEY& cmd, ComPtr<IPortableDeviceValues>& v) {
    HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&v));
    if (FAILED(hr)) return hr;
    v->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, cmd.fmtid);
    return v->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, cmd.pid);
}

HRESULT AddOp(IPortableDeviceValues* v, WORD op, std::initializer_list<DWORD> p) {
    ComPtr<IPortableDevicePropVariantCollection> col;
    HRESULT hr = CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&col));
    if (FAILED(hr)) return hr;
    for (DWORD x : p) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_UI4;
        pv.ulVal = x;
        col->Add(&pv);
    }
    v->SetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_OPERATION_CODE, op);
    return v->SetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_OPERATION_PARAMS, col.Get());
}

void ReadResponse(IPortableDeviceValues* out, PtpResult& r) {
    DWORD code = 0;
    if (SUCCEEDED(out->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, &code))) r.code = code;
    ComPtr<IPortableDevicePropVariantCollection> col;
    if (SUCCEEDED(out->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_RESPONSE_PARAMS, &col)) &&
        col) {
        DWORD n = 0;
        col->GetCount(&n);
        for (DWORD i = 0; i < n; i++) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(col->GetAt(i, &pv)) && pv.vt == VT_UI4) r.params.push_back(pv.ulVal);
            PropVariantClear(&pv);
        }
    }
}

// Splits the GetViewFinderData blocks ([u32 size][u32 type][data]) and finds the JPEG (type 1).
bool ExtractJpeg(const std::vector<BYTE>& d, size_t& off, size_t& len) {
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

struct WpdDeviceEntry {
    std::wstring id;
    std::string name, manufacturer;
};

std::vector<WpdDeviceEntry> ListDevices() {
    std::vector<WpdDeviceEntry> out;
    ComPtr<IPortableDeviceManager> mgr;
    if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
        return out;
    mgr->RefreshDeviceList();
    DWORD n = 0;
    if (FAILED(mgr->GetDevices(nullptr, &n)) || n == 0) return out;
    std::vector<PWSTR> ids(n);
    if (FAILED(mgr->GetDevices(ids.data(), &n))) return out;
    for (DWORD i = 0; i < n; i++) {
        WpdDeviceEntry d;
        d.id = ids[i];
        DWORD len = 0;
        if (SUCCEEDED(mgr->GetDeviceFriendlyName(ids[i], nullptr, &len)) && len) {
            std::wstring s(len, L'\0');
            if (SUCCEEDED(mgr->GetDeviceFriendlyName(ids[i], s.data(), &len))) d.name = W2U(s.c_str());
        }
        len = 0;
        if (SUCCEEDED(mgr->GetDeviceManufacturer(ids[i], nullptr, &len)) && len) {
            std::wstring s(len, L'\0');
            if (SUCCEEDED(mgr->GetDeviceManufacturer(ids[i], s.data(), &len))) d.manufacturer = W2U(s.c_str());
        }
        out.push_back(std::move(d));
        CoTaskMemFree(ids[i]);
    }
    return out;
}

bool ContainsI(std::string hay, std::string needle) {
    for (auto& c : hay) c = char(tolower((unsigned char)c));
    for (auto& c : needle) c = char(tolower((unsigned char)c));
    return hay.find(needle) != std::string::npos;
}

}  // namespace

std::string W2U(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- PtpDevice

HRESULT PtpDevice::Open(const std::wstring& id) {
    ComPtr<IPortableDeviceValues> client;
    HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client));
    if (FAILED(hr)) return hr;
    client->SetStringValue(WPD_CLIENT_NAME, L"canon-eos-webcam");
    client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
    client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
    client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
    client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
    client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ | GENERIC_WRITE);
    ComPtr<IPortableDevice> dev;
    hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) return hr;
    hr = dev->Open(id.c_str(), client.Get());
    if (SUCCEEDED(hr)) dev_ = dev;
    return hr;
}

void PtpDevice::Close() {
    if (dev_) dev_->Close();
    dev_.Reset();
}

HRESULT PtpDevice::Send(IPortableDeviceValues* in, ComPtr<IPortableDeviceValues>& out) {
    out.Reset();
    if (!dev_) return E_HANDLE;
    HRESULT hr = dev_->SendCommand(0, in, &out);
    if (FAILED(hr)) return hr;
    HRESULT hrCmd = S_OK;
    if (out && SUCCEEDED(out->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &hrCmd))) return hrCmd;
    return S_OK;
}

HRESULT PtpDevice::EndTransfer(PCWSTR ctx, PtpResult& r) {
    ComPtr<IPortableDeviceValues> in, out;
    HRESULT hr = NewCmd(WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER, in);
    if (FAILED(hr)) return hr;
    in->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, ctx);
    hr = Send(in.Get(), out);
    if (out) ReadResponse(out.Get(), r);
    return hr;
}

PtpResult PtpDevice::Exec(WORD op, std::initializer_list<DWORD> p) {
    PtpResult r;
    ComPtr<IPortableDeviceValues> in, out;
    if (FAILED(r.hr = NewCmd(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE, in))) return r;
    if (FAILED(r.hr = AddOp(in.Get(), op, p))) return r;
    r.hr = Send(in.Get(), out);
    if (out) ReadResponse(out.Get(), r);
    return r;
}

PtpResult PtpDevice::Read(WORD op, std::initializer_list<DWORD> p) {
    PtpResult r;
    ComPtr<IPortableDeviceValues> in, out;
    if (FAILED(r.hr = NewCmd(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_READ, in))) return r;
    if (FAILED(r.hr = AddOp(in.Get(), op, p))) return r;
    r.hr = Send(in.Get(), out);
    if (!out) return r;

    PWSTR ctx = nullptr;
    if (FAILED(out->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &ctx)) || !ctx) {
        ReadResponse(out.Get(), r);
        return r;
    }
    ULONGLONG total = 0;
    out->GetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, &total);

    HRESULT hrData = r.hr;
    if (SUCCEEDED(hrData) && total > 0 && total < 64ull * 1024 * 1024) {
        r.data.reserve(size_t(total));
        std::vector<BYTE> buf;
        while (r.data.size() < total) {
            ULONGLONG want = std::min<ULONGLONG>(total - r.data.size(), 1024 * 1024);
            buf.assign(size_t(want), 0);
            ComPtr<IPortableDeviceValues> rin, rout;
            if (FAILED(hrData = NewCmd(WPD_COMMAND_MTP_EXT_READ_DATA, rin))) break;
            rin->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, ctx);
            rin->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_READ, want);
            rin->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, buf.data(), DWORD(want));
            if (FAILED(hrData = Send(rin.Get(), rout))) break;
            ULONGLONG got = 0;
            rout->GetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_READ, &got);
            BYTE* chunk = nullptr;
            DWORD cb = 0;
            if (FAILED(hrData = rout->GetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, &chunk, &cb))) break;
            size_t n = size_t(std::min<ULONGLONG>(got, cb));
            r.data.insert(r.data.end(), chunk, chunk + n);
            CoTaskMemFree(chunk);
            if (n == 0) break;
        }
    }
    HRESULT hrEnd = EndTransfer(ctx, r);
    if (SUCCEEDED(r.hr)) r.hr = FAILED(hrData) ? hrData : hrEnd;
    CoTaskMemFree(ctx);
    return r;
}

PtpResult PtpDevice::Write(WORD op, std::initializer_list<DWORD> p, const std::vector<BYTE>& data) {
    PtpResult r;
    ComPtr<IPortableDeviceValues> in, out;
    if (FAILED(r.hr = NewCmd(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITH_DATA_TO_WRITE, in))) return r;
    if (FAILED(r.hr = AddOp(in.Get(), op, p))) return r;
    in->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_TOTAL_DATA_SIZE, data.size());
    r.hr = Send(in.Get(), out);
    if (!out) return r;

    PWSTR ctx = nullptr;
    if (FAILED(out->GetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, &ctx)) || !ctx) {
        ReadResponse(out.Get(), r);
        return r;
    }
    HRESULT hrData = r.hr;
    if (SUCCEEDED(hrData)) {
        ComPtr<IPortableDeviceValues> win, wout;
        if (SUCCEEDED(hrData = NewCmd(WPD_COMMAND_MTP_EXT_WRITE_DATA, win))) {
            win->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, ctx);
            win->SetUnsignedLargeIntegerValue(WPD_PROPERTY_MTP_EXT_TRANSFER_NUM_BYTES_TO_WRITE, data.size());
            win->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, const_cast<BYTE*>(data.data()),
                                DWORD(data.size()));
            hrData = Send(win.Get(), wout);
        }
    }
    HRESULT hrEnd = EndTransfer(ctx, r);
    if (SUCCEEDED(r.hr)) r.hr = FAILED(hrData) ? hrData : hrEnd;
    CoTaskMemFree(ctx);
    return r;
}

// ---------------------------------------------------------------- CanonCamera

PtpResult CanonCamera::SetPropEx(uint32_t prop, uint32_t value) {
    std::vector<BYTE> d(12);
    auto put = [&](size_t i, uint32_t v) {
        d[i] = BYTE(v); d[i + 1] = BYTE(v >> 8); d[i + 2] = BYTE(v >> 16); d[i + 3] = BYTE(v >> 24);
    };
    put(0, 12);
    put(4, prop);
    put(8, value);
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
    }
    return r;
}

PtpResult CanonCamera::WritePropWords(uint32_t prop, const std::vector<uint32_t>& words) {
    std::vector<BYTE> d(8 + words.size() * 4);
    auto put = [&](size_t i, uint32_t v) {
        d[i] = BYTE(v); d[i + 1] = BYTE(v >> 8); d[i + 2] = BYTE(v >> 16); d[i + 3] = BYTE(v >> 24);
    };
    put(0, uint32_t(d.size()));
    put(4, prop);
    for (size_t i = 0; i < words.size(); i++) put(8 + i * 4, words[i]);
    return dev_.Write(PTP_OC_CANON_EOS_SetDevicePropValueEx, {}, d);
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
void CanonCamera::ParseEvents(const std::vector<BYTE>& d) {
    constexpr uint32_t kPropValueChanged = 0xC189, kAvailListChanged = 0xC18A;
    bool changed = false;
    size_t i = 0;
    while (i + 8 <= d.size()) {
        uint32_t sz = Rd32(d, i), ty = Rd32(d, i + 4);
        if (sz == 8 && ty == 0) break;
        if (sz < 8 || i + sz > d.size()) break;
        if (dumpEvents_ && (ty == kPropValueChanged || ty == kAvailListChanged) && sz >= 12) {
            char hex[400];
            size_t n = std::min<size_t>(sz, 96), o = 0;
            for (size_t k = 12; k < n && o + 3 < sizeof hex; k++)
                o += size_t(snprintf(hex + o, sizeof hex - o, "%02X ", d[i + k]));
            hex[o] = 0;
            printf("event %04X prop %04X (%u bytes): %s\n", ty, Rd32(d, i + 8), sz, hex);
            fflush(stdout);
        }
        if (ty == kPropValueChanged && sz >= 16) {
            uint32_t prop = Rd32(d, i + 8);
            std::lock_guard<std::mutex> lk(propsMu_);
            PropState& p = props_[prop];
            uint32_t v = Rd32(d, i + 12);
            p.raw.assign(d.begin() + ptrdiff_t(i) + 12, d.begin() + ptrdiff_t(i + sz));
            if (!p.hasValue || p.value != v) changed = true;
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
    if (changed) propsGen_++;
}

bool CanonCamera::PollEvents() {
    if (!connected_) return false;
    uint32_t before = propsGen_;
    auto r = dev_.Read(PTP_OC_CANON_EOS_GetEvent);
    lastEventTick_ = GetTickCount64();
    if (r.ok()) ParseEvents(r.data);
    return propsGen_ != before;
}

bool CanonCamera::Connect(std::string& error) {
    Disconnect();
    const WpdDeviceEntry* sel = nullptr;
    auto devs = ListDevices();
    for (auto& d : devs)
        if (ContainsI(d.manufacturer, "canon") || ContainsI(d.name, "eos")) { sel = &d; break; }
    if (!sel) { error = "no Canon camera found (turned off or disconnected?)"; return false; }

    HRESULT hr = dev_.Open(sel->id);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof b, "failed to open %s (hr=0x%08lX)", sel->name.c_str(), (unsigned long)hr);
        error = b;
        return false;
    }
    name_ = sel->name;
    {
        std::lock_guard<std::mutex> lk(propsMu_);
        props_.clear();
    }

    if (!EnableLiveView()) {
        char b[128];
        snprintf(b, sizeof b, "couldn't turn Live View on (hr=0x%08lX resp=0x%04lX)", (unsigned long)lastHr_,
                 (unsigned long)lastCode_);
        error = b;
        dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {0});
        dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {0});
        dev_.Close();
        return false;
    }
    {
        wchar_t v[8];
        dumpEvents_ = GetEnvironmentVariableW(L"CANON_EOS_WEBCAM_DUMP_EVENTS", v, 8) > 0 && v[0] == L'1';
    }
    connected_ = true;
    consecutiveErrors_ = 0;
    recoveriesSinceOk_ = 0;
    lastEventTick_ = lastOkTick_ = lastRecoveryTick_ = connectTick_ = GetTickCount64();
    return true;
}

// Remote mode + Live View with output to the PC. Idempotent: also turns Live View back on when some other
// software turned it off.
bool CanonCamera::EnableLiveView() {
    auto r = dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {1});
    lastHr_ = r.hr;
    lastCode_ = r.code;
    if (!r.ok()) return false;
    dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {1});
    {
        auto ev = dev_.Read(PTP_OC_CANON_EOS_GetEvent);  // the first GetEvent dumps the complete state
        if (ev.ok()) ParseEvents(ev.data);
    }
    SetPropEx(PTP_DPC_CANON_EOS_EVFMode, 1);
    r = SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, 2);
    lastHr_ = r.hr;
    lastCode_ = r.code;
    return r.ok();
}

void CanonCamera::Disconnect() {
    if (!dev_.IsOpen()) return;
    if (connected_) {
        SetPropEx(PTP_DPC_CANON_EOS_EVFOutputDevice, 0);
        dev_.Exec(PTP_OC_CANON_EOS_SetEventMode, {0});
        dev_.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {0});
    }
    dev_.Close();
    connected_ = false;
}

CanonCamera::Frame CanonCamera::GetFrame(std::vector<uint8_t>& jpeg) {
    if (!connected_) return Frame::Error;
    uint64_t now = GetTickCount64();
    if (now - lastEventTick_ > eventIntervalMs_) {  // drains the camera event queue
        PollEvents();
    }

    // No frames for too long: someone turned Live View off (e.g. the EOS Webcam Utility driver when the
    // Windows Frame Server starts). Turn it back on; after 3 attempts in a row without success, give up so
    // the caller reconnects from scratch.
    // (the first frame takes ~1.2 s after Live View is turned on, hence the initial grace period)
    if (now - connectTick_ > 2500 && now - lastOkTick_ > 800 && now - lastRecoveryTick_ > 1000) {
        if (recoveriesSinceOk_ >= 3) return Frame::Error;
        lastRecoveryTick_ = now;
        recoveries_++;
        recoveriesSinceOk_++;
        EnableLiveView();
    }

    auto r = dev_.Read(PTP_OC_CANON_EOS_GetViewFinderData, {0x00100000, 0, 0});
    if (!r.ok()) {
        lastHr_ = r.hr;
        lastCode_ = r.code;
        lastSize_ = r.data.size();
        if (r.code == PTP_RC_CANON_NOT_READY || r.code == PTP_RC_DeviceBusy) {
            consecutiveErrors_ = 0;
            return Frame::NotReady;
        }
        return ++consecutiveErrors_ > 10 ? Frame::Error : Frame::NotReady;
    }
    size_t off = 0, len = 0;
    if (!ExtractJpeg(r.data, off, len)) {
        lastHr_ = r.hr;
        lastCode_ = r.code;
        lastSize_ = r.data.size();
        return Frame::NotReady;
    }
    consecutiveErrors_ = 0;
    recoveriesSinceOk_ = 0;
    lastOkTick_ = GetTickCount64();
    jpeg.assign(r.data.begin() + off, r.data.begin() + off + len);
    return Frame::Ok;
}

}  // namespace canon_eos_webcam