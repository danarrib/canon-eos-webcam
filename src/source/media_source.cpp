// Media source for a Windows 11 virtual camera (MFCreateVirtualCamera).
//
// The Frame Server creates the Activator (CLSID registered in HKLM), calls ActivateObject and gets the
// MediaSource. It exposes a single NV12 video stream at 1920x1080. The frames come from eoscam.exe through
// shared memory (shared.h); without frames, the stream shows a screen explaining what is missing.

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <wrl/ftm.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "i18n.h"
#include "log.h"
#include "renderer.h"
#include "shared.h"

// User-mode IKsControl (the SDK's ks.h only declares the kernel version).
#ifndef _IKsControl_
#define _IKsControl_
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) = 0;
};
#endif

using namespace Microsoft::WRL;

namespace eoscam {

namespace {

struct VideoSize { UINT w, h; };
// 1920x1080 only: with several options, some programs first opened at 640x480 (4:3) and switched to 16:9 a
// second later. The bridge (eoscam.exe) delivers the NV12 1920x1080 frame already processed on the GPU
// (16:9 crop, AI effects, image adjustments and background); this DLL only copies the frame.
constexpr VideoSize kSizes[] = {{kFrameWidth, kFrameHeight}};
constexpr UINT kFps = 30;

HRESULT CreateNv12Type(UINT w, UINT h, IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MAX, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE_RANGE_MIN, kFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    t->SetUINT32(MF_MT_AVG_BITRATE, w * h * 12 * kFps);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    *out = t.Detach();
    return S_OK;
}

constexpr HRESULT kKsNotFound = HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);

// Text of the warning screen shown while there is no live image.
void DescribeState(SharedChannel& chan, std::wstring& title, std::wstring& detail) {
    if (!chan.IsOpen()) {
        title = L"Canon EOS Webcam";
        detail = TrW(Str::SrcInitializing);
    } else if (!chan.ProducerAlive()) {
        title = TrW(Str::SrcNotRunningTitle);
        detail = TrW(Str::SrcNotRunningDetail);
    } else {
        switch (chan.Producer()) {
            case ProducerState::NoCamera:
                title = TrW(Str::SrcNoCameraTitle);
                detail = TrW(Str::SrcNoCameraDetail);
                break;
            case ProducerState::Streaming:
                title = TrW(Str::SrcWaitingTitle);
                detail = L"";
                break;
            default:
                title = TrW(Str::SrcStartingTitle);
                detail = TrW(Str::SrcStartingDetail);
                break;
        }
    }
}

}  // namespace

// =====================================================================================
// MediaStream

class MediaStream
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>, IKsControl,
                          FtmBase> {
public:
    ~MediaStream() override {
        StopWorker();
        if (requestEvent_) CloseHandle(requestEvent_);
    }

    HRESULT RuntimeClassInitialize(IMFMediaSource* parent, DWORD id) {
        parent_ = parent;
        id_ = id;
        requestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!requestEvent_) return HRESULT_FROM_WIN32(GetLastError());
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (FAILED(hr)) return hr;

        std::vector<ComPtr<IMFMediaType>> types;
        for (auto& s : kSizes) {
            ComPtr<IMFMediaType> t;
            hr = CreateNv12Type(s.w, s.h, &t);
            if (FAILED(hr)) return hr;
            types.push_back(t);
        }
        std::vector<IMFMediaType*> raw;
        for (auto& t : types) raw.push_back(t.Get());
        hr = MFCreateStreamDescriptor(id_, DWORD(raw.size()), raw.data(), &desc_);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaTypeHandler> handler;
        hr = desc_->GetMediaTypeHandler(&handler);
        if (FAILED(hr)) return hr;
        hr = handler->SetCurrentMediaType(raw[0]);
        if (FAILED(hr)) return hr;

        hr = MFCreateAttributes(&attrs_, 8);
        if (FAILED(hr)) return hr;
        for (IMFAttributes* a : {static_cast<IMFAttributes*>(attrs_.Get()), static_cast<IMFAttributes*>(desc_.Get())}) {
            a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
            a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, id_);
            a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
            a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
        }
        return S_OK;
    }

    // ---- IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override { return queue_->BeginGetEvent(cb, state); }
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return queue_->EndGetEvent(r, e); }
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override { return queue_->GetEvent(flags, e); }
    IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override {
        return queue_->QueueEventParamVar(t, g, hr, v);
    }

    // ---- IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return parent_.CopyTo(pp);
    }
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return desc_.CopyTo(pp);
    }
    IFACEMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard<std::mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        if (tokens_.size() > 64) tokens_.pop_front();
        tokens_.emplace_back(token);
        SetEvent(requestEvent_);
        return S_OK;
    }

    // ---- IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE newState) override {
        Log("stream: SetStreamState(%d)", int(newState));
        switch (newState) {
            case MF_STREAM_STATE_RUNNING: {
                ComPtr<IMFMediaTypeHandler> h;
                ComPtr<IMFMediaType> t;
                HRESULT hr = desc_->GetMediaTypeHandler(&h);
                if (SUCCEEDED(hr)) hr = h->GetCurrentMediaType(&t);
                if (FAILED(hr)) return hr;
                return Start(t.Get(), nullptr);
            }
            case MF_STREAM_STATE_STOPPED:
                return Stop();
            case MF_STREAM_STATE_PAUSED: {
                {
                    std::lock_guard<std::mutex> l(mutex_);
                    if (shutdown_) return MF_E_SHUTDOWN;
                    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
                    state_ = MF_STREAM_STATE_PAUSED;
                }
                StopWorker();
                return queue_->QueueEventParamVar(MEStreamPaused, GUID_NULL, S_OK, nullptr);
            }
            default:
                return E_INVALIDARG;
        }
    }
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* current) override {
        if (!current) return E_POINTER;
        std::lock_guard<std::mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *current = state_;
        return S_OK;
    }

    // ---- IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }

    // ---- internal (called by the MediaSource)
    HRESULT Start(IMFMediaType* type, const PROPVARIANT* startPos) {
        UINT32 w = 0, h = 0;
        HRESULT hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        if (FAILED(hr)) return hr;
        bool restart;
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (shutdown_) return MF_E_SHUTDOWN;
            restart = (w != width_ || h != height_);
            width_ = w;
            height_ = h;
            state_ = MF_STREAM_STATE_RUNNING;
        }
        Log("stream: Start %ux%u", w, h);
        {
            std::lock_guard<std::mutex> c(control_);
            if (restart || !worker_.joinable()) {
                StopWorkerLocked();
                stopWorker_ = false;
                worker_ = std::thread([this] { WorkerLoop(); });
            }
        }
        return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, startPos);
    }

    HRESULT Stop() {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (shutdown_) return MF_E_SHUTDOWN;
            state_ = MF_STREAM_STATE_STOPPED;
            tokens_.clear();
        }
        Log("stream: Stop");
        StopWorker();
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            state_ = MF_STREAM_STATE_STOPPED;
            tokens_.clear();
        }
        StopWorker();
        queue_->Shutdown();
        std::lock_guard<std::mutex> l(mutex_);
        parent_.Reset();  // breaks the source <-> stream cycle
    }

    IMFStreamDescriptor* Descriptor() const { return desc_.Get(); }
    IMFAttributes* Attributes() const { return attrs_.Get(); }

private:
    void StopWorker() {
        std::lock_guard<std::mutex> c(control_);
        StopWorkerLocked();
    }
    void StopWorkerLocked() {
        if (!worker_.joinable()) return;
        stopWorker_ = true;
        SetEvent(requestEvent_);
        if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
        else worker_.join();
    }

    bool PopToken(ComPtr<IUnknown>& token, bool& any) {
        std::lock_guard<std::mutex> l(mutex_);
        any = !tokens_.empty();
        if (!any || state_ != MF_STREAM_STATE_RUNNING) return false;
        token = tokens_.front();
        tokens_.pop_front();
        return true;
    }

    bool HasPending() {
        std::lock_guard<std::mutex> l(mutex_);
        return !tokens_.empty() && state_ == MF_STREAM_STATE_RUNNING;
    }

    HRESULT Deliver(const std::vector<uint8_t>& nv12, IUnknown* token) {
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(DWORD(nv12.size()), &buf);
        if (FAILED(hr)) return hr;
        BYTE* p = nullptr;
        hr = buf->Lock(&p, nullptr, nullptr);
        if (FAILED(hr)) return hr;
        memcpy(p, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength(DWORD(nv12.size()));

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10'000'000 / kFps);
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token);
        return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }

    void WorkerLoop() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        UINT w, h;
        {
            std::lock_guard<std::mutex> l(mutex_);
            w = width_;
            h = height_;
        }
        FrameRenderer renderer;
        HRESULT hr = renderer.Init(w, h);
        if (FAILED(hr)) Log("stream: failed to start the renderer (hr=0x%08lX)", (unsigned long)hr);

        SharedChannel chan;
        uint64_t lastOpenTry = 0, shownFrame = 0, lastLiveTick = 0, lastDelivery = 0;
        bool haveLive = false, loggedOpenError = false;
        uint64_t statsTick = NowTick(), statDelivered = 0, statLive = 0, statIncompat = 0, statReadFail = 0;
        double statCopyMs = 0;
        bool incompatible = false;
        std::wstring title, detail;
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);

        Log("stream: worker started %ux%u", w, h);
        while (!stopWorker_) {
            uint64_t now = NowTick();
            if (!chan.IsOpen() && now - lastOpenTry > 1000) {
                lastOpenTry = now;
                DWORD err = 0;
                if (chan.Open(true, &err)) Log("stream: shared channel open");
                else if (!loggedOpenError) {
                    Log("stream: couldn't open/create the shared channel (error %lu)", err);
                    loggedOpenError = true;
                }
            }
            chan.ConsumerHeartbeat(LONG(w), LONG(h));

            if (!HasPending()) {
                WaitForSingleObject(requestEvent_, 100);
                continue;
            }

            uint64_t latest = chan.LatestFrameNumber();
            if (latest == shownFrame) {
                HANDLE ev = chan.Event();
                if (ev) WaitForSingleObject(ev, 10);
                else Sleep(10);
                latest = chan.LatestFrameNumber();
                now = NowTick();
                // no new frame: repeat the last image every now and then so the client doesn't stall
                uint64_t maxGap = (haveLive && now - lastLiveTick < 1500) ? 250 : 100;
                if (latest == shownFrame && now - lastDelivery < maxGap) continue;
            }

            bool live = false;
            if (latest != shownFrame) {
                FrameMeta meta;
                LARGE_INTEGER a, b;
                QueryPerformanceCounter(&a);
                ReadResult rr = chan.ReadLatest(renderer.Nv12Data(), renderer.Nv12Bytes(), w, h, meta);
                QueryPerformanceCounter(&b);
                switch (rr) {
                    case ReadResult::Ok:
                        shownFrame = latest;
                        statCopyMs += double(b.QuadPart - a.QuadPart) * 1000.0 / double(qpf.QuadPart);
                        renderer.InvalidateMessage();
                        if (!haveLive) Log("stream: live image (frame %llu)", (unsigned long long)meta.frameNumber);
                        haveLive = live = true;
                        incompatible = false;
                        lastLiveTick = NowTick();
                        break;
                    case ReadResult::Incompatible:
                        shownFrame = latest;
                        incompatible = true;
                        if (++statIncompat <= 5)
                            Log("stream: incompatible frame %llu (format %08lX %ux%u, %u bytes; expected NV12 %ux%u)",
                                (unsigned long long)meta.frameNumber, (unsigned long)meta.format, meta.width,
                                meta.height, meta.size, w, h);
                        break;
                    default:
                        if (++statReadFail <= 5)
                            Log("stream: failed to read frame %llu from the channel", (unsigned long long)latest);
                        break;
                }
            }
            if (!live && (!haveLive || NowTick() - lastLiveTick > 1500)) {
                haveLive = false;
                if (incompatible) {
                    title = TrW(Str::SrcIncompatibleTitle);
                    detail = TrW(Str::SrcIncompatibleDetail);
                } else {
                    DescribeState(chan, title, detail);
                }
                renderer.RenderMessage(title, detail);
            }

            ComPtr<IUnknown> token;
            bool any = false;
            if (!PopToken(token, any)) continue;
            hr = Deliver(renderer.Nv12(), token.Get());
            if (FAILED(hr)) Log("stream: failed to deliver a sample (hr=0x%08lX)", (unsigned long)hr);
            lastDelivery = NowTick();
            statDelivered++;
            if (live) statLive++;

            if (NowTick() - statsTick >= 10000) {
                double secs = (NowTick() - statsTick) / 1000.0;
                Log("stream: %.1f frames/s delivered (%.1f from the camera), average copy %.2f ms, "
                    "read failures/incompatible %llu/%llu, producer %s state %ld",
                    statDelivered / secs, statLive / secs, statLive ? statCopyMs / statLive : 0.0,
                    (unsigned long long)statReadFail, (unsigned long long)statIncompat,
                    chan.ProducerAlive() ? "alive" : "absent", long(chan.Producer()));
                statsTick = NowTick();
                statDelivered = statLive = 0;
                statCopyMs = 0;
            }
        }
        Log("stream: worker finished");
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    std::mutex mutex_;    // estado
    std::mutex control_;  // worker creation/teardown
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> desc_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFMediaSource> parent_;
    DWORD id_ = 0;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    UINT width_ = 0, height_ = 0;
    std::deque<ComPtr<IUnknown>> tokens_;
    HANDLE requestEvent_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stopWorker_{false};
};

// =====================================================================================
// MediaSource

class MediaSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
                          IKsControl, FtmBase> {
public:
    HRESULT RuntimeClassInitialize() {
        HRESULT hr = MFCreateAttributes(&attrs_, 4);
        if (FAILED(hr)) return hr;
        hr = MFCreateEventQueue(&queue_);
        if (FAILED(hr)) return hr;
        hr = MakeAndInitialize<MediaStream>(&stream_, static_cast<IMFMediaSource*>(this), DWORD(0));
        if (FAILED(hr)) return hr;
        IMFStreamDescriptor* sds[] = {stream_->Descriptor()};
        hr = MFCreatePresentationDescriptor(1, sds, &pd_);
        if (FAILED(hr)) return hr;
        pd_->SelectStream(0);
        Log("source: created");
        return S_OK;
    }

    // ---- IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override { return queue_->BeginGetEvent(cb, state); }
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return queue_->EndGetEvent(r, e); }
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override { return queue_->GetEvent(flags, e); }
    IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override {
        return queue_->QueueEventParamVar(t, g, hr, v);
    }

    // ---- IMFMediaSource
    IFACEMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }

    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(pp);
    }

    IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmt, const PROPVARIANT* pos) override {
        if (!pd || !pos) return E_INVALIDARG;
        if (fmt && *fmt != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;

        DWORD count = 0;
        HRESULT hr = pd->GetStreamDescriptorCount(&count);
        if (FAILED(hr)) return hr;
        if (count != 1) return E_INVALIDARG;
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr)) return hr;

        if (selected) {
            ComPtr<IMFMediaTypeHandler> handler, mine;
            ComPtr<IMFMediaType> type;
            hr = sd->GetMediaTypeHandler(&handler);
            if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
            if (FAILED(hr)) return hr;
            if (SUCCEEDED(stream_->Descriptor()->GetMediaTypeHandler(&mine))) mine->SetCurrentMediaType(type.Get());
            pd_->SelectStream(0);
            hr = queue_->QueueEventParamUnk(streamStartedBefore_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                            static_cast<IMFMediaStream*>(stream_.Get()));
            if (FAILED(hr)) return hr;
            streamStartedBefore_ = true;
            hr = stream_->Start(type.Get(), pos);
            if (FAILED(hr)) return hr;
        } else {
            pd_->DeselectStream(0);
            stream_->Stop();
        }

        PROPVARIANT t;
        PropVariantInit(&t);
        t.vt = VT_I8;
        t.hVal.QuadPart = (pos->vt == VT_I8) ? pos->hVal.QuadPart : MFGetSystemTime();
        return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &t);
    }

    IFACEMETHODIMP Stop() override {
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        Log("source: Stop");
        stream_->Stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }

    IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }

    IFACEMETHODIMP Shutdown() override {
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        Log("source: Shutdown");
        shutdown_ = true;
        stream_->Shutdown();
        queue_->Shutdown();
        return S_OK;
    }

    // ---- IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return attrs_.CopyTo(pp);
    }
    IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::recursive_mutex> l(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        IMFAttributes* a = stream_->Attributes();
        a->AddRef();
        *pp = a;
        return S_OK;
    }
    IFACEMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }

    // ---- IMFGetService
    IFACEMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // ---- IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return kKsNotFound; }

private:
    std::recursive_mutex mutex_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<MediaStream> stream_;
    bool shutdown_ = false;
    bool streamStartedBefore_ = false;
};

// =====================================================================================
// Activator (registered COM class)

class __declspec(uuid("D4DFB62F-F0E4-4DD7-A4AA-EDB7F8E013BC")) Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase> {
public:
    Activator() {
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        MFCreateAttributes(&attrs_, 4);
        Log("activator: created");
    }
    ~Activator() override { MFShutdown(); }

    // ---- IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        std::lock_guard<std::mutex> l(mutex_);
        if (!source_) {
            HRESULT hr = MakeAndInitialize<MediaSource>(&source_);
            if (FAILED(hr)) {
                Log("activator: failed to create the source (hr=0x%08lX)", (unsigned long)hr);
                return hr;
            }
        }
        return source_->QueryInterface(riid, ppv);
    }
    IFACEMETHODIMP ShutdownObject() override {
        std::lock_guard<std::mutex> l(mutex_);
        if (source_) source_->Shutdown();
        source_.Reset();
        return S_OK;
    }
    IFACEMETHODIMP DetachObject() override {
        std::lock_guard<std::mutex> l(mutex_);
        source_.Reset();
        return S_OK;
    }

    // ---- IMFAttributes (forwarded to an internal store)
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attrs_->Compare(o, m, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return attrs_->GetStringLength(k, n); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 cch, UINT32* n) override { return attrs_->GetString(k, s, cch, n); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override { return attrs_->GetAllocatedString(k, s, n); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return attrs_->GetBlobSize(k, n); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 cb, UINT32* n) override { return attrs_->GetBlob(k, b, cb, n); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return attrs_->GetAllocatedBlob(k, b, n); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* pv) override { return attrs_->GetUnknown(k, riid, pv); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 cb) override { return attrs_->SetBlob(k, b, cb); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attrs_->SetUnknown(k, u); }
    IFACEMETHODIMP LockStore() override { return attrs_->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* n) override { return attrs_->GetCount(n); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attrs_->CopyAllItems(dest); }

private:
    std::mutex mutex_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<MediaSource> source_;
};

CoCreatableClass(Activator);

}  // namespace eoscam

// =====================================================================================
// DLL exports

static HMODULE g_module = nullptr;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    return Module<InProc>::GetModule().GetClassObject(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow() { return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE; }

static const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\" EOSCAM_CLSID_STRING;

static HRESULT SetRegString(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value) {
    HKEY k = nullptr;
    LSTATUS s = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    s = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                       DWORD((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring inproc = std::wstring(kClsidKey) + L"\\InprocServer32";
    HRESULT hr = SetRegString(HKEY_LOCAL_MACHINE, kClsidKey, nullptr, L"EOS Webcam Source (eoscam)");
    if (SUCCEEDED(hr)) hr = SetRegString(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr, path);
    if (SUCCEEDED(hr)) hr = SetRegString(HKEY_LOCAL_MACHINE, inproc.c_str(), L"ThreadingModel", L"Both");
    return hr;
}

STDAPI DllUnregisterServer() {
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}
