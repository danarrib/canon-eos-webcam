// The PipeWire virtual camera: a Video/Source node (media.role = Camera) with 1280x720 @ 30 fps in a single
// pixel format. The stream is the graph driver: a 30 Hz timer (armed only while a consumer is linked) triggers
// each cycle and the process callback always delivers the last processed frame (repeated when no new one
// arrived), or a dark gray placeholder when there is no recent camera image. A PipeWire restart is survived by
// recreating the whole connection after a short delay.
#pragma once

#include "bridge.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_source;

namespace canon_eos_webcam {

class PwOutput {
public:
    PwOutput(Bridge& bridge, OutFormat format);
    ~PwOutput() { Stop(); }

    void Start();
    void Stop();
    bool Connected() const { return connected_; }
    bool Streaming() const { return streaming_; }

    // PipeWire callbacks (public so the C trampolines can reach them).
    struct Impl;

private:
    void Manage();
    bool RunOnce();

    Bridge& bridge_;
    OutFormat format_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false}, streaming_{false};
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool failed_ = false;
    friend struct Impl;
};

}  // namespace canon_eos_webcam
