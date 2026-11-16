#include "bridge.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <tlhelp32.h>

#include "applog.h"

namespace eoscam {

namespace {

// Sleep() do Windows tem resolução de ~15,6 ms; para esperas curtas usamos um timer de alta resolução.
void ShortSleep(DWORD ms) {
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(ms) * 10000;
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(ms);
}

double FtSec(const FILETIME& f) { return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; }

double ProcessCpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return FtSec(k) + FtSec(u);
}

// Diagnóstico: CPU (kernel+usuário) acumulada de cada thread do processo, com o nome dado por
// SetThreadDescription. Serve para ver qual thread gasta CPU (trabalho real x espera ativa).
void LogThreadCpu() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            double cpu = FtSec(k) + FtSec(u);
            if (cpu >= 0.05) {
                std::string name;
                PWSTR d = nullptr;
                if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
                    for (const wchar_t* p = d; *p; p++) name += *p < 128 ? char(*p) : '?';
                    LocalFree(d);
                }
                Log("  thread %lu %s: CPU %.2f s (usuário %.2f, kernel %.2f)", (unsigned long)te.th32ThreadID,
                    name.c_str(), cpu, FtSec(u), FtSec(k));
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

}  // namespace

Bridge::Bridge(SettingsStore& store) : store_(store), pipeline_(store) {
    mboxEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Bridge::~Bridge() {
    stop_ = true;
    if (proc_.joinable()) proc_.join();
    if (mboxEvent_) CloseHandle(mboxEvent_);
}

BridgeStatus Bridge::Status() const {
    BridgeStatus s;
    s.state = ProducerState(state_.load());
    s.cameraConnected = camConnected_;
    s.consumerActive = consumer_;
    {
        std::lock_guard<std::mutex> lk(nameMu_);
        s.cameraName = camName_;
    }
    s.fpsIn = fpsIn_;
    s.fpsOut = fpsOut_;
    s.pipeline = pipeline_.Stats();
    return s;
}

bool Bridge::CameraProp(uint32_t prop, PropState& out) const { return cam_.GetProp(prop, out); }

void Bridge::SetCameraProp(uint32_t prop, uint32_t value, const std::string& label) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    cmds_.push_back({prop, {value}, false, label});
}

void Bridge::SetCameraPropWords(uint32_t prop, const std::vector<uint32_t>& words, const std::string& label) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    cmds_.push_back({prop, words, true, label});
}

std::string Bridge::CameraError() const {
    std::lock_guard<std::mutex> lk(cmdMu_);
    if (camError_.empty() || NowTick() - camErrorTick_ > 8000) return {};
    return camError_;
}

void Bridge::SetCameraError(const std::string& msg) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    camError_ = msg;
    camErrorTick_ = NowTick();
}

// Executa, na thread de captura, os ajustes pedidos pelo painel.
void Bridge::RunCameraCommands() {
    for (;;) {
        CamCommand c;
        {
            std::lock_guard<std::mutex> lk(cmdMu_);
            if (cmds_.empty()) return;
            c = std::move(cmds_.front());
            cmds_.pop_front();
        }
        if (!cam_.IsConnected()) {
            SetCameraError("A câmera não está conectada: não deu para ajustar " + c.label + ".");
            continue;
        }
        PtpResult r = c.structured ? cam_.WritePropWords(c.prop, c.words) : cam_.WriteProp(c.prop, c.words[0]);
        if (!r.ok()) {
            char b[160];
            snprintf(b, sizeof b, "A câmera recusou o ajuste (%s): resposta 0x%04lX, hr=0x%08lX. Modo automático?",
                     c.label.c_str(), (unsigned long)r.code, (unsigned long)r.hr);
            SetCameraError(b);
            Log("câmera: %s", b);
        } else {
            Log("câmera: %s ajustado (prop %04X = 0x%X)", c.label.c_str(), c.prop, c.words.empty() ? 0u : c.words[0]);
        }
        cam_.PollEvents();  // lê de volta o valor que a câmera de fato aceitou
        propsGen_ = cam_.PropsGeneration();
    }
}

void Bridge::ProcessLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDescription(GetCurrentThread(), L"processamento");
    pipeline_.Init();
    std::vector<uint8_t> jpeg(1 << 20), nv12(kFrameBytes);
    uint64_t lastReload = 0, taken = 0;
    while (!stop_) {
        WaitForSingleObject(mboxEvent_, 100);
        uint64_t now = NowTick();
        if (now - lastReload > 1000) {
            lastReload = now;
            store_.ReloadIfChangedOnDisk();
        }
        if (restarted_.exchange(false)) pipeline_.StreamRestarted();
        if (prewarm_.exchange(false)) pipeline_.Prewarm();

        uint64_t tick = 0;
        {
            std::lock_guard<std::mutex> lk(mboxMu_);
            if (mboxSeq_ == taken) continue;
            taken = mboxSeq_;
            jpeg.swap(mboxJpeg_);
            tick = mboxTick_;
        }
        if (pipeline_.Process(jpeg.data(), jpeg.size(), nv12.data())) {
            if (chanReady_) chan_.PublishNv12(nv12.data(), kFrameWidth, kFrameHeight, tick);
            framesOut_++;
        } else {
            static int fails = 0;
            if (fails++ < 5) Log("pipeline: falha ao processar um quadro (%zu bytes)", jpeg.size());
        }
    }
    CoUninitialize();
}

int Bridge::Run(HANDLE stopEvent, const std::atomic<bool>* externalStop) {
    std::vector<uint8_t> jpeg;
    uint64_t lastOpenTry = 0, lastWanted = 0, lastConnectTry = 0, statsTick = 0, statFrames = 0, statOut = 0;
    uint64_t fpsTick = NowTick(), fpsFrames = 0, fpsOutBase = 0;
    int lastRecoveries = 0;
    std::string lastError;
    ProducerState state = ProducerState::Idle;
    double cpuBase = ProcessCpuSeconds();
    uint64_t cpuTick = NowTick();

    SetThreadDescription(GetCurrentThread(), L"captura");
    proc_ = std::thread([this] { ProcessLoop(); });

    Log("eoscam rodando. Ctrl+C para sair.");
    Log("Aguardando a câmera virtual ser registrada/aberta pelo Windows...");

    while (!stop_ && !(externalStop && *externalStop)) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        uint64_t now = NowTick();
        // O canal Global\ só existe depois que a DLL é aberta por algum programa (um usuário comum não
        // consegue criá-lo). Sem ele a ponte ainda funciona para o painel (câmera + prévia), só não publica.
        if (!chan_.IsOpen() && now - lastOpenTry >= 1000) {
            lastOpenTry = now;
            if (chan_.Open(true)) {
                chanReady_ = true;
                Log("Canal com a câmera virtual conectado.");
            }
        }

        bool consumer = chan_.IsOpen() && chan_.ConsumerActive();
        consumer_ = consumer;
        bool wanted = consumer || panelOpen_;
        if (wanted) lastWanted = now;

        if (wanted && !cam_.IsConnected() && now - lastConnectTry >= 2000) {
            lastConnectTry = now;
            state = ProducerState::Starting;
            if (chan_.IsOpen()) chan_.SetProducer(state);
            state_ = int(state);
            std::string err;
            if (cam_.Connect(err)) {
                Log("%s: Live View ligado (%s).", consumer ? "Webcam em uso" : "Painel aberto",
                    cam_.Name().c_str());
                state = ProducerState::Streaming;
                lastError.clear();
                statsTick = NowTick();
                statFrames = 0;
                camConnected_ = true;
                {
                    std::lock_guard<std::mutex> lk(nameMu_);
                    camName_ = cam_.Name();
                }
                propsGen_ = cam_.PropsGeneration();
                prewarm_ = true;     // pré-carrega os efeitos ativos
                restarted_ = true;   // zera o estado temporal dos efeitos
                SetEvent(mboxEvent_);
            } else {
                state = ProducerState::NoCamera;
                if (err != lastError) Log("Câmera indisponível: %s", err.c_str());
                lastError = err;
            }
        }

        if (cam_.IsConnected() && !wanted && now - lastWanted > 5000) {
            cam_.Disconnect();
            camConnected_ = false;
            state = ProducerState::Idle;
            Log("Webcam não está mais em uso: Live View desligado.");
        }
        if (!cam_.IsConnected() && !wanted && state != ProducerState::Idle) state = ProducerState::Idle;

        if (chan_.IsOpen()) chan_.SetProducer(state);
        state_ = int(state);

        if (cam_.IsConnected()) {
            cam_.SetEventInterval(panelOpen_ ? 250 : 1000);
            RunCameraCommands();
            CanonCamera::Frame fr = cam_.GetFrame(jpeg);
            if (cam_.PropsGeneration() != propsGen_) propsGen_ = cam_.PropsGeneration();
            if (cam_.Recoveries() != lastRecoveries) {
                lastRecoveries = cam_.Recoveries();
                Log("  a câmera parou de enviar quadros (resposta 0x%04lX); religando o Live View",
                    (unsigned long)cam_.LastCode());
            }
            switch (fr) {
                case CanonCamera::Frame::Ok: {
                    {
                        std::lock_guard<std::mutex> lk(mboxMu_);
                        mboxJpeg_.swap(jpeg);
                        mboxTick_ = NowTick();
                        mboxSeq_++;
                    }
                    SetEvent(mboxEvent_);
                    statFrames++;
                    fpsFrames++;
                    break;
                }
                case CanonCamera::Frame::NotReady:
                    // O Live View da T3 renova a ~30 fps; aqui esperamos só um pouco antes de pedir de novo.
                    ShortSleep(3);
                    break;
                case CanonCamera::Frame::Error:
                    Log("Perdi a comunicação com a câmera; tentando reconectar.");
                    cam_.Disconnect();
                    camConnected_ = false;
                    state = ProducerState::NoCamera;
                    break;
            }
            uint64_t tn = NowTick();
            if (tn - fpsTick >= 1000) {
                fpsIn_ = fpsFrames * 1000.0 / double(tn - fpsTick);
                uint64_t fo = framesOut_;
                fpsOut_ = double(fo - fpsOutBase) * 1000.0 / double(tn - fpsTick);
                fpsOutBase = fo;
                fpsTick = tn;
                fpsFrames = 0;
            }
            if (tn - statsTick >= 60000) {
                double secs = double(tn - statsTick) / 1000.0;
                PipelineStats ps = pipeline_.Stats();
                double cpu = ProcessCpuSeconds();
                double cpuPct = (cpu - cpuBase) / (double(tn - cpuTick) / 1000.0) * 100.0;
                cpuBase = cpu;
                cpuTick = tn;
                uint64_t fo = framesOut_;
                Log("  %.1f quadros/s da câmera, %.1f publicados; CPU da ponte %.1f%% de um núcleo", statFrames / secs,
                    double(fo - statOut) / secs, cpuPct);
                Log("  por quadro (ms): decodificação %.2f | GPU: envio %.2f, denoise %.2f, artefatos %.2f, super "
                    "resolução %.2f, máscara/desfoque %.2f, retorno %.2f | CPU: escala+ajustes+fundo+NV12 %.2f | "
                    "total %.2f",
                    ps.msDecode, ps.msUpload, ps.msDenoise, ps.msArtifact, ps.msSuperRes, ps.msSegment,
                    ps.msDownload, ps.msFinal, ps.msTotal);
                statOut = fo;
                statsTick = tn;
                statFrames = 0;
            }
        } else {
            RunCameraCommands();  // responde com o erro "câmera não conectada"
            Sleep(100);
        }
    }

    {
        PipelineStats ps = pipeline_.Stats();
        double cpu = ProcessCpuSeconds() - cpuBase;
        double secs = double(NowTick() - cpuTick) / 1000.0;
        if (ps.frames > 0 && secs > 1)
            Log("  resumo: %llu quadros processados; por quadro (ms): decodificação %.2f | GPU: envio %.2f, denoise "
                "%.2f, artefatos %.2f, super resolução %.2f, máscara/desfoque %.2f, retorno %.2f | CPU: escala+"
                "ajustes+fundo+NV12 %.2f | total %.2f; CPU da ponte %.1f%% de um núcleo",
                (unsigned long long)ps.frames, ps.msDecode, ps.msUpload, ps.msDenoise, ps.msArtifact, ps.msSuperRes,
                ps.msSegment, ps.msDownload, ps.msFinal, ps.msTotal, cpu / secs * 100.0);
    }
    LogThreadCpu();
    Log("Encerrando...");
    stop_ = true;
    SetEvent(mboxEvent_);
    if (proc_.joinable()) proc_.join();
    cam_.Disconnect();
    camConnected_ = false;
    if (chan_.IsOpen()) {
        chan_.SetProducer(ProducerState::Unknown);
        InterlockedExchange64(&chan_.Header().producerTick, 0);
    }
    state_ = int(ProducerState::Unknown);
    return 0;
}

}  // namespace eoscam
