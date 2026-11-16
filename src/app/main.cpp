// eoscam.exe — ponte entre a Canon EOS (PTP/WPD) e a câmera virtual do Windows 11.
//
//   eoscam run         fica rodando: liga o Live View só quando algum programa usa a webcam
//   eoscam panel       abre o painel de controle (do eoscam-bg em execução; inicia-o se preciso)
//   eoscam install     cria a câmera virtual "Canon EOS Webcam (eoscam)" (persistente)
//   eoscam uninstall   remove a câmera virtual
//   eoscam test        abre a câmera virtual como um app comum e mede quadros/s e CPU
//   eoscam process     processa um JPEG pelo pipeline de imagem (sem câmera) e mede os tempos

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
#include "bridge.h"
#include "canon_camera.h"
#include "color.h"
#include "panel.h"
#include "pipeline.h"
#include "settings.h"
#include "shared.h"

using Microsoft::WRL::ComPtr;
using namespace eoscam;

static std::atomic<bool> g_stop{false};

#ifdef EOSCAM_BACKGROUND
constexpr bool kWithUi = true;
#else
constexpr bool kWithUi = false;
#endif

static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

#define Out eoscam::Log

static double FtSec(const FILETIME& f) { return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; }

// =====================================================================================
// run

// Só um produtor por vez (eoscam run ou eoscam-bg), senão os dois brigam pela câmera.
static bool AcquireSingleInstance(HANDLE& single, HANDLE& stopEvent) {
    single = CreateMutexW(nullptr, TRUE, L"Local\\EosCamProducer");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        Out("Outra instância do eoscam já está rodando (use \"eoscam stop\" para encerrá-la).");
        CloseHandle(single);
        single = nullptr;
        return false;
    }
    // "eoscam stop" sinaliza este evento para encerrar com o Live View desligado corretamente.
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\EosCamStop");
    if (stopEvent) ResetEvent(stopEvent);
    return true;
}

// withUi: no eoscam-bg a thread principal roda o ícone da bandeja e o painel, e a ponte numa thread própria.
static int CmdRun(bool withUi) {
    HANDLE single = nullptr, stopEvent = nullptr;
    if (!AcquireSingleInstance(single, stopEvent)) return 1;

    SettingsStore store;
    store.Load(SettingsStore::DefaultPath());
    Out("Configurações em %S", store.Path().c_str());
    Bridge bridge(store);

    int rc = 0;
    if (!withUi) {
        rc = bridge.Run(stopEvent, &g_stop);
    } else {
        PanelApp app(bridge, store);
        std::thread worker([&] {
            bridge.Run(stopEvent, &g_stop);
            app.RequestQuit();  // "eoscam stop", Ctrl+C ou "Sair" no menu: fecha a janela também
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
// start / stop / autostart (versão em segundo plano: eoscam-bg.exe)

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    return p.substr(0, p.find_last_of(L'\\') + 1);
}

static bool BackgroundRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\EosCamProducer");
    if (!m) return false;
    CloseHandle(m);
    return true;
}

static int CmdStart() {
    if (BackgroundRunning()) { printf("O eoscam já está rodando.\n"); return 0; }
    std::wstring exe = ExeDir() + L"eoscam-bg.exe";
    std::wstring cmdline = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si,
                        &pi)) {
        printf("Não consegui iniciar %s (erro %lu).\n", W2U(exe.c_str()).c_str(), GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("eoscam iniciado em segundo plano (log em %s\\eoscam.log).\n", W2U(kDataDir).c_str());
    return 0;
}

static int CmdStop() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\EosCamStop");
    if (!ev) { printf("O eoscam não está rodando.\n"); return 0; }
    SetEvent(ev);
    CloseHandle(ev);
    for (int i = 0; i < 50 && BackgroundRunning(); i++) Sleep(100);
    printf(BackgroundRunning() ? "O eoscam não respondeu ao pedido de parada.\n" : "eoscam encerrado.\n");
    return 0;
}

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static int CmdAutostart(const std::wstring& arg) {
    if (arg == L"on") {
        std::wstring value = L"\"" + ExeDir() + L"eoscam-bg.exe\"";
        LSTATUS s = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"EosCam", REG_SZ, value.c_str(),
                                    DWORD((value.size() + 1) * sizeof(wchar_t)));
        if (s != ERROR_SUCCESS) { printf("Falha ao gravar no registro (erro %ld).\n", (long)s); return 1; }
        printf("O eoscam vai iniciar junto com o Windows (%s).\n", W2U(value.c_str()).c_str());
        return CmdStart();
    }
    if (arg == L"off") {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"EosCam");
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) {
            printf("Falha ao remover do registro (erro %ld).\n", (long)s);
            return 1;
        }
        printf("O eoscam não vai mais iniciar com o Windows.\n");
        return 0;
    }
    printf("uso: eoscam autostart on|off\n");
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
    if (FAILED(hr)) { printf("MFEnumDeviceSources falhou (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
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
        printf("Canal %s não existe (erro %lu): a webcam virtual ainda não foi aberta por nenhum programa.\n",
               W2U(MappingName()).c_str(), err);
        return 1;
    }
    for (int i = 0; i < 5 && !g_stop; i++) {
        SharedHeader& h = chan.Header();
        uint64_t now = NowTick();
        printf("quadro nº %lld  slot %ld  produtor estado %ld (há %lld ms)  consumidor %ldx%ld (há %lld ms)\n",
               (long long)h.frameNumber, (long)h.latestSlot, (long)h.producerState,
               h.producerTick ? (long long)(now - uint64_t(h.producerTick)) : -1LL, (long)h.consumerWidth,
               (long)h.consumerHeight, h.consumerTick ? (long long)(now - uint64_t(h.consumerTick)) : -1LL);
        Sleep(500);
    }
    return 0;
}

// =====================================================================================
// install / uninstall

static HRESULT CreateVCam(ComPtr<IMFVirtualCamera>& vc) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_CurrentUser, kCameraName, EOSCAM_CLSID_STRING, nullptr, 0, &vc);
}

static int CmdInstall() {
    ComPtr<IMFVirtualCamera> vc;
    HRESULT hr = CreateVCam(vc);
    if (FAILED(hr)) {
        printf("MFCreateVirtualCamera falhou (hr=0x%08lX).\n", (unsigned long)hr);
        if (hr == REGDB_E_CLASSNOTREG)
            printf("A DLL da fonte não está registrada. Rode scripts\\deploy.ps1 como administrador.\n");
        return 1;
    }
    hr = vc->Start(nullptr);
    if (FAILED(hr)) {
        printf("IMFVirtualCamera::Start falhou (hr=0x%08lX).\n", (unsigned long)hr);
        return 1;
    }
    printf("Câmera virtual \"%s\" registrada.\n", W2U(kCameraName).c_str());
    return 0;
}

static int CmdUninstall() {
    ComPtr<IMFVirtualCamera> vc;
    HRESULT hr = CreateVCam(vc);
    if (SUCCEEDED(hr)) hr = vc->Remove();
    if (FAILED(hr)) {
        printf("Falha ao remover a câmera virtual (hr=0x%08lX).\n", (unsigned long)hr);
        return 1;
    }
    printf("Câmera virtual removida.\n");
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

// Carrega eoscam-source.dll (ao lado do exe) direto neste processo, sem registro nem Frame Server.
static HRESULT ActivateDirect(ComPtr<IMFActivate>& act) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    p = p.substr(0, p.find_last_of(L'\\') + 1) + L"eoscam-source.dll";
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
        if (FAILED(hr)) { printf("Falha ao carregar a DLL diretamente (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
        printf("Modo direto: fonte carregada neste processo (canal %s).\n",
               W2U(MappingName()).c_str());
        return TestWithActivate(act.Get(), seconds, wantW, wantH, outPath);
    }

    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
    if (FAILED(hr)) { printf("MFEnumDeviceSources falhou (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("Câmeras de vídeo encontradas:\n");
    ComPtr<IMFActivate> sel;
    for (UINT32 i = 0; i < n; i++) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        // o Windows acrescenta " (Câmera Virtual do Windows)" ao nome
        bool match = name && wcsncmp(name, kCameraName, wcslen(kCameraName)) == 0;
        printf("  %s %s\n", match ? "->" : "  ", W2U(name).c_str());
        if (match && !sel) sel = devs[i];
        CoTaskMemFree(name);
    }
    for (UINT32 i = 0; i < n; i++) devs[i]->Release();
    CoTaskMemFree(devs);
    if (!sel) { printf("\nA câmera virtual não apareceu. Rode \"eoscam install\" primeiro.\n"); return 1; }
    return TestWithActivate(sel.Get(), seconds, wantW, wantH, outPath);
}

static int TestWithActivate(IMFActivate* sel, double seconds, UINT wantW, UINT wantH, const std::wstring& outPath) {
    ComPtr<IMFMediaSource> src;
    HRESULT hr = sel->ActivateObject(IID_PPV_ARGS(&src));
    if (FAILED(hr)) { printf("Falha ao abrir a câmera virtual (hr=0x%08lX)\n", (unsigned long)hr); return 1; }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(src.Get(), nullptr, &reader);
    if (FAILED(hr)) { printf("MFCreateSourceReaderFromMediaSource falhou (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("\nFormatos oferecidos:\n");
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
    if (!chosen) { printf("Formato %ux%u não oferecido.\n", wantW, wantH); return 1; }
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, chosen.Get());
    if (FAILED(hr)) { printf("SetCurrentMediaType falhou (hr=0x%08lX)\n", (unsigned long)hr); return 1; }

    printf("\nLendo quadros por %.0f s (o Live View leva alguns segundos para ligar)...\n", seconds);
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
        if (FAILED(hr)) { printf("ReadSample falhou (hr=0x%08lX)\n", (unsigned long)hr); break; }
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
    printf("\n================ RESULTADO ================\n");
    printf("Formato .............. NV12 %ux%u\n", wantW, wantH);
    printf("Amostras recebidas ... %llu em %.1f s (%.1f/s)\n", (unsigned long long)frames, secs, frames / secs);
    printf("Quadros distintos .... %llu (%.1f/s)\n", (unsigned long long)distinct, distinct / secs);
    if (firstLiveTick) printf("Imagem ao vivo após .. %.1f s\n", (firstLiveTick - t0) / 1000.0);
    printf("CPU deste teste ...... %.1f%% de um núcleo\n", proc / secs * 100);
    printf("CPU do sistema todo .. %.1f%%\n", sysTotal > 0 ? (sysTotal - sysIdle) / sysTotal * 100 : 0.0);

    if (last.size() >= size_t(wantW) * wantH * 3 / 2) {
        std::vector<uint8_t> bgra(size_t(wantW) * wantH * 4);
        Nv12ToBgra(last.data(), int(wantW), int(wantH), bgra.data());
        if (SaveBgraAsJpeg(outPath, bgra.data(), wantW, wantH))
            printf("Último quadro salvo em %s\n", W2U(outPath.c_str()).c_str());
    }
    return 0;
}

// =====================================================================================
// process: pipeline de imagem sobre um JPEG (sem câmera), para testar efeitos e medir os tempos

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
        case FxState::Off: return "desligado";
        case FxState::Loading: return "carregando";
        case FxState::Active: return "ativo";
        case FxState::Failed: return "FALHOU";
        case FxState::Unavailable: return "indisponível";
    }
    return "?";
}

// Dá partida nos carregamentos dos modelos (em segundo plano) e processa o mesmo quadro até que todos os
// efeitos pedidos estejam ativos (ou tenham falhado); depois aquece o estado temporal (Denoise, Green Screen).
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
    printf("Efeitos carregados em %.1f s\n", double(NowTick() - t0) / 1000.0);
    for (int i = 0; i < 15; i++) pipe.Process(jpeg.data(), jpeg.size(), nv12.data());
    return true;
}

static int CmdProcess(const std::wstring& inPath, const std::wstring& outPath, int bench) {
    if (inPath.empty()) {
        printf("uso: eoscam process entrada.jpg [--out saida.jpg] [--bench N]\n");
        return 2;
    }
    std::vector<uint8_t> jpeg = ReadWholeFile(inPath);
    if (jpeg.empty()) { printf("Não consegui ler %s\n", W2U(inPath.c_str()).c_str()); return 1; }

    SettingsStore store;
    store.Load(SettingsStore::DefaultPath());
    printf("Configurações: %s\n", W2U(store.Path().c_str()).c_str());
    FramePipeline pipe(store);
    if (!pipe.Init()) { printf("Falha ao iniciar o pipeline.\n"); return 1; }
    std::vector<uint8_t> nv12(kFrameBytes);

    if (!WarmUpPipeline(pipe, jpeg, nv12)) { printf("Falha ao processar o quadro.\n"); return 1; }

    if (bench > 0) {
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        uint64_t b0 = NowTick();
        for (int i = 0; i < bench; i++) pipe.Process(jpeg.data(), jpeg.size(), nv12.data());
        double secs = double(NowTick() - b0) / 1000.0;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        double cpu = (FtSec(k1) + FtSec(u1)) - (FtSec(k0) + FtSec(u0));
        printf("Benchmark: %d quadros em %.2f s = %.1f quadros/s (%.1f ms por quadro); CPU %.1f%% de um núcleo\n",
               bench, secs, bench / secs, secs * 1000.0 / bench, cpu / secs * 100.0);
    }
    PipelineStats st = pipe.Stats();
    if (st.maxineAvailable) printf("Maxine: disponível (%s)\n", st.maxineVersion.c_str());
    else printf("Maxine: INDISPONÍVEL (%s)\n", st.maxineReason.c_str());
    printf("  Denoise ............ %s (%s)\n", FxStateName(st.denoise.state), st.denoise.text.c_str());
    printf("  Artifact Reduction . %s (%s)\n", FxStateName(st.artifact.state), st.artifact.text.c_str());
    printf("  Super Resolution ... %s (%s)\n", FxStateName(st.superRes.state), st.superRes.text.c_str());
    printf("  Green Screen ....... %s (%s)\n", FxStateName(st.greenScreen.state), st.greenScreen.text.c_str());
    printf("  Desfoque do SDK .... %s (%s)\n", FxStateName(st.blur.state), st.blur.text.c_str());
    if (!st.backgroundNote.empty()) printf("  Fundo: %s\n", st.backgroundNote.c_str());
    printf("Tempo por quadro (ms, média móvel): decodificação %.2f | GPU: envio %.2f, denoise %.2f, artefatos %.2f, "
           "super resolução %.2f, máscara/desfoque %.2f, retorno %.2f | CPU: escala+ajustes+fundo+NV12 %.2f | total %.2f\n",
           st.msDecode, st.msUpload, st.msDenoise, st.msArtifact, st.msSuperRes, st.msSegment, st.msDownload,
           st.msFinal, st.msTotal);

    std::vector<uint8_t> bgra(size_t(kFrameWidth) * kFrameHeight * 4);
    Nv12ToBgra(nv12.data(), int(kFrameWidth), int(kFrameHeight), bgra.data());
    if (SaveBgraAsJpeg(outPath, bgra.data(), kFrameWidth, kFrameHeight))
        printf("Quadro salvo em %s\n", W2U(outPath.c_str()).c_str());
    return 0;
}

// =====================================================================================
// cam: diagnóstico dos ajustes da câmera (PTP) com a ponte parada

static void PrintCameraProps(CanonCamera& cam) {
    for (auto& [prop, st] : cam.AllProps()) {
        printf("  %04X", prop);
        if (st.hasValue) printf(" valor=0x%X", st.value);
        if (st.hasList) {
            printf(" lista(%zu):", st.list.size());
            for (uint32_t v : st.list) printf(" %X", v);
        }
        if (st.raw.size() > 4) {
            printf(" bytes(%zu):", st.raw.size());
            for (size_t i = 0; i < st.raw.size() && i < 48; i++) printf(" %02X", st.raw[i]);
        }
        printf("\n");
    }
}

// Conecta, mostra as propriedades que a câmera anuncia, aplica --set PROP=VALOR (hexadecimal) um por vez e salva,
// depois de cada um, um quadro processado pelo pipeline (para comparar o efeito na imagem).
static int CmdCam(const std::vector<std::wstring>& sets, const std::wstring& outPath) {
    if (BackgroundRunning()) {
        printf("Pare o eoscam antes (eoscam stop): só um programa pode falar com a câmera.\n");
        return 1;
    }
    CanonCamera cam;
    std::string err;
    if (!cam.Connect(err)) { printf("Câmera indisponível: %s\n", err.c_str()); return 1; }
    printf("Câmera: %s\n", cam.Name().c_str());

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
        printf("  luminância média %.1f, croma U %.1f V %.1f\n", sy / double((ny + 6) / 7), su / double(ny / 4),
               sv / double(ny / 4));
        if (SaveBgraAsJpeg(path, bgra.data(), kFrameWidth, kFrameHeight))
            printf("  quadro salvo em %s\n", W2U(path.c_str()).c_str());
    };

    grab(2500);
    cam.PollEvents();
    printf("Propriedades anunciadas pela câmera:\n");
    PrintCameraProps(cam);
    std::wstring base = outPath;
    size_t dot = base.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? base : base.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos ? L".jpg" : base.substr(dot);
    save(stem + L"-0" + ext);

    int n = 0;
    for (auto& a : sets) {
        // PROP=VALOR ou PROP=P0,P1,... (propriedade estruturada, palavras de 32 bits; valores com sinal em decimal)
        unsigned prop = 0;
        size_t eq = a.find(L'=');
        if (eq == std::wstring::npos || swscanf_s(a.c_str(), L"%x", &prop) != 1) {
            printf("--set PROP=VALOR (hexadecimal)\n");
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
        printf("Ajuste %04X (%zu palavras): hr=0x%08lX resposta=0x%04lX %s\n", prop, words.size(), (unsigned long)r.hr,
               (unsigned long)r.code, r.ok() ? "(aceito)" : "(RECUSADO)");
        grab(2500);
        cam.PollEvents();
        PropState ps;
        if (cam.GetProp(prop, ps)) {
            printf("  valor agora: 0x%X", ps.value);
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
// panel: abre o painel do eoscam-bg em execução (inicia-o se preciso)

static int CmdPanel() {
    auto find = [] { return FindWindowW(kBridgeWindowClass, nullptr); };
    HWND w = find();
    if (!w) {
        if (BackgroundRunning()) {
            printf("O eoscam está rodando sem janela (\"eoscam run\" em console): o painel só existe no eoscam-bg.\n");
            return 1;
        }
        if (CmdStart() != 0) return 1;
        for (int i = 0; i < 100 && !(w = find()); i++) Sleep(100);
        if (!w) { printf("O eoscam iniciou, mas a janela não apareceu.\n"); return 1; }
    }
    PostMessageW(w, kMsgOpenPanel, 0, 0);
    printf("Painel aberto.\n");
    return 0;
}

// =====================================================================================

static void Usage() {
    printf(
        "uso: eoscam [comando]\n"
        "  run                 (padrão) faz a ponte entre a câmera Canon e a webcam virtual, neste console\n"
        "  start | stop        inicia/encerra a ponte em segundo plano (eoscam-bg.exe)\n"
        "  autostart on|off    inicia (ou não) a ponte em segundo plano junto com o Windows\n"
        "  install             registra a câmera virtual \"Canon EOS Webcam (eoscam)\"\n"
        "  uninstall           remove a câmera virtual\n"
        "  cam [--set PROP=VALOR]... [--out f.jpg]\n"
        "                      diagnóstico: lista os ajustes que a câmera anuncia e testa escrever (hex), salvando quadros\n"
        "  panel               abre o painel de controle (inicia o eoscam-bg se ele não estiver rodando)\n"
        "  process in.jpg [--out f.jpg] [--bench N]\n"
        "                      passa um JPEG pelo pipeline de imagem (sem câmera): testa efeitos e mede tempos\n"
        "  status              mostra o estado do canal entre o app e a câmera virtual\n"
        "  list                lista as câmeras de vídeo que o Windows enxerga\n"
        "  test [--seconds N] [--size LxA] [--out arquivo.jpg] [--direct]\n"
        "                      abre a webcam virtual como um app comum e mede o desempenho\n"
        "                      --direct carrega a DLL neste processo (sem Frame Server; use EOSCAM_LOCAL=1\n"
        "                      tanto aqui quanto no \"eoscam run\")\n");
}

static int Dispatch(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"run";
    double seconds = 15;
    UINT w = kFrameWidth, h = kFrameHeight;
    int bench = 0;
    std::vector<std::wstring> sets;
    std::wstring out = L"eoscam-test.jpg", positional;
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

    // No eoscam-bg a thread principal é a da interface (apartamento STA, necessário para o diálogo de
    // arquivos); a ponte inicia o próprio COM na thread dela.
    bool ui = cmd == L"run" && kWithUi;
    if (FAILED(CoInitializeEx(nullptr, ui ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED))) return 1;
    if (cmd != L"run") MFStartup(MF_VERSION);
    int rc;
    if (cmd == L"run") rc = CmdRun(kWithUi);
    else if (cmd == L"cam") rc = CmdCam(sets, out == L"eoscam-test.jpg" ? L"eoscam-cam.jpg" : out);
    else if (cmd == L"process") rc = CmdProcess(positional, out == L"eoscam-test.jpg" ? L"eoscam-process.jpg" : out, bench);
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

#ifdef EOSCAM_BACKGROUND

// eoscam-bg.exe: mesma ponte do "eoscam run", sem janela, com log em ProgramData\EosCam\eoscam.log.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CreateDirectoryW(kDataDir, nullptr);
    std::wstring log = std::wstring(kDataDir) + L"\\eoscam.log";
    wchar_t envLog[MAX_PATH];
    DWORD nl = GetEnvironmentVariableW(L"EOSCAM_LOG", envLog, MAX_PATH);  // só para testes
    if (nl > 0 && nl < MAX_PATH) log = envLog;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2u * 1024 * 1024)
        DeleteFileW(log.c_str());  // recomeça o log quando passa de 2 MB
    FILE* f = nullptr;
    _wfreopen_s(&f, log.c_str(), L"a", stdout);
    wchar_t* args[] = {const_cast<wchar_t*>(L"eoscam-bg"), const_cast<wchar_t*>(L"run")};
    return Dispatch(2, args);
}

#else

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    return Dispatch(argc, argv);
}

#endif