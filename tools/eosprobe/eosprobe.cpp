// eosprobe — measures the Live View of Canon EOS cameras over PTP, using only the standard Windows
// WPD/MTP driver (no additional driver).
//
// PTP sequence (the same one libgphoto2 uses for EOS):
//   SetRemoteMode(1) -> SetEventMode(1) -> GetEvent
//   SetDevicePropValueEx(EVFOutputDevice = 2 /*PC*/)
//   loop: GetViewFinderData -> blocks [u32 size][u32 type][payload], type 1 = JPEG
//   on exit: EVFOutputDevice = 0, SetEventMode(0), SetRemoteMode(0)

#include <windows.h>
#include <objbase.h>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>
#include <wincodec.h>
#include <initguid.h>  // instancia as chaves de WpdMtpExtensions.h neste arquivo
#include <WpdMtpExtensions.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static std::atomic<bool> g_stop{false};
static bool g_verbose = false;

// ---------------------------------------------------------------- utilities

static std::string W2U(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static double Now() {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

static double FtSec(const FILETIME& f) {
    return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
}

static double ProcessCpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return FtSec(k) + FtSec(u);
}

struct SysTimes { double idle = 0, total = 0; };
static SysTimes SystemTimes() {
    FILETIME i, k, u;
    GetSystemTimes(&i, &k, &u);
    return {FtSec(i), FtSec(k) + FtSec(u)};  // kernel includes idle
}

static uint32_t Rd32(const std::vector<BYTE>& d, size_t i) {
    return uint32_t(d[i]) | uint32_t(d[i + 1]) << 8 | uint32_t(d[i + 2]) << 16 | uint32_t(d[i + 3]) << 24;
}
static void Wr32(std::vector<BYTE>& d, size_t i, uint32_t v) {
    d[i] = BYTE(v); d[i + 1] = BYTE(v >> 8); d[i + 2] = BYTE(v >> 16); d[i + 3] = BYTE(v >> 24);
}

static uint64_t Fnv1a(const BYTE* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool SaveFile(const std::wstring& path, const BYTE* p, size_t n) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- PTP / WPD

enum : uint16_t {
    PTP_RC_OK = 0x2001,
    PTP_RC_DeviceBusy = 0x2019,
    PTP_RC_CANON_NOT_READY = 0xA102,

    PTP_OC_GetDeviceInfo = 0x1001,
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

struct PtpResult {
    HRESULT hr = E_FAIL;
    DWORD code = 0;
    std::vector<DWORD> params;
    std::vector<BYTE> data;
    bool ok() const { return SUCCEEDED(hr) && code == PTP_RC_OK; }
};

class PtpDevice {
public:
    HRESULT Open(const std::wstring& id) {
        ComPtr<IPortableDeviceValues> client;
        HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client));
        if (FAILED(hr)) return hr;
        client->SetStringValue(WPD_CLIENT_NAME, L"eosprobe");
        client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
        client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
        client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
        client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
        client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ | GENERIC_WRITE);
        hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dev_));
        if (FAILED(hr)) return hr;
        return dev_->Open(id.c_str(), client.Get());
    }

    void Close() {
        if (dev_) dev_->Close();
        dev_.Reset();
    }

    PtpResult Exec(WORD op, std::initializer_list<DWORD> p = {}) {
        PtpResult r;
        ComPtr<IPortableDeviceValues> in, out;
        if (FAILED(r.hr = NewCmd(WPD_COMMAND_MTP_EXT_EXECUTE_COMMAND_WITHOUT_DATA_PHASE, in))) return r;
        if (FAILED(r.hr = AddOp(in.Get(), op, p))) return r;
        r.hr = Send(in.Get(), out);
        if (out) ReadResponse(out.Get(), r);
        return r;
    }

    PtpResult Read(WORD op, std::initializer_list<DWORD> p = {}) {
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

    PtpResult Write(WORD op, std::initializer_list<DWORD> p, const std::vector<BYTE>& data) {
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
                win->SetBufferValue(WPD_PROPERTY_MTP_EXT_TRANSFER_DATA, const_cast<BYTE*>(data.data()), DWORD(data.size()));
                hrData = Send(win.Get(), wout);
            }
        }
        HRESULT hrEnd = EndTransfer(ctx, r);
        if (SUCCEEDED(r.hr)) r.hr = FAILED(hrData) ? hrData : hrEnd;
        CoTaskMemFree(ctx);
        return r;
    }

private:
    static HRESULT NewCmd(const PROPERTYKEY& cmd, ComPtr<IPortableDeviceValues>& v) {
        HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&v));
        if (FAILED(hr)) return hr;
        v->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, cmd.fmtid);
        return v->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, cmd.pid);
    }

    static HRESULT AddOp(IPortableDeviceValues* v, WORD op, std::initializer_list<DWORD> p) {
        ComPtr<IPortableDevicePropVariantCollection> col;
        HRESULT hr = CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&col));
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

    HRESULT Send(IPortableDeviceValues* in, ComPtr<IPortableDeviceValues>& out) {
        out.Reset();
        HRESULT hr = dev_->SendCommand(0, in, &out);
        if (FAILED(hr)) return hr;
        HRESULT hrCmd = S_OK;
        if (out && SUCCEEDED(out->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &hrCmd))) return hrCmd;
        return S_OK;
    }

    HRESULT EndTransfer(PCWSTR ctx, PtpResult& r) {
        ComPtr<IPortableDeviceValues> in, out;
        HRESULT hr = NewCmd(WPD_COMMAND_MTP_EXT_END_DATA_TRANSFER, in);
        if (FAILED(hr)) return hr;
        in->SetStringValue(WPD_PROPERTY_MTP_EXT_TRANSFER_CONTEXT, ctx);
        hr = Send(in.Get(), out);
        if (out) ReadResponse(out.Get(), r);
        return hr;
    }

    static void ReadResponse(IPortableDeviceValues* out, PtpResult& r) {
        DWORD code = 0;
        if (SUCCEEDED(out->GetUnsignedIntegerValue(WPD_PROPERTY_MTP_EXT_RESPONSE_CODE, &code))) r.code = code;
        ComPtr<IPortableDevicePropVariantCollection> col;
        if (SUCCEEDED(out->GetIPortableDevicePropVariantCollectionValue(WPD_PROPERTY_MTP_EXT_RESPONSE_PARAMS, &col)) && col) {
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

    ComPtr<IPortableDevice> dev_;
};

static bool Step(const char* name, const PtpResult& r) {
    bool ok = r.ok();
    if (g_verbose || !ok)
        printf("  %-36s hr=0x%08lX resp=0x%04lX  %s\n", name, (unsigned long)r.hr, (unsigned long)r.code,
               ok ? "OK" : "FAILED");
    return ok;
}

static PtpResult SetPropEx(PtpDevice& cam, uint32_t prop, uint32_t val) {
    std::vector<BYTE> d(12);
    Wr32(d, 0, 12);
    Wr32(d, 4, prop);
    Wr32(d, 8, val);
    return cam.Write(PTP_OC_CANON_EOS_SetDevicePropValueEx, {}, d);
}

// ---------------------------------------------------------------- DeviceInfo

struct DeviceInfo {
    std::string vendorExt, manufacturer, model, version, serial;
    std::vector<uint16_t> ops, props;
    bool has(uint16_t op) const { return std::find(ops.begin(), ops.end(), op) != ops.end(); }
};

struct Reader {
    const std::vector<BYTE>& d;
    size_t i = 0;
    bool fail = false;
    bool need(size_t n) { if (i + n > d.size()) fail = true; return !fail; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(d[i] | d[i + 1] << 8); i += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = Rd32(d, i); i += 4; return v; }
    std::string str() {
        if (!need(1)) return {};
        size_t n = d[i++];
        if (n == 0 || !need(n * 2)) return {};
        std::wstring w;
        for (size_t k = 0; k < n; k++) w.push_back(wchar_t(d[i + 2 * k] | d[i + 2 * k + 1] << 8));
        i += n * 2;
        while (!w.empty() && w.back() == 0) w.pop_back();
        return W2U(w.c_str());
    }
    std::vector<uint16_t> arr16() {
        uint32_t n = u32();
        std::vector<uint16_t> v;
        if (!need(size_t(n) * 2)) return v;
        for (uint32_t k = 0; k < n; k++) v.push_back(u16());
        return v;
    }
};

static bool ParseDeviceInfo(const std::vector<BYTE>& d, DeviceInfo& di) {
    Reader r{d};
    r.u16();  // StandardVersion
    r.u32();  // VendorExtensionID
    r.u16();  // VendorExtensionVersion
    di.vendorExt = r.str();
    r.u16();  // FunctionalMode
    di.ops = r.arr16();
    r.arr16();  // Events
    di.props = r.arr16();
    r.arr16();  // CaptureFormats
    r.arr16();  // ImageFormats
    di.manufacturer = r.str();
    di.model = r.str();
    di.version = r.str();
    di.serial = r.str();
    return !r.fail;
}

// ---------------------------------------------------------------- JPEG

struct JpegInfo {
    int w = 0, h = 0, comps = 0;
    bool progressive = false;
    std::string sampling;
};

static bool ParseJpeg(const BYTE* d, size_t n, JpegInfo& ji) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { i++; continue; }
        BYTE m = d[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { i += 2; continue; }
        if (m == 0xD9 || m == 0xDA) break;
        size_t len = size_t(d[i + 2]) << 8 | d[i + 3];
        if (len < 2) return false;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            const BYTE* s = d + i + 4;
            if (len < 8 || i + 2 + len > n) return false;
            ji.h = s[1] << 8 | s[2];
            ji.w = s[3] << 8 | s[4];
            ji.comps = s[5];
            ji.progressive = (m == 0xC2);
            if (ji.comps == 3 && len >= 8 + 9) {
                int yh = s[7] >> 4, yv = s[7] & 15, ch = s[10] >> 4, cv = s[10] & 15;
                if (ch == 1 && cv == 1) {
                    if (yh == 2 && yv == 1) ji.sampling = "4:2:2";
                    else if (yh == 2 && yv == 2) ji.sampling = "4:2:0";
                    else if (yh == 1 && yv == 1) ji.sampling = "4:4:4";
                }
                if (ji.sampling.empty()) {
                    char b[32];
                    snprintf(b, sizeof b, "Y%dx%d C%dx%d", yh, yv, ch, cv);
                    ji.sampling = b;
                }
            } else if (ji.comps == 1) {
                ji.sampling = "grayscale";
            }
            return true;
        }
        i += 2 + len;
    }
    return false;
}

// Splits the GetViewFinderData blocks and finds the JPEG (block type 1).
static bool ExtractJpeg(const std::vector<BYTE>& d, size_t& off, size_t& len, std::vector<uint32_t>* types) {
    bool found = false;
    size_t i = 0;
    while (i + 8 <= d.size()) {
        uint32_t sz = Rd32(d, i), ty = Rd32(d, i + 4);
        if (sz < 8 || i + sz > d.size()) break;
        if (types) types->push_back(ty);
        if (ty == 1 && !found && sz > 10 && d[i + 8] == 0xFF && d[i + 9] == 0xD8) {
            off = i + 8;
            len = sz - 8;
            found = true;
        }
        i += sz;
    }
    if (!found) {  // unexpected format: look for SOI/EOI directly
        for (size_t k = 0; k + 1 < d.size(); k++) {
            if (d[k] == 0xFF && d[k + 1] == 0xD8) {
                off = k;
                len = d.size() - k;
                for (size_t e = d.size(); e >= k + 4; e--)
                    if (d[e - 2] == 0xFF && d[e - 1] == 0xD9) { len = e - k; break; }
                found = true;
                break;
            }
        }
    }
    return found;
}

// Average time (ms) to decode the JPEG to BGRA with WIC (the Windows decoder).
static double BenchDecode(const std::vector<BYTE>& jpg, int iters) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return -1;
    std::vector<BYTE> px;
    int ok = 0;
    double t0 = Now();
    for (int it = 0; it < iters; it++) {
        ComPtr<IWICStream> s;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICFormatConverter> cv;
        if (FAILED(f->CreateStream(&s))) break;
        if (FAILED(s->InitializeFromMemory(const_cast<BYTE*>(jpg.data()), DWORD(jpg.size())))) break;
        if (FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        cv->GetSize(&w, &h);
        px.resize(size_t(w) * h * 4);
        if (FAILED(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) break;
        ok++;
    }
    return ok ? (Now() - t0) / ok * 1000.0 : -1;
}

// ---------------------------------------------------------------- device

struct WpdDevice {
    std::wstring id;
    std::string name, manufacturer;
};

static std::vector<WpdDevice> ListDevices() {
    std::vector<WpdDevice> out;
    ComPtr<IPortableDeviceManager> mgr;
    if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
        return out;
    DWORD n = 0;
    if (FAILED(mgr->GetDevices(nullptr, &n)) || n == 0) return out;
    std::vector<PWSTR> ids(n);
    if (FAILED(mgr->GetDevices(ids.data(), &n))) return out;
    for (DWORD i = 0; i < n; i++) {
        WpdDevice d;
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
        out.push_back(d);
        CoTaskMemFree(ids[i]);
    }
    return out;
}

static bool ContainsI(const std::string& hay, const std::string& needle) {
    auto low = [](std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; };
    return low(hay).find(low(needle)) != std::string::npos;
}

static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

static void Usage() {
    printf(
        "usage: eosprobe [--seconds N] [--out FOLDER] [--device TEXT] [--verbose]\n"
        "  --seconds N    length of the measurement (default 10)\n"
        "  --out FOLDER   where to save the sample frames (default .\\eosprobe-out)\n"
        "  --device TEXT  picks the WPD device whose name contains TEXT (default: Canon/EOS)\n"
        "  --verbose      shows every PTP command\n");
}

// ---------------------------------------------------------------- main

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    double seconds = 10;
    std::wstring outDir = L"eosprobe-out";
    std::string devFilter;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (a == L"--out" && i + 1 < argc) outDir = argv[++i];
        else if (a == L"--device" && i + 1 < argc) devFilter = W2U(argv[++i]);
        else if (a == L"--verbose" || a == L"-v") g_verbose = true;
        else { Usage(); return a == L"--help" || a == L"-h" ? 0 : 2; }
    }
    if (seconds <= 0) seconds = 10;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { printf("CoInitializeEx failed\n"); return 1; }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // 1. Find the camera
    auto devs = ListDevices();
    printf("WPD devices found: %zu\n", devs.size());
    const WpdDevice* sel = nullptr;
    for (auto& d : devs) {
        bool match = devFilter.empty() ? (ContainsI(d.manufacturer, "canon") || ContainsI(d.name, "eos"))
                                       : ContainsI(d.name, devFilter);
        printf("  %s %s (%s)\n", match && !sel ? "->" : "  ", d.name.c_str(), d.manufacturer.c_str());
        if (match && !sel) sel = &d;
    }
    if (!sel) { printf("\nNo Canon camera found. Is it turned on and connected?\n"); return 1; }

    PtpDevice cam;
    HRESULT hr = cam.Open(sel->id);
    if (FAILED(hr)) {
        printf("\nFailed to open the device (hr=0x%08lX). Close other programs that use the camera.\n", (unsigned long)hr);
        return 1;
    }

    // 2. DeviceInfo
    DeviceInfo di;
    auto rdi = cam.Read(PTP_OC_GetDeviceInfo);
    bool haveInfo = Step("GetDeviceInfo", rdi) && ParseDeviceInfo(rdi.data, di);
    if (haveInfo) {
        printf("\nCamera: %s %s  (firmware %s)\n", di.manufacturer.c_str(), di.model.c_str(), di.version.c_str());
        printf("  Supported PTP operations: %zu, properties: %zu\n", di.ops.size(), di.props.size());
        struct { uint16_t op; const char* name; } need[] = {
            {PTP_OC_CANON_EOS_SetRemoteMode, "SetRemoteMode"},
            {PTP_OC_CANON_EOS_SetEventMode, "SetEventMode"},
            {PTP_OC_CANON_EOS_SetDevicePropValueEx, "SetDevicePropValueEx"},
            {PTP_OC_CANON_EOS_GetViewFinderData, "GetViewFinderData"},
        };
        for (auto& n : need) printf("  0x%04X %-22s %s\n", n.op, n.name, di.has(n.op) ? "sim" : "NÃO");
    }

    // 3. Start Live View to the PC
    printf("\nStarting Live View...\n");
    Step("SetRemoteMode(1)", cam.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {1}));
    Step("SetEventMode(1)", cam.Exec(PTP_OC_CANON_EOS_SetEventMode, {1}));
    auto ev = cam.Read(PTP_OC_CANON_EOS_GetEvent);
    Step("GetEvent", ev);
    if (g_verbose) printf("  (GetEvent: %zu bytes)\n", ev.data.size());
    {
        auto r = SetPropEx(cam, PTP_DPC_CANON_EOS_EVFMode, 1);
        if (g_verbose) Step("EVFMode = 1 (optional)", r);
    }
    Step("EVFOutputDevice = 2 (PC)", SetPropEx(cam, PTP_DPC_CANON_EOS_EVFOutputDevice, 2));

    auto stopLiveView = [&] {
        printf("\nStopping Live View...\n");
        Step("EVFOutputDevice = 0", SetPropEx(cam, PTP_DPC_CANON_EOS_EVFOutputDevice, 0));
        Step("SetEventMode(0)", cam.Exec(PTP_OC_CANON_EOS_SetEventMode, {0}));
        Step("SetRemoteMode(0)", cam.Exec(PTP_OC_CANON_EOS_SetRemoteMode, {0}));
        cam.Close();
        CoUninitialize();
    };

    // 4. Wait for the first frame
    std::vector<BYTE> firstJpeg;
    std::vector<uint32_t> blockTypes;
    PtpResult last;
    double tWait = Now();
    while (!g_stop && Now() - tWait < 8.0) {
        last = cam.Read(PTP_OC_CANON_EOS_GetViewFinderData, {0x00100000, 0, 0});
        size_t off = 0, len = 0;
        if (last.ok() && ExtractJpeg(last.data, off, len, &blockTypes)) {
            firstJpeg.assign(last.data.begin() + off, last.data.begin() + off + len);
            break;
        }
        blockTypes.clear();
        Sleep(100);
        if (Now() - tWait > 3.0 && int((Now() - tWait) * 10) % 10 == 0) cam.Read(PTP_OC_CANON_EOS_GetEvent);
    }
    if (firstJpeg.empty()) {
        printf("\nNo Live View frame received (last response hr=0x%08lX resp=0x%04lX).\n",
               (unsigned long)last.hr, (unsigned long)last.code);
        printf("Check: photo mode (P/Av/Tv/M, not the automatic/movie mode), Live View enabled in the menu,\n"
               "no other program using the camera. Run with --verbose for details.\n");
        stopLiveView();
        return 1;
    }
    printf("First frame received after %.0f ms.\n", (Now() - tWait) * 1000);

    JpegInfo ji;
    ParseJpeg(firstJpeg.data(), firstJpeg.size(), ji);
    CreateDirectoryW(outDir.c_str(), nullptr);
    SaveFile(outDir + L"\\first.jpg", firstJpeg.data(), firstJpeg.size());
    SaveFile(outDir + L"\\evf-raw.bin", last.data.data(), last.data.size());

    // 5. Measurement
    printf("Measuring for %.0f s (Ctrl+C to stop earlier)...\n", seconds);
    size_t calls = 0, okFrames = 0, distinct = 0, notReady = 0, busy = 0, errors = 0, consecErrors = 0;
    double latSum = 0, latMin = 1e9, latMax = 0;
    size_t bytesSum = 0, jpgSum = 0, jpgMin = SIZE_MAX, jpgMax = 0;
    uint64_t prevHash = 0;
    std::map<std::pair<int, int>, size_t> sizesSeen;
    std::vector<BYTE> lastJpeg;
    double lastEvent = Now();

    double cpu0 = ProcessCpuSeconds();
    SysTimes sys0 = SystemTimes();
    double t0 = Now(), tPrint = t0;
    while (!g_stop && Now() - t0 < seconds) {
        double a = Now();
        auto r = cam.Read(PTP_OC_CANON_EOS_GetViewFinderData, {0x00100000, 0, 0});
        double dt = Now() - a;
        calls++;
        if (!r.ok()) {
            if (r.code == PTP_RC_CANON_NOT_READY) notReady++;
            else if (r.code == PTP_RC_DeviceBusy) busy++;
            else {
                errors++;
                if (g_verbose) Step("GetViewFinderData", r);
                if (++consecErrors > 50) { printf("Too many errors in a row, aborting.\n"); break; }
            }
            continue;
        }
        consecErrors = 0;
        size_t off = 0, len = 0;
        if (!ExtractJpeg(r.data, off, len, nullptr)) { errors++; continue; }
        okFrames++;
        latSum += dt;
        latMin = std::min(latMin, dt);
        latMax = std::max(latMax, dt);
        bytesSum += r.data.size();
        jpgSum += len;
        jpgMin = std::min(jpgMin, len);
        jpgMax = std::max(jpgMax, len);
        uint64_t h = Fnv1a(r.data.data() + off, len);
        if (h != prevHash) { distinct++; prevHash = h; }
        JpegInfo fji;
        if (ParseJpeg(r.data.data() + off, len, fji)) sizesSeen[{fji.w, fji.h}]++;
        lastJpeg.assign(r.data.begin() + off, r.data.begin() + off + len);

        if (Now() - lastEvent > 1.0) {  // keeps the camera event queue empty
            cam.Read(PTP_OC_CANON_EOS_GetEvent);
            lastEvent = Now();
        }
        if (Now() - tPrint > 1.0) {
            double el = Now() - t0;
            printf("\r  %4.1f s  distinct frames: %zu (%.1f fps)   ", el, distinct, distinct / el);
            fflush(stdout);
            tPrint = Now();
        }
    }
    double elapsed = Now() - t0;
    double cpuProc = ProcessCpuSeconds() - cpu0;
    SysTimes sys1 = SystemTimes();
    printf("\n");

    if (!lastJpeg.empty()) SaveFile(outDir + L"\\last.jpg", lastJpeg.data(), lastJpeg.size());
    double decMs = BenchDecode(firstJpeg, 100);

    stopLiveView();

    // 6. Report
    int ncpu = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    double sysTotal = sys1.total - sys0.total, sysIdle = sys1.idle - sys0.idle;
    double sysBusy = sysTotal > 0 ? (sysTotal - sysIdle) / sysTotal * 100 : 0;

    printf("\n================= RESULT =================\n");
    if (haveInfo) printf("Camera ............... %s (fw %s)\n", di.model.c_str(), di.version.c_str());
    printf("Live View resolution . %d x %d", ji.w, ji.h);
    if (!ji.sampling.empty()) printf("  (JPEG %s%s)", ji.sampling.c_str(), ji.progressive ? ", progressive" : "");
    printf("\n");
    if (sizesSeen.size() > 1) {
        printf("  (different resolutions seen during the measurement:");
        for (auto& [k, v] : sizesSeen) printf(" %dx%d×%zu", k.first, k.second, v);
        printf(")\n");
    }
    printf("Blocks in EVF packet . ");
    for (auto t : blockTypes) printf("type %u  ", t);
    printf("\n");
    if (okFrames) {
        printf("JPEG size ............ average %.1f KB  (min %.1f, max %.1f)\n", jpgSum / 1024.0 / okFrames,
               jpgMin / 1024.0, jpgMax / 1024.0);
        printf("Latency per request .. average %.1f ms  (min %.1f, max %.1f)\n", latSum / okFrames * 1000,
               latMin * 1000, latMax * 1000);
    }
    printf("Requests ............. %zu in %.1f s  (ok %zu, not-ready %zu, busy %zu, error %zu)\n", calls, elapsed,
           okFrames, notReady, busy, errors);
    printf("Distinct frames ...... %zu  =>  %.1f real fps\n", distinct, distinct / elapsed);
    printf("Useful USB rate ...... %.2f MB/s\n", bytesSum / elapsed / 1048576.0);
    printf("CPU of this process .. %.1f%% of a core (%.2f%% of the total of %d logical cores)\n",
           cpuProc / elapsed * 100, cpuProc / elapsed * 100 / ncpu, ncpu);
    printf("Whole-system CPU ..... %.1f%% (includes the WPD driver host and everything else running)\n", sysBusy);
    if (decMs >= 0)
        printf("Decoding (WIC) ....... %.2f ms per frame => %.1f%% of a core at 30 fps\n", decMs, decMs * 30 / 10);
    printf("Frames saved in ...... %s\\ (first.jpg, last.jpg, evf-raw.bin)\n", W2U(outDir.c_str()).c_str());
    return 0;
}
