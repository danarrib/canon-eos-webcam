// Frame channel between eoscam.exe (user session, talks to the camera, processes the image on the GPU)
// and eoscam-source.dll (loaded by the Frame Server, LocalService account, session 0).
//
// Layout v2: the slots carry ready-made NV12 1920x1080 frames (v1 sent Live View JPEGs).
// The mapping/event names end in "2" so an installed v1 and v2 never get mixed up.
//
// The shared memory lives in the Global\ namespace. Creating objects there requires
// SeCreateGlobalPrivilege, which services have and regular users don't: that is why the DLL
// creates it and the app only opens it. The app keeps the handle open, so the channel survives
// while the app is running, even if the Frame Server unloads the DLL.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>

namespace eoscam {

// {D4DFB62F-F0E4-4DD7-A4AA-EDB7F8E013BC}
inline constexpr GUID kSourceClsid = {0xd4dfb62f, 0xf0e4, 0x4dd7, {0xa4, 0xaa, 0xed, 0xb7, 0xf8, 0xe0, 0x13, 0xbc}};
#define EOSCAM_CLSID_STRING L"{D4DFB62F-F0E4-4DD7-A4AA-EDB7F8E013BC}"

inline constexpr wchar_t kCameraName[] = L"Canon EOS Webcam (eoscam)";
// With the environment variable EOSCAM_LOCAL=1 the channel uses the Local\ namespace (current
// session), which allows testing the source inside a regular process, without the Frame Server.
inline bool UseLocalNamespace() {
    wchar_t v[8];
    return GetEnvironmentVariableW(L"EOSCAM_LOCAL", v, 8) > 0 && v[0] == L'1';
}
inline const wchar_t* MappingName() {
    return UseLocalNamespace() ? L"Local\\EosCamFrames2" : L"Global\\EosCamFrames2";
}
inline const wchar_t* EventName() {
    return UseLocalNamespace() ? L"Local\\EosCamFrameReady2" : L"Global\\EosCamFrameReady2";
}
inline constexpr wchar_t kDataDir[] = L"C:\\ProgramData\\EosCam";

// SYSTEM, LocalService, Administrators and authenticated users have full access.
inline constexpr wchar_t kSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;AU)";

constexpr uint32_t kMagic = 0x4D414345;  // "ECAM"
constexpr uint32_t kLayoutVersion = 2;
constexpr int kSlotCount = 3;

// The single format offered by the virtual camera: NV12 1920x1080 (3,110,400 bytes).
constexpr uint32_t kFrameWidth = 1920;
constexpr uint32_t kFrameHeight = 1080;
constexpr uint32_t kFrameBytes = kFrameWidth * kFrameHeight * 3 / 2;
constexpr uint32_t kMaxFrameBytes = 4u << 20;  // maximum slot size
constexpr uint32_t kFourccNV12 = 0x3231564E;   // "NV12"

// The consumer counts as active if its last heartbeat is more recent than this.
constexpr uint64_t kConsumerTimeoutMs = 2500;
// The producer counts as alive if its last heartbeat is more recent than this.
constexpr uint64_t kProducerTimeoutMs = 3000;

enum class ProducerState : LONG {
    Unknown = 0,
    Idle = 1,       // app running, nobody using the webcam
    Starting = 2,   // turning Live View on
    Streaming = 3,  // sending frames
    NoCamera = 4,   // camera off/disconnected/busy
};

struct alignas(64) FrameSlot {
    volatile LONG seq;  // seqlock: odd while being written
    uint32_t size;      // valid bytes in data
    uint64_t frameNumber;
    uint64_t captureTick;
    uint32_t format;    // kFourccNV12
    uint32_t width;
    uint32_t height;
    uint32_t stride;    // bytes per row of the Y plane
    uint8_t data[kMaxFrameBytes];
};

// Header of a frame read from the channel.
struct FrameMeta {
    uint64_t frameNumber = 0;
    uint64_t captureTick = 0;
    uint32_t format = 0, width = 0, height = 0, stride = 0, size = 0;
};

enum class ReadResult { None, Ok, Torn, Incompatible };

struct alignas(64) SharedHeader {
    uint32_t magic;
    uint32_t layoutVersion;
    volatile LONG64 consumerTick;  // GetTickCount64 of the source's last heartbeat (wants frames)
    volatile LONG64 producerTick;  // GetTickCount64 of the app's last heartbeat
    volatile LONG producerState;   // ProducerState
    volatile LONG latestSlot;      // -1 = no frame yet
    volatile LONG64 frameNumber;   // incremented on every published frame
    volatile LONG consumerWidth;   // informational: resolution requested by the client
    volatile LONG consumerHeight;
};

struct SharedFrames {
    SharedHeader header;
    FrameSlot slots[kSlotCount];
};

inline uint64_t NowTick() { return GetTickCount64(); }

class SharedChannel {
public:
    SharedChannel() = default;
    SharedChannel(const SharedChannel&) = delete;
    SharedChannel& operator=(const SharedChannel&) = delete;
    ~SharedChannel() { Close(); }

    // Opens the channel; if it doesn't exist and allowCreate is set, tries to create it.
    // lastError receives the Windows error code on failure.
    bool Open(bool allowCreate, DWORD* lastError = nullptr) {
        if (frames_) return true;
        HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, MappingName());
        DWORD err = m ? 0 : GetLastError();
        if (!m && allowCreate) {
            PSECURITY_DESCRIPTOR sd = nullptr;
            ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr);
            SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
            ULONGLONG size = sizeof(SharedFrames);
            m = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(size >> 32), DWORD(size),
                                   MappingName());
            err = m ? 0 : GetLastError();
            if (m && !event_) event_ = CreateEventW(&sa, FALSE, FALSE, EventName());
            if (sd) LocalFree(sd);
        }
        if (!m) {
            if (lastError) *lastError = err;
            return false;
        }
        void* v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedFrames));
        if (!v) {
            if (lastError) *lastError = GetLastError();
            CloseHandle(m);
            return false;
        }
        mapping_ = m;
        frames_ = static_cast<SharedFrames*>(v);
        if (!event_) event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EventName());

        SharedHeader& h = frames_->header;
        if (h.magic != kMagic || h.layoutVersion != kLayoutVersion) {
            // freshly created memory (zeroed by the system) or from an old version
            std::memset(&h, 0, sizeof h);
            h.latestSlot = -1;
            h.layoutVersion = kLayoutVersion;
            MemoryBarrier();
            h.magic = kMagic;
        }
        return true;
    }

    void Close() {
        if (frames_) UnmapViewOfFile(frames_);
        if (mapping_) CloseHandle(mapping_);
        if (event_) CloseHandle(event_);
        frames_ = nullptr;
        mapping_ = event_ = nullptr;
    }

    bool IsOpen() const { return frames_ != nullptr; }
    SharedHeader& Header() { return frames_->header; }
    HANDLE Event() const { return event_; }

    // ---- producer side (app)
    // Publishes an NV12 frame (w x h, stride = w). Copies it to the next slot of the ring.
    void PublishNv12(const uint8_t* nv12, uint32_t w, uint32_t h, uint64_t captureTick) {
        uint32_t n = w * h * 3 / 2;
        if (!frames_ || !nv12 || n == 0 || n > kMaxFrameBytes) return;
        SharedHeader& hd = frames_->header;
        LONG cur = hd.latestSlot;
        int slot = (cur < 0 || cur >= kSlotCount) ? 0 : (cur + 1) % kSlotCount;
        FrameSlot& s = frames_->slots[slot];
        InterlockedIncrement(&s.seq);  // odd: writing
        std::memcpy(s.data, nv12, n);
        s.size = n;
        s.format = kFourccNV12;
        s.width = w;
        s.height = h;
        s.stride = w;
        s.frameNumber = uint64_t(hd.frameNumber) + 1;
        s.captureTick = captureTick;
        InterlockedIncrement(&s.seq);  // even: ready
        InterlockedExchange(&hd.latestSlot, slot);
        InterlockedIncrement64(&hd.frameNumber);
        if (event_) SetEvent(event_);
    }

    void SetProducer(ProducerState st) {
        if (!frames_) return;
        InterlockedExchange(&frames_->header.producerState, LONG(st));
        InterlockedExchange64(&frames_->header.producerTick, LONG64(NowTick()));
    }

    bool ConsumerActive() const {
        if (!frames_) return false;
        uint64_t t = uint64_t(frames_->header.consumerTick);
        return t != 0 && NowTick() - t < kConsumerTimeoutMs;
    }

    // ---- consumer side (DLL)
    void ConsumerHeartbeat(LONG w, LONG h) {
        if (!frames_) return;
        InterlockedExchange64(&frames_->header.consumerTick, LONG64(NowTick()));
        InterlockedExchange(&frames_->header.consumerWidth, w);
        InterlockedExchange(&frames_->header.consumerHeight, h);
    }

    uint64_t LatestFrameNumber() const { return frames_ ? uint64_t(frames_->header.frameNumber) : 0; }

    bool ProducerAlive() const {
        if (!frames_) return false;
        uint64_t t = uint64_t(frames_->header.producerTick);
        return t != 0 && NowTick() - t < kProducerTimeoutMs;
    }

    ProducerState Producer() const {
        return frames_ ? ProducerState(frames_->header.producerState) : ProducerState::Unknown;
    }

    // Copies the most recent frame to dst (dstBytes bytes) if it is NV12 expectW x expectH.
    // Incompatible: the producer sent another format/size (different eoscam versions).
    ReadResult ReadLatest(uint8_t* dst, uint32_t dstBytes, uint32_t expectW, uint32_t expectH, FrameMeta& meta) {
        if (!frames_) return ReadResult::None;
        LONG slot = frames_->header.latestSlot;
        if (slot < 0 || slot >= kSlotCount) return ReadResult::None;
        FrameSlot& s = frames_->slots[slot];
        for (int attempt = 0; attempt < 8; attempt++) {
            LONG a = s.seq;
            if (a & 1) { YieldProcessor(); continue; }
            MemoryBarrier();
            FrameMeta m;
            m.size = s.size;
            m.format = s.format;
            m.width = s.width;
            m.height = s.height;
            m.stride = s.stride;
            m.frameNumber = s.frameNumber;
            m.captureTick = s.captureTick;
            bool ok = m.format == kFourccNV12 && m.width == expectW && m.height == expectH &&
                      m.stride == expectW && m.size == expectW * expectH * 3 / 2 && m.size <= dstBytes &&
                      m.size <= kMaxFrameBytes;
            if (ok) std::memcpy(dst, s.data, m.size);
            MemoryBarrier();
            if (s.seq != a) continue;
            meta = m;
            return ok ? ReadResult::Ok : ReadResult::Incompatible;
        }
        return ReadResult::Torn;
    }

private:
    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    SharedFrames* frames_ = nullptr;
};

}  // namespace eoscam
