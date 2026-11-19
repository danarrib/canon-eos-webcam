// The control channel: a Unix socket (mode 0600) carrying one JSON object per line, served by the control
// thread with a single poll() loop (clients, an eventfd for wake-ups, inotify on the settings directory and the
// settings write debounce).
//
// Client -> service:
//   {"cmd":"get"}                                      one-shot: status, image and camera messages
//   {"cmd":"subscribe","preview":true,"size":640}      panel: counts as a consumer, receives updates (and the
//                                                      preview JPEG when asked); size = preview width (160..1920)
//   {"cmd":"unsubscribe"}
//   {"cmd":"set","key":"image.brightness","value":20,"id":1}   -> {"type":"result","id":1,"ok":true}
// Service -> client: status, image, camera, preview, result (see control.cpp).
#pragma once

#include "bridge.h"
#include "pw_output.h"
#include "settings.h"

#include "../common/json.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace canon_eos_webcam {

class Control {
public:
    Control(Bridge& bridge, SettingsStore& settings, PwOutput* pw) : bridge_(bridge), settings_(settings), pw_(pw) {}
    ~Control();

    // Creates the socket. False (with an English message) if another service already owns it.
    bool Listen(std::string& error);
    // Runs the poll loop on the calling thread until stop becomes true.
    void Run(const std::atomic<bool>& stop);
    // Thread-safe: wakes the loop.
    void Wake();

private:
    struct Client {
        int id = 0;
        int fd = -1;
        std::string in, out;
        bool subscribed = false, preview = false;
        int previewSize = 640;
        uint32_t sentImageVersion = 0, sentCamGen = 0;
        bool sentCamValid = false, sentCamConnected = false;
        uint64_t lastStatusMs = 0;
        uint64_t previewsDropped = 0;
        bool dead = false;
    };
    struct Completed {
        int clientId;
        int64_t reqId;
        std::string error;  // empty = ok
        uint16_t code;      // the camera's PTP response code (0 = none)
    };

    void Accept();
    void ReadClient(Client& c);
    void HandleLine(Client& c, const std::string& line);
    void HandleSet(Client& c, const Json& msg);
    void Send(Client& c, const Json& j);
    Json StatusJson() const;
    Json ImageJson(uint32_t& version) const;
    Json CameraJson(const CameraSnapshot& snap) const;
    void Broadcast();
    void SendPreviews();
    void Reply(Client& c, int64_t id, const std::string& error, uint16_t code = 0);
    void RefreshConsumers();

    Bridge& bridge_;
    SettingsStore& settings_;
    PwOutput* pw_;
    int listenFd_ = -1, wakeFd_ = -1;
    std::string path_;
    int nextId_ = 1;
    std::vector<Client> clients_;
    std::mutex doneMu_;
    std::deque<Completed> done_;
};

}  // namespace canon_eos_webcam
