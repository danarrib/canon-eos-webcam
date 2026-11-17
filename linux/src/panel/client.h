// Connection of the panel to the service: a non-blocking Unix socket driven by the GLib main loop. It
// reconnects by itself (every 2 s) and never blocks the UI.
#pragma once

#include "../common/json.h"

#include <functional>
#include <string>

namespace canon_eos_webcam {

class ServiceClient {
public:
    using MessageFn = std::function<void(const Json&)>;
    using StateFn = std::function<void(bool connected)>;

    ServiceClient(MessageFn onMessage, StateFn onState);
    ~ServiceClient();

    void Start();
    bool Connected() const { return fd_ >= 0; }
    // Queues a message (dropped if not connected).
    void Send(const Json& j);
    // (Re)subscribes: the service then sends updates and, if preview, JPEG frames of the given width.
    void Subscribe(bool preview, int previewWidth);

private:
    void TryConnect();
    void Disconnect();
    void Flush();
    static int OnFd(int fd, unsigned condition, void* data);
    static int OnRetry(void* data);

    MessageFn onMessage_;
    StateFn onState_;
    int fd_ = -1;
    unsigned watch_ = 0, retry_ = 0;
    bool wantOut_ = false;
    std::string in_, out_;
    bool preview_ = true;
    int width_ = 640;
};

}  // namespace canon_eos_webcam
