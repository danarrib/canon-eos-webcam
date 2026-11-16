// canon-eos-webcam.exe — bridge between the Canon EOS (PTP/WPD) and the Windows 11 virtual camera.
//
//   canon-eos-webcam run         keeps running: turns Live View on only when some program uses the webcam
//   canon-eos-webcam panel       opens the control panel (of the running canon-eos-webcam-bg; starts it if needed)
//   canon-eos-webcam install     creates the virtual camera "Canon EOS Webcam" (persistent)
//   canon-eos-webcam uninstall   removes the virtual camera
//   canon-eos-webcam test        opens the virtual camera like a regular app and measures frames/s and CPU
//   canon-eos-webcam process     runs a JPEG through the image pipeline (no camera) and measures the timings
//
// Logs and diagnostic output are in English; the everyday commands speak the user's language (i18n.h).

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "applog.h"
#include "autostart.h"
#include "bridge.h"
#include "canon_camera.h"
#include "color.h"
#include "i18n.h"
#include "panel.h"
#include "pipeline.h"
#include "settings.h"
#include "shared.h"

using Microsoft::WRL::ComPtr;
using namespace canon_eos_webcam;

static std::atomic<bool> g_stop{false};

#ifdef CANON_EOS_WEBCAM_BACKGROUND
constexpr bool kWithUi = true;
#else
constexpr bool kWithUi = false;
#endif

static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

#define Out canon_eos_webcam::Log

static double FtSec(const FILETIME& f) { return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; }

// =====================================================================================
// run

// Only one producer at a time (canon-eos-webcam run or canon-eos-webcam-bg), otherwise both fight over the camera.
static bool AcquireSingleInstance(HANDLE& single, HANDLE& stopEvent) {
    single = CreateMutexW(nullptr, TRUE, L"Local\\CanonEosWebcamProducer");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        Out("%s", Tr(Str::CliOtherInstance));
        CloseHandle(single);
        single = nullptr;
        return false;
    }
    // "canon-eos-webcam stop" signals this event to shut down with Live View properly turned off.
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\CanonEosWebcamStop");
    if (stopEvent) ResetEvent(stopEvent);
    return true;
}

// withUi: in canon-eos-webcam-bg the main thread runs the tray icon and the panel, and the bridge gets its own thread.
static int CmdRun(bool withUi) {
    HANDLE single = nullptr, stopEvent = nullptr;
    if (!AcquireSingleInstance(single, stopEvent)) return 1;

    SettingsStore store;
    store.Load(SettingsStore::DefaultPath());
    Out("Settings in %S", store.Path().c_str());
    Bridge bridge(store);

    int rc = 0;
    if (!withUi) {
        rc = bridge.Run(stopEvent, &g_stop);
    } else {
        PanelApp app(bridge, store);
        std::thread worker([&] {
            bridge.Run(stopEvent, &g_stop);
            app.RequestQuit();  // "canon-eos-webcam stop", Ctrl+C or "Exit" in the menu: closes the window too
        });
        rc = app.Run();
        bridge.RequestStop();
        worker.join();
    }
    store.FlushNow();
    if (stopEvent) CloseHandle(stopEvent);
    if (single) CloseHandle(single);
    return rc;
}

// =====================================================================================
// start / stop / autostart (background version: canon-eos-webcam-bg.exe)

static bool BackgroundRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\CanonEosWebcamProducer");
    if (!m) return false;
    CloseHandle(m);
    return true;
}

static int CmdStart() {
    if (BackgroundRunning()) { printf("%s", Tr(Str::CliAlreadyRunning)); return 0; }
    std::wstring exe = ExeDirectory() + L"canon-eos-webcam-bg.exe";
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si,
                        &pi)) {
        printf(Tr(Str::CliStartFailed), W2U(exe.c_str()).c_str(), GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf(Tr(Str::CliStarted), W2U(kDataDir).c_str());
    return 0;
}

static int CmdStop() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\CanonEosWebcamStop");
    if (!ev) { printf("%s", Tr(Str::CliNotRunning)); return 0; }
    SetEvent(ev);
    CloseHandle(ev);
    for (int i = 0; i < 50 && BackgroundRunning(); i++) Sleep(100);
    printf("%s", Tr(BackgroundRunning() ? Str::CliStopTimeout : Str::CliStopped));
    return 0;
}

static int CmdAutostart(const std::wstring& arg) {
    if (arg == L"on") {
        LSTATUS s = SetAutostart(true);
        if (s != ERROR_SUCCESS) { printf(Tr(Str::CliRegWriteFailed), (long)s); return 1; }
        printf(Tr(Str::CliAutostartOn), W2U(AutostartCommand().c_str()).c_str());
        return CmdStart();
    }
    if (arg == L"off") {
        LSTATUS s = SetAutostart(false);
        if (s != ERROR_SUCCESS) {
            printf(Tr(Str::CliRegDeleteFailed), (long)s);
            return 1;
        }
        printf("%s", Tr(Str::CliAutostartOff));
        return 0;
    }
    printf("%s", Tr(Str::CliAutostartUsage));
    return 2;
}

// =====================================================================================
// status

static int CmdList() {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
    if (FAILED(hr)) { printf("MFEnumDeviceSources failed (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
    for (UINT32 i = 0; i < n; i++) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        printf("  %s\n", W2U(name).c_str());
        CoTaskMemFree(name);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return 0;
}

static int CmdStatus() {
    SharedChannel chan;
    DWORD err = 0;
    if (!chan.Open(false, &err)) {
        printf("Channel %s does not exist (error %lu): no program has opened the virtual webcam yet.\n",
               W2U(MappingName()).c_str(), err);
        return 1;
    }
    for (int i = 0; i < 5 && !g_stop; i++) {
        SharedHeader& h = chan.Header();
        uint64_t now = NowTick();
        printf("frame #%lld  slot %ld  producer state %ld (%lld ms ago)  consumer %ldx%ld (%lld ms ago)\n",
               (long long)h.frameNumber, (long)h.latestSlot, (long)h.producerState,
               h.producerTick ? (long long)(now - uint64_t(h.producerTick)) : -1LL, (long)h.consumerWidth,
               (long)h.consumerHeight, h.consumerTick ? (long long)(now - uint64_t(h.consumerTick)) : -1LL);
        Sleep(500);
    }
    return 0;
}

// =====================================================================================
// install / uninstall

// Name of the virtual camera created by the versions released under the old name (eoscam).
static constexpr wchar_t kLegacyCameraName[] = L"Canon EOS Webcam (eoscam)";

static HRESULT CreateVCam(ComPtr<IMFVirtualCamera>& vc, const wchar_t* name = kCameraName) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_CurrentUser, name, CANON_EOS_WEBCAM_CLSID_STRING, nullptr, 0,
                                 &vc);
}

static int CmdInstall() {
    ComPtr<IMFVirtualCamera> vc;
    // An upgrade from the old name leaves a second camera behind; remove it (no-op if it doesn't exist).
    if (SUCCEEDED(CreateVCam(vc, kLegacyCameraName)) && SUCCEEDED(vc->Remove()))
        Out("Removed the virtual camera of the old version (\"%S\").", kLegacyCameraName);
    vc.Reset();
    HRESULT hr = CreateVCam(vc);
    if (FAILED(hr)) {
        printf(Tr(Str::CliInstallFailed), (unsigned long)hr);
        if (hr == REGDB_E_CLASSNOTREG) printf("%s", Tr(Str::CliDllNotRegistered));
        return 1;
    }
    hr = vc->Start(nullptr);
    if (FAILED(hr)) {
        printf(Tr(Str::CliVcamStartFailed), (unsigned long)hr);
        return 1;
    }
    printf(Tr(Str::CliInstalled), W2U(kCameraName).c_str());
    return 0;
}

static int CmdUninstall() {
    ComPtr<IMFVirtualCamera> vc;
    HRESULT hr = CreateVCam(vc);
    if (SUCCEEDED(hr)) hr = vc->Remove();
    if (FAILED(hr)) {
        printf(Tr(Str::CliUninstallFailed), (unsigned long)hr);
        return 1;
    }
    printf("%s", Tr(Str::CliUninstalled));
    return 0;
}

// =====================================================================================
// test

static std::string SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    char b[16];
    snprintf(b, sizeof b, "%08lX", g.Data1);
    return b;
}

static bool SaveBgraAsJpeg(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    if (FAILED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4,
                                         const_cast<BYTE*>(bgra), &bmp)))
        return false;
    if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

static int TestWithActivate(IMFActivate* sel, double seconds, UINT wantW, UINT wantH, const std::wstring& outPath);

// Loads canon-eos-webcam-source.dll (next to the exe) straight into this process, without registration or Frame Server.
static HRESULT ActivateDirect(ComPtr<IMFActivate>& act) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L'\\') + 1) + L"canon-eos-webcam-source.dll";
    HMODULE m = LoadLibraryW(p.c_str());
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto fn = reinterpret_cast<GetClassObjectFn>(GetProcAddress(m, "DllGetClassObject"));
    if (!fn) return E_NOINTERFACE;
    ComPtr<IClassFactory> cf;
    HRESULT hr = fn(kSourceClsid, IID_PPV_ARGS(&cf));
    if (FAILED(hr)) return hr;
    return cf->CreateInstance(nullptr, IID_PPV_ARGS(&act));
}

static int CmdTest(double seconds, UINT wantW, UINT wantH, const std::wstring& outPath, bool direct) {
    if (direct) {
        ComPtr<IMFActivate> act;
        HRESULT hr = ActivateDirect(act);
        if (FAILED(hr)) { printf("Failed to load the DLL directly (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
        printf("Direct mode: source loaded in this process (channel %s).\n", W2U(MappingName()).c_str());
        return TestWithActivate(act.Get(), seconds, wantW, wantH, outPath);
    }

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
    if (FAILED(hr)) { printf("MFEnumDeviceSources failed (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("Video cameras found:\n");
    ComPtr<IMFActivate> sel;
    for (UINT32 i = 0; i < n; i++) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        // Windows appends " (Windows Virtual Camera)" to the name (localized)
        bool match = name && wcsncmp(name, kCameraName, wcslen(kCameraName)) == 0 &&
                     wcsncmp(name, kLegacyCameraName, wcslen(kLegacyCameraName)) != 0;
        printf("  %s %s\n", match ? "->" : "  ", W2U(name).c_str());
        if (match && !sel) sel = devs[i];
        CoTaskMemFree(name);
    }
    for (UINT32 i = 0; i < n; i++) devs[i]->Release();
    CoTaskMemFree(devs);
    if (!sel) { printf("\nThe virtual camera did not show up. Run \"canon-eos-webcam install\" first.\n"); return 1; }
    return TestWithActivate(sel.Get(), seconds, wantW, wantH, outPath);
}

static int TestWithActivate(IMFActivate* sel, double seconds, UINT wantW, UINT wantH, const std::wstring& outPath) {
    ComPtr<IMFMediaSource> src;
    HRESULT hr = sel->ActivateObject(IID_PPV_ARGS(&src));
    if (FAILED(hr)) { printf("Failed to open the virtual camera (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    if (FAILED(hr)) { printf("MFCreateSourceReaderFromMediaSource failed (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("\nFormats offered:\n");
    ComPtr<IMFMediaType> chosen;
    for (DWORD k = 0;; k++) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), k, &t))) break;
        GUID sub{};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        bool pick = !chosen && w == wantW && h == wantH;
        printf("  %s %s %ux%u @ %.0f fps\n", pick ? "->" : "  ", SubtypeName(sub).c_str(), w, h,
               den ? double(num) / den : 0.0);
        if (pick) chosen = t;
    }
    if (!chosen) { printf("Format %ux%u not offered.\n", wantW, wantH); return 1; }
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, chosen.Get());
    if (FAILED(hr)) { printf("SetCurrentMediaType failed (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("\nReading frames for %.0f s (Live View takes a few seconds to start)...\n", seconds);
    FILETIME c, e, k0, u0, k1, u1, si0, sk0, su0, si1, sk1, su1;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
    GetSystemTimes(&si0, &sk0, &su0);
    uint64_t t0 = NowTick();
    uint64_t frames = 0, distinct = 0, prevHash = 0, firstLiveTick = 0;
    std::vector<uint8_t> nv12, last;
    while (!g_stop && NowTick() - t0 < uint64_t(seconds * 1000)) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) { printf("ReadSample failed (hr=0x%08lX)\n", (unsigned long)hr); break; }
        if (!sample) continue;
        frames++;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
        uint64_t h = 1469598103934665603ull;
        for (DWORD i = 0; i < len; i += 61) { h ^= p[i]; h *= 1099511628211ull; }
        if (h != prevHash) { distinct++; prevHash = h; }
        last.assign(p, p + len);
        buf->Unlock();
        if (!firstLiveTick && distinct > 3) firstLiveTick = NowTick();
    }
    uint64_t elapsedMs = NowTick() - t0;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
    GetSystemTimes(&si1, &sk1, &su1);
    src->Shutdown();

    double secs = elapsedMs / 1000.0;
    double sysTotal = (FtSec(sk1) + FtSec(su1)) - (FtSec(sk0) + FtSec(su0));
    double sysIdle = FtSec(si1) - FtSec(si0);
    double proc = (FtSec(k1) + FtSec(u1)) - (FtSec(k0) + FtSec(u0));
    printf("\n================= RESULT =================\n");
    printf("Format ............... NV12 %ux%u\n", wantW, wantH);
    printf("Samples received ..... %llu in %.1f s (%.1f/s)\n", (unsigned long long)frames, secs, frames / secs);
    printf("Distinct frames ...... %llu (%.1f/s)\n", (unsigned long long)distinct, distinct / secs);
    if (firstLiveTick) printf("Live image after ..... %.1f s\n", (firstLiveTick - t0) / 1000.0);
    printf("CPU of this test ..... %.1f%% of a core\n", proc / secs * 100);
    printf("Whole-system CPU ..... %.1f%%\n", sysTotal > 0 ? (sysTotal - sysIdle) / sysTotal * 100 : 0.0);

    if (last.size() >= size_t(wantW) * wantH * 3 / 2) {
        std::vector<uint8_t> bgra(size_t(wantW) * wantH * 4);
        Nv12ToBgra(last.data(), int(wantW), int(wantH), bgra.data());
        if (SaveBgraAsJpeg(outPath, bgra.data(), wantW, wantH))
            printf("Last frame saved to %s\n", W2U(outPath.c_str()).c_str());
    }
    return 0;
}

// =====================================================================================
// process: image pipeline over a JPEG (no camera), to test effects and measure the timings

static std::vector<uint8_t> ReadWholeFile(const std::wstring& path) {
    std::vector<uint8_t> d;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (256ll << 20)) {
        d.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, d.data(), DWORD(d.size()), &got, nullptr) || got != d.size()) d.clear();
    }
    CloseHandle(h);
    return d;
}

static const char* FxStateName(FxState s) {
    switch (s) {
        case FxState::Off: return "off";
        case FxState::Loading: return "loading";
        case FxState::Active: return "active";
        case FxState::Failed: return "FAILED";
        case FxState::Unavailable: return "unavailable";
    }
    return "?";
}

// Kicks off the model loads (in the background) and processes the same frame until every requested effect
// is active (or has failed); then warms up the temporal state (Denoise, Green Screen).
static bool WarmUpPipeline(FramePipeline& pipe, const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& nv12) {
    pipe.Prewarm();
    uint64_t t0 = NowTick();
    for (;;) {
        if (!pipe.Process(jpeg.data(), jpeg.size(), nv12.data())) return false;
        PipelineStats st = pipe.Stats();
        bool pending = st.denoise.state == FxState::Loading || st.artifact.state == FxState::Loading ||
                       st.superRes.state == FxState::Loading || st.greenScreen.state == FxState::Loading ||
                       st.blur.state == FxState::Loading;
        if (!pending || NowTick() - t0 > 60000) break;
        Sleep(20);
    }
    printf("Effects loaded in %.1f s\n", double(NowTick() - t0) / 1000.0);
    for (int i = 0; i < 15; i++) pipe.Process(jpeg.data(), jpeg.size(), nv12.data());
    return true;
}

static int CmdProcess(const std::wstring& inPath, const std::wstring& outPath, int bench) {
    if (inPath.empty()) {
        printf("usage: canon-eos-webcam process input.jpg [--out output.jpg] [--bench N]\n");
        return 2;
    }
    std::vector<uint8_t> jpeg = ReadWholeFile(inPath);
    if (jpeg.empty()) { printf("Couldn't read %s\n", W2U(inPath.c_str()).c_str()); return 1; }

    SettingsStore store;
    store.Load(SettingsStore::DefaultPath());
    printf("Settings: %s\n", W2U(store.Path().c_str()).c_str());
    FramePipeline pipe(store);
    if (!pipe.Init()) { printf("Failed to start the pipeline.\n"); return 1; }
    std::vector<uint8_t> nv12(kFrameBytes);

    if (!WarmUpPipeline(pipe, jpeg, nv12)) { printf("Failed to process the frame.\n"); return 1; }

    if (bench > 0) {
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        uint64_t b0 = NowTick();
        for (int i = 0; i < bench; i++) pipe.Process(jpeg.data(), jpeg.size(), nv12.data());
        double secs = double(NowTick() - b0) / 1000.0;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        double cpu = (FtSec(k1) + FtSec(u1)) - (FtSec(k0) + FtSec(u0));
        printf("Benchmark: %d frames in %.2f s = %.1f frames/s (%.1f ms per frame); CPU %.1f%% of a core\n",
               bench, secs, bench / secs, secs * 1000.0 / bench, cpu / secs * 100.0);
    }
    PipelineStats st = pipe.Stats();
    if (st.maxineAvailable) printf("Maxine: available (%s)\n", st.maxineVersion.c_str());
    else printf("Maxine: UNAVAILABLE (%s)\n", Text(st.maxineReason, Lang::En).c_str());
    auto fx = [](const FxInfo& i) { return FxText(i, Lang::En); };
    printf("  Denoise ............ %s (%s)\n", FxStateName(st.denoise.state), fx(st.denoise).c_str());
    printf("  Artifact Reduction . %s (%s)\n", FxStateName(st.artifact.state), fx(st.artifact).c_str());
    printf("  Super Resolution ... %s (%s)\n", FxStateName(st.superRes.state), fx(st.superRes).c_str());
    printf("  Green Screen ....... %s (%s)\n", FxStateName(st.greenScreen.state), fx(st.greenScreen).c_str());
    printf("  SDK blur ........... %s (%s)\n", FxStateName(st.blur.state), fx(st.blur).c_str());
    if (!st.backgroundNote.empty()) printf("  Background: %s\n", Text(st.backgroundNote, Lang::En).c_str());
    printf("Time per frame (ms, moving average): decode %.2f | GPU: upload %.2f, denoise %.2f, artifacts %.2f, "
           "super resolution %.2f, mask/blur %.2f, download %.2f | CPU: scale+adjust+background+NV12 %.2f | total %.2f\n",
           st.msDecode, st.msUpload, st.msDenoise, st.msArtifact, st.msSuperRes, st.msSegment, st.msDownload,
           st.msFinal, st.msTotal);

    std::vector<uint8_t> bgra(size_t(kFrameWidth) * kFrameHeight * 4);
    Nv12ToBgra(nv12.data(), int(kFrameWidth), int(kFrameHeight), bgra.data());
    if (SaveBgraAsJpeg(outPath, bgra.data(), kFrameWidth, kFrameHeight))
        printf("Frame saved to %s\n", W2U(outPath.c_str()).c_str());
    return 0;
}

// =====================================================================================
// cam: diagnostics of the camera settings (PTP) with the bridge stopped

static void PrintCameraProps(CanonCamera& cam) {
    for (auto& [prop, st] : cam.AllProps()) {
        printf("  %04X", prop);
        if (st.hasValue) printf(" value=0x%X", st.value);
        if (st.hasList) {
            printf(" list(%zu):", st.list.size());
            for (uint32_t v : st.list) printf(" %X", v);
        }
        if (st.raw.size() > 4) {
            printf(" bytes(%zu):", st.raw.size());
            for (size_t i = 0; i < st.raw.size() && i < 48; i++) printf(" %02X", st.raw[i]);
        }
        printf("\n");
    }
}

// Connects, shows the properties the camera announces, applies --set PROP=VALUE (hexadecimal) one at a time
// and, after each one, saves a frame processed by the pipeline (to compare the effect on the image).
static int CmdCam(const std::vector<std::wstring>& sets, const std::wstring& outPath) {
    if (BackgroundRunning()) {
        printf("Stop canon-eos-webcam first (canon-eos-webcam stop): only one program can talk to the camera.\n");
        return 1;
    }
    CanonCamera cam;
    std::string err;
    if (!cam.Connect(err)) { printf("Camera unavailable: %s\n", err.c_str()); return 1; }
    printf("Camera: %s\n", cam.Name().c_str());

    SettingsStore store;
    store.Load(SettingsStore::DefaultPath());
    FramePipeline pipe(store);
    pipe.Init();
    std::vector<uint8_t> jpeg, nv12(kFrameBytes), bgra(size_t(kFrameWidth) * kFrameHeight * 4);

    auto grab = [&](DWORD ms) {
        uint64_t t0 = NowTick();
        bool got = false;
        std::vector<uint8_t> j;
        while (NowTick() - t0 < ms) {
            if (cam.GetFrame(j) == CanonCamera::Frame::Ok) { jpeg = j; got = true; }
            else Sleep(5);
        }
        return got;
    };
    auto save = [&](const std::wstring& path) {
        if (jpeg.empty() || !WarmUpPipeline(pipe, jpeg, nv12)) return;
        Nv12ToBgra(nv12.data(), int(kFrameWidth), int(kFrameHeight), bgra.data());
        double sy = 0, su = 0, sv = 0;
        size_t ny = size_t(kFrameWidth) * kFrameHeight;
        for (size_t i = 0; i < ny; i += 7) sy += nv12[i];
        for (size_t i = 0; i < ny / 2; i += 2) { su += nv12[ny + i]; sv += nv12[ny + i + 1]; }
        printf("  average luma %.1f, chroma U %.1f V %.1f\n", sy / double((ny + 6) / 7), su / double(ny / 4),
               sv / double(ny / 4));
        if (SaveBgraAsJpeg(path, bgra.data(), kFrameWidth, kFrameHeight))
            printf("  frame saved to %s\n", W2U(path.c_str()).c_str());
    };

    grab(2500);
    cam.PollEvents();
    printf("Properties announced by the camera:\n");
    PrintCameraProps(cam);
    std::wstring base = outPath;
    size_t dot = base.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? base : base.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos ? L".jpg" : base.substr(dot);
    save(stem + L"-0" + ext);

    int n = 0;
    for (auto& a : sets) {
        // PROP=VALUE or PROP=P0,P1,... (structured property, 32-bit words; negative values with a leading -)
        unsigned prop = 0;
        size_t eq = a.find(L'=');
        if (eq == std::wstring::npos || swscanf_s(a.c_str(), L"%x", &prop) != 1) {
            printf("--set PROP=VALUE (hexadecimal)\n");
            continue;
        }
        std::vector<uint32_t> words;
        std::wstring rest = a.substr(eq + 1);
        for (size_t pos = 0; pos <= rest.size();) {
            size_t c = rest.find(L',', pos);
            std::wstring tok = rest.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos);
            bool neg = !tok.empty() && tok[0] == L'-';
            words.push_back(neg ? uint32_t(-int(wcstol(tok.c_str() + 1, nullptr, 16))) : uint32_t(wcstoul(tok.c_str(), nullptr, 16)));
            if (c == std::wstring::npos) break;
            pos = c + 1;
        }
        PtpResult r = words.size() == 1 ? cam.WriteProp(prop, words[0]) : cam.WritePropWords(prop, words);
        printf("Setting %04X (%zu words): hr=0x%08lX response=0x%04lX %s\n", prop, words.size(), (unsigned long)r.hr,
               (unsigned long)r.code, r.ok() ? "(accepted)" : "(REFUSED)");
        grab(2500);
        cam.PollEvents();
        PropState ps;
        if (cam.GetProp(prop, ps)) {
            printf("  value now: 0x%X", ps.value);
            if (ps.raw.size() > 4)
                for (size_t k = 0; k + 4 <= ps.raw.size() && k < 48; k += 4)
                    printf(" %d", int(uint32_t(ps.raw[k]) | uint32_t(ps.raw[k + 1]) << 8 |
                                      uint32_t(ps.raw[k + 2]) << 16 | uint32_t(ps.raw[k + 3]) << 24));
            printf("\n");
        }
        save(stem + L"-" + std::to_wstring(++n) + ext);
    }
    cam.Disconnect();
    return 0;
}

// =====================================================================================
// panel: opens the panel of the running canon-eos-webcam-bg (starts it if needed)

static int CmdPanel() {
    auto find = [] { return FindWindowW(kBridgeWindowClass, nullptr); };
    HWND w = find();
    if (!w) {
        if (BackgroundRunning()) {
            printf("%s", Tr(Str::CliPanelConsoleOnly));
            return 1;
        }
        if (CmdStart() != 0) return 1;
        for (int i = 0; i < 100 && !(w = find()); i++) Sleep(100);
        if (!w) { printf("%s", Tr(Str::CliPanelNoWindow)); return 1; }
    }
    PostMessageW(w, kMsgOpenPanel, 0, 0);
    printf("%s", Tr(Str::CliPanelOpened));
    return 0;
}

// =====================================================================================

static void Usage() { printf("%s", Tr(Str::CliUsage)); }

static int Dispatch(int argc, wchar_t** argv) {
    InitLang(SettingsStore::DefaultPath().c_str());
    std::wstring cmd = argc > 1 ? argv[1] : L"run";
    double seconds = 15;
    UINT w = kFrameWidth, h = kFrameHeight;
    int bench = 0;
    std::vector<std::wstring> sets;
    std::wstring out = L"canon-eos-webcam-test.jpg", positional;
    bool direct = false;
    for (int i = 2; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%ux%u", &w, &h);
        else if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else if (a == L"--direct") direct = true;
        else if (a == L"--bench" && i + 1 < argc) bench = _wtoi(argv[++i]);
        else if (a == L"--set" && i + 1 < argc) sets.push_back(argv[++i]);
        else if (positional.empty() && a[0] != L'-') positional = a;
        else { Usage(); return 2; }
    }

    if (cmd == L"start") return CmdStart();
    if (cmd == L"stop") return CmdStop();
    if (cmd == L"autostart") return CmdAutostart(positional);
    if (cmd == L"panel") return CmdPanel();

    // In canon-eos-webcam-bg the main thread is the UI thread (STA apartment, needed by the file dialog); the bridge
    // initializes its own COM on its thread.
    bool ui = cmd == L"run" && kWithUi;
    if (FAILED(CoInitializeEx(nullptr, ui ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED))) return 1;
    if (cmd != L"run") MFStartup(MF_VERSION);
    int rc;
    if (cmd == L"run") rc = CmdRun(kWithUi);
    else if (cmd == L"cam") rc = CmdCam(sets, out == L"canon-eos-webcam-test.jpg" ? L"canon-eos-webcam-cam.jpg" : out);
    else if (cmd == L"process") rc = CmdProcess(positional, out == L"canon-eos-webcam-test.jpg" ? L"canon-eos-webcam-process.jpg" : out, bench);
    else if (cmd == L"install") rc = CmdInstall();
    else if (cmd == L"uninstall") rc = CmdUninstall();
    else if (cmd == L"status") rc = CmdStatus();
    else if (cmd == L"list") rc = CmdList();
    else if (cmd == L"test") rc = CmdTest(seconds > 0 ? seconds : 15, w, h, out, direct);
    else { Usage(); rc = cmd == L"--help" || cmd == L"-h" ? 0 : 2; }
    if (cmd != L"run") MFShutdown();
    CoUninitialize();
    return rc;
}

#ifdef CANON_EOS_WEBCAM_BACKGROUND

// canon-eos-webcam-bg.exe: the same bridge as "canon-eos-webcam run", without a console, logging to ProgramData\CanonEosWebcam\canon-eos-webcam.log.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CreateDirectoryW(kDataDir, nullptr);
    std::wstring log = std::wstring(kDataDir) + L"\\canon-eos-webcam.log";
    wchar_t envLog[MAX_PATH];
    DWORD nl = GetEnvironmentVariableW(L"CANON_EOS_WEBCAM_LOG", envLog, MAX_PATH);  // for tests only
    if (nl > 0 && nl < MAX_PATH) log = envLog;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2u * 1024 * 1024)
        DeleteFileW(log.c_str());  // starts the log over when it exceeds 2 MB
    FILE* f = nullptr;
    _wfreopen_s(&f, log.c_str(), L"a", stdout);
    wchar_t* args[] = {const_cast<wchar_t*>(L"canon-eos-webcam-bg"), const_cast<wchar_t*>(L"run")};
    return Dispatch(2, args);
}

#else

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    return Dispatch(argc, argv);
}

#endif