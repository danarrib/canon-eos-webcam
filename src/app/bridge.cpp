#include "bridge.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <tlhelp32.h>

#include "applog.h"

namespace canon_eos_webcam {

namespace {

// Windows Sleep() has a resolution of ~15.6 ms; short waits use a high-resolution timer instead.
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

// Diagnostics: accumulated CPU (kernel+user) of each thread of the process, with the name given by
// SetThreadDescription. Shows which thread spends CPU (real work versus busy waiting).
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
                Log("  thread %lu %s: CPU %.2f s (user %.2f, kernel %.2f)", (unsigned long)te.th32ThreadID,
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

void Bridge::SetCameraProp(uint32_t prop, uint32_t value, Str label) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    cmds_.push_back({CamCommand::Kind::Prop, prop, {value}, label});
}

void Bridge::SetCameraPropWords(uint32_t prop, const std::vector<uint32_t>& words, Str label) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    cmds_.push_back({CamCommand::Kind::PropWords, prop, words, label});
}

void Bridge::AutoFocus() {
    std::lock_guard<std::mutex> lk(cmdMu_);
    // repeated clicks while the camera hasn't started the previous one would only queue redundant AFs
    for (const CamCommand& c : cmds_)
        if (c.kind == CamCommand::Kind::AutoFocus) return;
    cmds_.push_back({CamCommand::Kind::AutoFocus, 0, {}, Str::LabelAutoFocus});
}

std::string Bridge::CameraError(Lang lang) const {
    std::lock_guard<std::mutex> lk(cmdMu_);
    if (camError_.empty() || NowTick() - camErrorTick_ > 8000) return {};
    return Text(camError_, lang);
}

void Bridge::SetCameraError(const Msg& msg) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    camError_ = msg;
    camErrorTick_ = NowTick();
}

// Runs, on the capture thread, the camera settings requested by the panel.
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
            SetCameraError(Msg(Str::CamErrNotConnected, {}, c.label));
            continue;
        }
        if (c.kind == CamCommand::Kind::AutoFocus) {
            PtpResult r = cam_.AutoFocus();
            if (!r.ok()) {
                char b[64];
                snprintf(b, sizeof b, "0x%04lX, hr=0x%08lX", (unsigned long)r.code, (unsigned long)r.hr);
                Msg m(Str::CamErrAutoFocus, b);
                SetCameraError(m);
                Log("camera: %s", Text(m, Lang::En).c_str());
            } else {
                Log("camera: autofocus requested");
            }
            continue;
        }
        PtpResult r = c.kind == CamCommand::Kind::PropWords ? cam_.WritePropWords(c.prop, c.words)
                                                             : cam_.WriteProp(c.prop, c.words[0]);
        if (!r.ok()) {
            char b[64];
            snprintf(b, sizeof b, "0x%04lX, hr=0x%08lX", (unsigned long)r.code, (unsigned long)r.hr);
            Msg m(Str::CamErrRefused, b, c.label);
            SetCameraError(m);
            Log("camera: %s", Text(m, Lang::En).c_str());
        } else {
            Log("camera: %s set (prop %04X = 0x%X)", Tr(c.label, Lang::En), c.prop, c.words.empty() ? 0u : c.words[0]);
        }
        cam_.PollEvents();  // reads back the value the camera actually accepted
        propsGen_ = cam_.PropsGeneration();
    }
}

void Bridge::ProcessLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDescription(GetCurrentThread(), L"processing");
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
            if (fails++ < 5) Log("pipeline: failed to process a frame (%zu bytes)", jpeg.size());
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

    SetThreadDescription(GetCurrentThread(), L"capture");
    proc_ = std::thread([this] { ProcessLoop(); });

    Log("canon-eos-webcam running. Ctrl+C to quit.");
    Log("Waiting for Windows to register/open the virtual camera...");

    while (!stop_ && !(externalStop && *externalStop)) {
        if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
        uint64_t now = NowTick();
        // The Global\ channel only exists after some program opens the DLL (a regular user cannot create
        // it). Without it the bridge still works for the panel (camera + preview), it just doesn't publish.
        if (!chan_.IsOpen() && now - lastOpenTry >= 1000) {
            lastOpenTry = now;
            if (chan_.Open(true)) {
                chanReady_ = true;
                Log("Channel to the virtual camera connected.");
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
                Log("%s: Live View on (%s).", consumer ? "Webcam in use" : "Panel open", cam_.Name().c_str());
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
                prewarm_ = true;     // preloads the enabled effects
                restarted_ = true;   // resets the temporal state of the effects
                SetEvent(mboxEvent_);
            } else {
                state = ProducerState::NoCamera;
                if (err != lastError) Log("Camera unavailable: %s", err.c_str());
                lastError = err;
            }
        }

        if (cam_.IsConnected() && !wanted && now - lastWanted > 5000) {
            cam_.Disconnect();
            camConnected_ = false;
            state = ProducerState::Idle;
            Log("Webcam no longer in use: Live View off.");
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
                Log("  the camera stopped sending frames (response 0x%04lX); restarting Live View",
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
                    // The T3 Live View refreshes at ~30 fps; wait only a little before asking again.
                    ShortSleep(3);
                    break;
                case CanonCamera::Frame::Error:
                    Log("Lost communication with the camera; trying to reconnect.");
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
                Log("  %.1f frames/s from the camera, %.1f published; bridge CPU %.1f%% of a core", statFrames / secs,
                    double(fo - statOut) / secs, cpuPct);
                Log("  per frame (ms): decode %.2f | GPU: upload %.2f, denoise %.2f, artifacts %.2f, super "
                    "resolution %.2f, mask/blur %.2f, download %.2f | CPU: scale+adjust+background+NV12 %.2f | "
                    "total %.2f",
                    ps.msDecode, ps.msUpload, ps.msDenoise, ps.msArtifact, ps.msSuperRes, ps.msSegment,
                    ps.msDownload, ps.msFinal, ps.msTotal);
                statOut = fo;
                statsTick = tn;
                statFrames = 0;
            }
        } else {
            RunCameraCommands();  // answers with the "camera not connected" error
            Sleep(100);
        }
    }

    {
        PipelineStats ps = pipeline_.Stats();
        double cpu = ProcessCpuSeconds() - cpuBase;
        double secs = double(NowTick() - cpuTick) / 1000.0;
        if (ps.frames > 0 && secs > 1)
            Log("  summary: %llu frames processed; per frame (ms): decode %.2f | GPU: upload %.2f, denoise "
                "%.2f, artifacts %.2f, super resolution %.2f, mask/blur %.2f, download %.2f | CPU: scale+"
                "adjust+background+NV12 %.2f | total %.2f; bridge CPU %.1f%% of a core",
                (unsigned long long)ps.frames, ps.msDecode, ps.msUpload, ps.msDenoise, ps.msArtifact, ps.msSuperRes,
                ps.msSegment, ps.msDownload, ps.msFinal, ps.msTotal, cpu / secs * 100.0);
    }
    LogThreadCpu();
    Log("Shutting down...");
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

}  // namespace canon_eos_webcam
