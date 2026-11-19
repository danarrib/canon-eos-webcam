#include "pw_output.h"

#include "log.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>

#include <cerrno>
#include <cstring>

namespace canon_eos_webcam {

namespace {
// How old the last processed frame may be and still be sent. With the camera connected, a short pause (Live View
// being turned back on, an autofocus) shows the last image frozen instead of a gray flash; with no camera, the
// placeholder comes after a second.
constexpr uint64_t kHoldConnectedMs = 8000;
constexpr uint64_t kHoldNoCameraMs = 1000;

spa_video_format SpaFormat(OutFormat f) {
    return f == OutFormat::I420 ? SPA_VIDEO_FORMAT_I420 : f == OutFormat::Nv12 ? SPA_VIDEO_FORMAT_NV12 : SPA_VIDEO_FORMAT_YUY2;
}
int StrideOf(OutFormat f) { return f == OutFormat::Yuy2 ? kOutWidth * 2 : kOutWidth; }
}  // namespace

struct PwOutput::Impl {
    PwOutput* owner = nullptr;
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_source* timer = nullptr;
    spa_hook coreListener{}, streamListener{};
    std::vector<uint8_t> frame, placeholder;
    size_t frameSize = 0;
    uint64_t delivered = 0, placeholders = 0;
    bool onPlaceholder = false;

    void Fail(const char* why) {
        Log("pipewire: %s", why);
        std::lock_guard<std::mutex> lk(owner->mu_);
        owner->failed_ = true;
        owner->cv_.notify_all();
    }

    void ArmTimer(bool on) {
        if (!timer) return;
        timespec v{0, on ? 33333333L : 0L}, iv{0, on ? 33333333L : 0L};
        pw_loop_update_timer(pw_thread_loop_get_loop(loop), timer, &v, &iv, false);
    }

    static void OnTimer(void* data, uint64_t) {
        auto* s = static_cast<Impl*>(data);
        if (s->stream && s->owner->streaming_) pw_stream_trigger_process(s->stream);
    }

    static void OnState(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
        auto* s = static_cast<Impl*>(data);
        Log("pipewire: stream %s -> %s%s%s", pw_stream_state_as_string(old), pw_stream_state_as_string(state),
            error ? ": " : "", error ? error : "");
        bool streaming = state == PW_STREAM_STATE_STREAMING;
        s->owner->streaming_ = streaming;
        s->ArmTimer(streaming);
        s->owner->bridge_.SetConsumers(streaming ? 1 : 0);
        if (state == PW_STREAM_STATE_ERROR) s->Fail("the stream went into an error state");
    }

    static void OnParam(void* data, uint32_t id, const spa_pod* param) {
        auto* s = static_cast<Impl*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(int(s->frameSize)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(StrideOf(s->owner->format_)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_header)))));
        pw_stream_update_params(s->stream, params, 2);
    }

    static void OnProcess(void* data) {
        auto* s = static_cast<Impl*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
        if (!b) return;
        spa_data& d = b->buffer->datas[0];
        if (d.data && d.maxsize >= s->frameSize) {
            const std::vector<uint8_t>* src = &s->placeholder;
            BridgeState st = s->owner->bridge_.State();
            uint64_t maxAge = st == BridgeState::NoCamera || st == BridgeState::Idle ? kHoldNoCameraMs : kHoldConnectedMs;
            // (the frames/s statistics come from the bridge, so a repeated frame never counts as a camera frame)
            if (s->owner->bridge_.LatestFrame(s->frame, maxAge)) {
                src = &s->frame;
                s->delivered++;
                if (s->onPlaceholder) { s->onPlaceholder = false; Log("pipewire: camera frames again"); }
            } else {
                s->placeholders++;
                if (!s->onPlaceholder && s->delivered) {
                    s->onPlaceholder = true;
                    Log("pipewire: sending the placeholder (state %s, no frame for over %llu s)", BridgeStateName(st),
                        static_cast<unsigned long long>(maxAge / 1000));
                }
            }
            memcpy(d.data, src->data(), s->frameSize);
            d.chunk->offset = 0;
            d.chunk->size = uint32_t(s->frameSize);
            d.chunk->stride = StrideOf(s->owner->format_);
            if (spa_meta_header* h = static_cast<spa_meta_header*>(
                    spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof(spa_meta_header)))) {
                timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                h->pts = int64_t(ts.tv_sec) * SPA_NSEC_PER_SEC + ts.tv_nsec;
                h->flags = 0;
                h->seq = s->delivered + s->placeholders;
                h->dts_offset = 0;
            }
        } else {
            d.chunk->size = 0;
        }
        pw_stream_queue_buffer(s->stream, b);
    }

    static void OnCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
        auto* s = static_cast<Impl*>(data);
        (void)seq;
        Log("pipewire: core error id %u: %s (%s)", id, message, strerror(-res));
        if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET)) s->Fail("the connection to PipeWire was lost");
    }
};

PwOutput::PwOutput(Bridge& bridge, OutFormat format) : bridge_(bridge), format_(format) {}

void PwOutput::Start() {
    stop_ = false;
    thread_ = std::thread([this] { Manage(); });
}

void PwOutput::Stop() {
    if (stop_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

void PwOutput::Manage() {
    pw_init(nullptr, nullptr);
    while (!stop_) {
        failed_ = false;
        if (RunOnce()) Log("pipewire: restarting the connection in 2 s");
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(2), [&] { return stop_.load(); });
    }
    pw_deinit();
}

// Returns true if the connection ran and was lost (so the caller logs the restart).
bool PwOutput::RunOnce() {
    Impl m;
    m.owner = this;
    m.frameSize = OutputFrameSize(format_);
    FillPlaceholder(format_, m.placeholder);

    m.loop = pw_thread_loop_new("canon-eos-webcam-pw", nullptr);
    if (!m.loop) return false;
    m.context = pw_context_new(pw_thread_loop_get_loop(m.loop), nullptr, 0);
    bool ran = false;
    if (m.context && pw_thread_loop_start(m.loop) == 0) {
        pw_thread_loop_lock(m.loop);
        m.core = pw_context_connect(m.context, nullptr, 0);
        if (!m.core) {
            Log("pipewire: couldn't connect to the PipeWire daemon (%s); retrying", strerror(errno));
        } else {
            static const pw_core_events coreEvents = [] {
                pw_core_events e{};
                e.version = PW_VERSION_CORE_EVENTS;
                e.error = Impl::OnCoreError;
                return e;
            }();
            pw_core_add_listener(m.core, &m.coreListener, &coreEvents, &m);
            pw_properties* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Video/Source",
                PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_NODE_NAME, "canon-eos-webcam", PW_KEY_NODE_DESCRIPTION,
                "Canon EOS Webcam", PW_KEY_NODE_NICK, "Canon EOS Webcam", nullptr);
            m.stream = pw_stream_new(m.core, "canon-eos-webcam", props);
            static const pw_stream_events streamEvents = [] {
                pw_stream_events e{};
                e.version = PW_VERSION_STREAM_EVENTS;
                e.state_changed = Impl::OnState;
                e.param_changed = Impl::OnParam;
                e.process = Impl::OnProcess;
                return e;
            }();
            pw_stream_add_listener(m.stream, &m.streamListener, &streamEvents, &m);
            m.timer = pw_loop_add_timer(pw_thread_loop_get_loop(m.loop), Impl::OnTimer, &m);

            uint8_t buf[512];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
            spa_rectangle size = SPA_RECTANGLE(kOutWidth, kOutHeight);
            spa_fraction fps = SPA_FRACTION(30, 1);
            const spa_pod* params[1];
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_Id(SpaFormat(format_)),
                SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps)));
            int r = pw_stream_connect(m.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                      pw_stream_flags((getenv("CANON_EOS_WEBCAM_PW_NODRIVER") ? 0 : PW_STREAM_FLAG_DRIVER) | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
            if (r < 0) {
                Log("pipewire: pw_stream_connect failed: %s", strerror(-r));
            } else {
                Log("pipewire: node canon-eos-webcam published (%dx%d, %s, 30 fps)", kOutWidth, kOutHeight,
                    FormatName(format_));
                connected_ = true;
                ran = true;
            }
        }
        pw_thread_loop_unlock(m.loop);
    }

    // wait until stopped or the connection fails
    if (ran) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_.load() || failed_; });
    }
    connected_ = false;

    if (m.loop) {
        pw_thread_loop_lock(m.loop);
        if (m.timer) {
            m.ArmTimer(false);
            pw_loop_destroy_source(pw_thread_loop_get_loop(m.loop), m.timer);
            m.timer = nullptr;
        }
        if (m.stream) { pw_stream_destroy(m.stream); m.stream = nullptr; }
        if (m.core) { spa_hook_remove(&m.coreListener); pw_core_disconnect(m.core); m.core = nullptr; }
        pw_thread_loop_unlock(m.loop);
        pw_thread_loop_stop(m.loop);
    }
    streaming_ = false;
    bridge_.SetConsumers(0);
    if (m.context) pw_context_destroy(m.context);
    if (m.loop) pw_thread_loop_destroy(m.loop);
    return ran && !stop_;
}

}  // namespace canon_eos_webcam
