#include "bridge.h"

#include "../common/camera_values.h"
#include "../common/util.h"
#include "log.h"

#include <algorithm>
#include <chrono>

namespace canon_eos_webcam {

namespace {
constexpr uint64_t kLiveViewOffDelayMs = 5000;
constexpr uint64_t kReconnectIntervalMs = 2000;
constexpr uint64_t kPreviewIntervalMs = 66;  // ~15 per second
constexpr uint64_t kCommandWaitMs = 6000;    // how long a command waits for the camera to connect
}  // namespace

const char* BridgeStateName(BridgeState s) {
    switch (s) {
        case BridgeState::Idle: return "idle";
        case BridgeState::Starting: return "starting";
        case BridgeState::Live: return "live";
        case BridgeState::NoCamera: return "nocamera";
    }
    return "idle";
}

void Bridge::Start() {
    stop_ = false;
    capture_ = std::thread([this] { CaptureLoop(); });
    process_ = std::thread([this] { ProcessLoop(); });
}

void Bridge::Stop() {
    if (stop_.exchange(true)) return;
    cmdCv_.notify_all();
    mbCv_.notify_all();
    if (capture_.joinable()) capture_.join();
    if (process_.joinable()) process_.join();
    // commands that never ran
    std::deque<Pending> rest;
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        rest.swap(cmds_);
    }
    for (auto& p : rest) p.done(CmdResult::NotConnected);
}

void Bridge::Wake() {
    { std::lock_guard<std::mutex> lk(cmdMu_); }  // so a waiter between its check and its wait can't miss the notify
    cmdCv_.notify_all();
}

bool Bridge::Wanted() const {
    if (consumers_ > 0 || panels_ > 0 || bench_) return true;
    return false;
}

void Bridge::Submit(const CamCommand& cmd, std::function<void(CmdResult)> done) {
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        cmds_.push_back({cmd, std::move(done), NowMs() + kCommandWaitMs});
    }
    cmdCv_.notify_all();
}

CmdResult Bridge::RunCommand(const CamCommand& c) {
    PtpResult r;
    if (c.kind == CamCommand::Kind::Prop) {
        r = cam_.WriteProp(c.prop, c.value);
    } else {
        PropState st;
        if (!cam_.GetProp(kPropPictureStyle, st) || !st.hasValue) return CmdResult::Refused;
        uint32_t sp = StylePropFor(st.value);
        PropState ps;
        std::vector<uint32_t> words;
        size_t base = 0;
        if (!sp || !cam_.GetProp(sp, ps) || !StyleParamWords(ps.raw, words, base)) return CmdResult::Refused;
        words[base + size_t(c.index)] = uint32_t(int32_t(c.param));
        r = cam_.WritePropWords(sp, words);
    }
    if (r.ok()) return CmdResult::Ok;
    return r.answered() ? CmdResult::Refused : CmdResult::Failed;
}

void Bridge::CaptureLoop() {
    uint64_t lastWantedMs = 0, lastTryMs = 0, lastRateMs = NowMs(), lastLogMs = 0;
    uint64_t frames = 0;
    std::string lastErr;
    bool wasLive = false;
    auto setState = [&](BridgeState s) {
        std::lock_guard<std::mutex> lk(stateMu_);
        state_ = s;
    };
    while (!stop_) {
        uint64_t now = NowMs();
        // ---- commands (they also count as a consumer while they wait for the camera)
        bool havePending;
        {
            std::lock_guard<std::mutex> lk(cmdMu_);
            havePending = !cmds_.empty();
        }
        bool wanted = Wanted() || havePending;
        if (wanted) lastWantedMs = now;

        if (cam_.IsConnected() && !wanted && now - lastWantedMs > kLiveViewOffDelayMs) {
            Log("camera: no consumers for 5 s, turning Live View off");
            cam_.Disconnect();
            { std::lock_guard<std::mutex> lk(stateMu_); connected_ = false; }
            setState(BridgeState::Idle);
            wasLive = false;
            Notify();
        }

        if (!cam_.IsConnected()) {
            if (!wanted) {
                setState(BridgeState::Idle);
                std::unique_lock<std::mutex> lk(cmdMu_);
                cmdCv_.wait_for(lk, std::chrono::milliseconds(1000));
                continue;
            }
            // fail the commands that waited too long
            {
                std::deque<Pending> expired;
                {
                    std::lock_guard<std::mutex> lk(cmdMu_);
                    for (auto it = cmds_.begin(); it != cmds_.end();) {
                        if (now > it->deadlineMs) { expired.push_back(std::move(*it)); it = cmds_.erase(it); }
                        else ++it;
                    }
                }
                for (auto& p : expired) p.done(CmdResult::NotConnected);
            }
            if (now - lastTryMs < kReconnectIntervalMs) {
                std::unique_lock<std::mutex> lk(cmdMu_);
                cmdCv_.wait_for(lk, std::chrono::milliseconds(100));
                continue;
            }
            lastTryMs = now;
            setState(BridgeState::Starting);
            std::string err;
            if (cam_.Connect(err)) {
                Log("camera: connected (%s)", cam_.Name().c_str());
                { std::lock_guard<std::mutex> lk(stateMu_); connected_ = true; camName_ = cam_.Name(); }
                cam_.SetDumpEvents(EnvFlag("CANON_EOS_WEBCAM_DUMP_EVENTS"));
                lastErr.clear();
                frames = 0;
                lastRateMs = NowMs();
                Notify();
            } else {
                setState(BridgeState::NoCamera);
                if (err != lastErr || now - lastLogMs > 30000) {
                    Log("camera: %s", err.c_str());
                    lastErr = err;
                    lastLogMs = now;
                }
                { std::lock_guard<std::mutex> lk(stateMu_); connected_ = false; }
                Notify();
            }
            continue;
        }

        // ---- connected: queued commands first
        for (;;) {
            Pending p;
            {
                std::lock_guard<std::mutex> lk(cmdMu_);
                if (cmds_.empty()) break;
                p = std::move(cmds_.front());
                cmds_.pop_front();
            }
            CmdResult r = RunCommand(p.cmd);
            p.done(r);
            if (cam_.PollEvents()) Notify();
        }

        cam_.SetEventInterval(panels_ > 0 ? 250 : 1000);
        uint32_t genBefore = cam_.PropsGeneration();
        std::vector<uint8_t> jpeg;
        auto f = cam_.GetFrame(jpeg);
        uint32_t genNow = cam_.PropsGeneration();
        if (genNow != genBefore) { camGen_ = genNow; Notify(); }
        if (f == CanonCamera::Frame::Ok) {
            frames++;
            framesIn_++;
            lastFrameMs_ = NowMs();
            {
                std::lock_guard<std::mutex> lk(mbMu_);
                mbJpeg_.swap(jpeg);
                mbSeq_++;
            }
            mbCv_.notify_one();
            if (!wasLive) { wasLive = true; setState(BridgeState::Live); Notify(); }
        } else if (f == CanonCamera::Frame::NotReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        } else {
            Log("camera: lost the connection (usb %s, code 0x%04X), reconnecting", UsbErrorName(cam_.LastUsbError()),
                cam_.LastCode());
            restartsTotal_ += 1;
            cam_.Disconnect();
            { std::lock_guard<std::mutex> lk(stateMu_); connected_ = false; }
            setState(BridgeState::Starting);
            wasLive = false;
            lastTryMs = NowMs() - kReconnectIntervalMs + 500;
            Notify();
        }
        if (wasLive && NowMs() - lastFrameMs_ > 1500) {
            wasLive = false;
            setState(BridgeState::Starting);
            Notify();
        }

        now = NowMs();
        if (now - lastRateMs >= 1000) {
            std::lock_guard<std::mutex> lk(statMu_);
            fpsIn_ = double(frames) * 1000.0 / double(now - lastRateMs);
            frames = 0;
            lastRateMs = now;
        }
    }
    if (cam_.IsConnected()) cam_.Disconnect();  // Live View off, remote mode off, session closed
}

void Bridge::ProcessLoop() {
    Pipeline pipe;
    std::vector<uint8_t> jpeg, out, rgb;
    uint64_t seen = 0, lastPreviewMs = 0, lastRateMs = NowMs(), frames = 0, lastLogMs = NowMs();
    double sum = 0;
    uint64_t logFramesIn0 = 0;
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(mbMu_);
            mbCv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return stop_ || mbSeq_ != seen; });
            if (stop_) break;
            if (mbSeq_ == seen) {
                // no frames: report zero fps
                std::lock_guard<std::mutex> sl(statMu_);
                if (NowMs() - lastRateMs >= 1000) { fpsOut_ = 0; lastRateMs = NowMs(); frames = 0; }
                continue;
            }
            seen = mbSeq_;
            jpeg.swap(mbJpeg_);
        }
        Settings s = settings_.Get();
        PipelineTimes tm;
        uint64_t now = NowMs();
        bool wantPreview = previewWanted_ && now - lastPreviewMs >= kPreviewIntervalMs;
        if (!pipe.Process(jpeg.data(), jpeg.size(), s, out, wantPreview ? &rgb : nullptr, &tm)) {
            Log("pipeline: dropped a frame that could not be decoded");
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(outMu_);
            out_.swap(out);
            outSeq_++;
            outTimeMs_ = NowMs();
        }
        if (wantPreview) {
            lastPreviewMs = now;
            {
                std::lock_guard<std::mutex> lk(pvMu_);
                pv_.swap(rgb);
                pvSeq_++;
            }
            Notify();
        }
        framesOut_++;
        frames++;
        sum += tm.totalMs;
        {
            std::lock_guard<std::mutex> lk(statMu_);
            sumMs_ += tm.totalMs;
            sumDecode_ += tm.decodeMs;
            sumAdjust_ += tm.adjustMs;
            sumScale_ += tm.scaleMs;
            sumConvert_ += tm.convertMs;
        }
        now = NowMs();
        if (now - lastRateMs >= 1000) {
            std::lock_guard<std::mutex> lk(statMu_);
            fpsOut_ = double(frames) * 1000.0 / double(now - lastRateMs);
            msProc_ = frames ? sum / double(frames) : 0;
            frames = 0;
            sum = 0;
            lastRateMs = now;
        }
        if (now - lastLogMs >= 60000) {
            uint64_t in = framesIn_;
            Log("stats: in %.1f fps, out %.1f fps, %.1f ms/frame (decode %.1f, adjust %.1f, scale %.1f, convert %.1f)",
                double(in - logFramesIn0) * 1000.0 / double(now - lastLogMs), fpsOut_, msProc_,
                tm.decodeMs, tm.adjustMs, tm.scaleMs, tm.convertMs);
            logFramesIn0 = in;
            lastLogMs = now;
        }
    }
}

bool Bridge::LatestFrame(std::vector<uint8_t>& out, uint64_t maxAgeMs, uint64_t* seq) const {
    std::lock_guard<std::mutex> lk(outMu_);
    if (out_.empty() || NowMs() - outTimeMs_ > maxAgeMs) return false;
    out = out_;
    if (seq) *seq = outSeq_;
    return true;
}

bool Bridge::TakePreview(std::vector<uint8_t>& rgb, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(pvMu_);
    if (pvSeq_ == pvTakenSeq_) return false;
    rgb.swap(pv_);
    seq = pvTakenSeq_ = pvSeq_;
    return true;
}

BridgeStats Bridge::Stats() const {
    BridgeStats s;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        s.state = state_;
        s.cameraConnected = connected_;
        s.cameraName = camName_;
    }
    {
        std::lock_guard<std::mutex> lk(statMu_);
        s.fpsIn = fpsIn_;
        s.fpsOut = fpsOut_;
        s.msProcess = msProc_;
    }
    if (s.state == BridgeState::Idle) s.fpsIn = s.fpsOut = 0;
    s.consumers = consumers_;
    s.panels = panels_;
    s.framesIn = framesIn_;
    s.framesOut = framesOut_;
    return s;
}

CameraSnapshot Bridge::Camera() const {
    CameraSnapshot c;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        c.connected = connected_;
        c.name = camName_;
    }
    if (c.connected) {
        c.props = cam_.AllProps();
        c.gen = cam_.PropsGeneration();
    } else {
        c.gen = camGen_;
    }
    return c;
}

void Bridge::ResetBenchCounters() {
    std::lock_guard<std::mutex> lk(statMu_);
    benchCap0_ = framesIn_;
    benchProc0_ = framesOut_;
    benchSum0_ = sumMs_;
    benchDec0_ = sumDecode_;
    benchAdj0_ = sumAdjust_;
    benchSc0_ = sumScale_;
    benchCv0_ = sumConvert_;
    benchRestarts0_ = restartsTotal_;
}

void Bridge::FillBenchTotals(uint64_t& capFrames, uint64_t& procFrames, double& procMsSum, double& decodeMs,
                             double& adjustMs, double& scaleMs, double& convertMs, uint64_t& restarts) const {
    std::lock_guard<std::mutex> lk(statMu_);
    capFrames = framesIn_ - benchCap0_;
    procFrames = framesOut_ - benchProc0_;
    procMsSum = sumMs_ - benchSum0_;
    decodeMs = sumDecode_ - benchDec0_;
    adjustMs = sumAdjust_ - benchAdj0_;
    scaleMs = sumScale_ - benchSc0_;
    convertMs = sumConvert_ - benchCv0_;
    restarts = restartsTotal_ - benchRestarts0_;
}

}  // namespace canon_eos_webcam
