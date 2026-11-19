// The two worker threads of the service, in the spirit of src/app/bridge.cpp:
//  - capture: the only owner of the PTP session. Connects, fetches the Live View JPEGs, reads the camera events,
//    runs the queued property writes between frames and reconnects by itself;
//  - processing: always takes the most recent JPEG (the others are dropped) and produces the output frame.
// Live View only stays on while somebody consumes the camera (a PipeWire consumer, an open panel, a bench or a
// pending command); it is turned off 5 s after the last consumer leaves.
#pragma once

#include "canon_camera.h"
#include "pipeline.h"
#include "settings.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace canon_eos_webcam {

enum class BridgeState { Idle, Starting, Live, NoCamera };
const char* BridgeStateName(BridgeState s);

struct BridgeStats {
    BridgeState state = BridgeState::Idle;
    bool cameraConnected = false;
    std::string cameraName;
    double fpsIn = 0, fpsOut = 0;
    double msProcess = 0;
    int consumers = 0, panels = 0;
    uint64_t framesIn = 0, framesOut = 0;
};

struct CameraSnapshot {
    bool connected = false;
    std::string name;
    uint32_t gen = 0;
    std::map<uint32_t, PropState> props;
};

// Result of a command run by the capture thread.
enum class CmdResult { Ok, Refused, NotConnected, Failed };

struct CamCommand {
    enum class Kind { Prop, StyleParam, AutoFocus } kind = Kind::Prop;
    uint32_t prop = 0;   // Prop: property code
    uint32_t value = 0;  // Prop: value
    int index = 0;       // StyleParam: 0 contrast, 1 sharpness, 2 saturation, 3 color tone
    int param = 0;       // StyleParam: new value
};

class Bridge {
public:
    explicit Bridge(SettingsStore& settings) : settings_(settings) {}
    ~Bridge() { Stop(); }

    void Start();
    void Stop();

    // Consumers of the camera (any of them keeps Live View on).
    void SetConsumers(int n) { consumers_ = n; Wake(); }
    void SetPanels(int n) { panels_ = n; Wake(); }
    void SetBench(bool on) { bench_ = on; Wake(); }
    void SetPreviewWanted(bool on) { previewWanted_ = on; }
    // Called (from any thread) when something worth telling the control thread happened.
    void SetOnChange(std::function<void()> f) { onChange_ = std::move(f); }

    // Latest processed frame in the output format. False if there is none newer than maxAgeMs.
    bool LatestFrame(std::vector<uint8_t>& out, uint64_t maxAgeMs, uint64_t* seq = nullptr) const;
    // Latest final RGB frame (1920x1080) handed over for the preview (at most ~15 per second).
    bool TakePreview(std::vector<uint8_t>& rgb, uint64_t& seq);

    BridgeStats Stats() const;
    BridgeState State() const {
        std::lock_guard<std::mutex> lk(stateMu_);
        return state_;
    }
    CameraSnapshot Camera() const;

    // Runs the command on the capture thread, between frames. done is called from that thread, with the camera's
    // PTP response code when it answered (0 otherwise). An autofocus submitted while another one is still queued
    // is dropped (done(Ok) right away).
    using CmdDone = std::function<void(CmdResult, uint16_t code)>;
    void Submit(const CamCommand& cmd, CmdDone done);

    // Diagnostics for `bench`.
    void ResetBenchCounters();
    void FillBenchTotals(uint64_t& capFrames, uint64_t& procFrames, double& procMsSum, double& decodeMs,
                         double& adjustMs, double& scaleMs, double& convertMs, uint64_t& restarts) const;

private:
    void CaptureLoop();
    void ProcessLoop();
    void Wake();
    bool Wanted() const;
    void Notify() { if (onChange_) onChange_(); }
    CmdResult RunCommand(const CamCommand& c, uint16_t& code);

    SettingsStore& settings_;
    std::function<void()> onChange_;
    std::atomic<bool> stop_{false};
    std::thread capture_, process_;

    std::atomic<int> consumers_{0}, panels_{0};
    std::atomic<bool> bench_{false}, previewWanted_{false};

    CanonCamera cam_;  // touched by the capture thread only (plus the thread-safe property cache)
    mutable std::mutex stateMu_;
    BridgeState state_ = BridgeState::Idle;
    bool connected_ = false;
    std::string camName_;
    std::atomic<uint32_t> camGen_{0};

    // single-slot mailbox: capture -> processing
    std::mutex mbMu_;
    std::condition_variable mbCv_;
    std::vector<uint8_t> mbJpeg_;
    uint64_t mbSeq_ = 0;

    // command queue
    struct Pending {
        CamCommand cmd;
        CmdDone done;
        uint64_t deadlineMs;
    };
    std::mutex cmdMu_;
    std::condition_variable cmdCv_;
    std::deque<Pending> cmds_;

    // output slot (read by the PipeWire thread)
    mutable std::mutex outMu_;
    std::vector<uint8_t> out_;
    uint64_t outSeq_ = 0, outTimeMs_ = 0;

    // preview slot
    std::mutex pvMu_;
    std::vector<uint8_t> pv_;
    uint64_t pvSeq_ = 0, pvTakenSeq_ = 0;

    // statistics
    mutable std::mutex statMu_;
    double fpsIn_ = 0, fpsOut_ = 0, msProc_ = 0;
    std::atomic<uint64_t> framesIn_{0}, framesOut_{0};
    std::atomic<uint64_t> lastFrameMs_{0};
    double sumMs_ = 0, sumDecode_ = 0, sumAdjust_ = 0, sumScale_ = 0, sumConvert_ = 0;
    uint64_t benchCap0_ = 0, benchProc0_ = 0, benchRestarts0_ = 0;
    double benchSum0_ = 0, benchDec0_ = 0, benchAdj0_ = 0, benchSc0_ = 0, benchCv0_ = 0;
    std::atomic<uint64_t> restartsTotal_{0};
};

}  // namespace canon_eos_webcam
