#include "client.h"

#include "../common/util.h"

#include <glib-unix.h>
#include <glib.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace canon_eos_webcam {

ServiceClient::ServiceClient(MessageFn onMessage, StateFn onState)
    : onMessage_(std::move(onMessage)), onState_(std::move(onState)) {}

ServiceClient::~ServiceClient() {
    Disconnect();
    if (retry_) g_source_remove(retry_);
}

void ServiceClient::Start() {
    TryConnect();
    retry_ = g_timeout_add_seconds(2, reinterpret_cast<GSourceFunc>(&ServiceClient::OnRetry), this);
}

int ServiceClient::OnRetry(void* data) {
    auto* c = static_cast<ServiceClient*>(data);
    if (c->fd_ < 0) c->TryConnect();
    return G_SOURCE_CONTINUE;
}

void ServiceClient::TryConnect() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string p = SocketPath();
    strncpy(addr.sun_path, p.c_str(), sizeof addr.sun_path - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return;
    }
    fd_ = fd;
    in_.clear();
    out_.clear();
    wantOut_ = false;
    watch_ = g_unix_fd_add(fd_, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR),
                           reinterpret_cast<GUnixFDSourceFunc>(&ServiceClient::OnFd), this);
    Subscribe(preview_, width_);
    if (onState_) onState_(true);
}

void ServiceClient::Disconnect() {
    if (fd_ < 0) return;
    if (watch_) g_source_remove(watch_);
    watch_ = 0;
    close(fd_);
    fd_ = -1;
    out_.clear();
    in_.clear();
    if (onState_) onState_(false);
}

void ServiceClient::Subscribe(bool preview, int previewWidth) {
    preview_ = preview;
    width_ = previewWidth;
    Json j = Json::Object();
    j.Set("cmd", "subscribe");
    j.Set("preview", preview);
    j.Set("size", previewWidth);
    Send(j);
}

void ServiceClient::Send(const Json& j) {
    if (fd_ < 0) return;
    if (out_.size() > 1024 * 1024) return;  // the service isn't reading: don't pile up
    out_ += j.Dump();
    out_ += '\n';
    Flush();
}

void ServiceClient::Flush() {
    while (fd_ >= 0 && !out_.empty()) {
        ssize_t n = send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) { out_.erase(0, size_t(n)); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        Disconnect();
        return;
    }
    bool need = fd_ >= 0 && !out_.empty();
    if (need != wantOut_ && fd_ >= 0) {  // watch for writability only while there is something pending
        wantOut_ = need;
        if (watch_) g_source_remove(watch_);
        watch_ = g_unix_fd_add(fd_, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR | (need ? G_IO_OUT : 0)),
                               reinterpret_cast<GUnixFDSourceFunc>(&ServiceClient::OnFd), this);
    }
}

int ServiceClient::OnFd(int, unsigned condition, void* data) {
    auto* c = static_cast<ServiceClient*>(data);
    if (condition & (G_IO_HUP | G_IO_ERR)) { c->watch_ = 0; c->Disconnect(); return G_SOURCE_REMOVE; }
    if (condition & G_IO_OUT) { c->Flush(); if (c->fd_ < 0) return G_SOURCE_REMOVE; }
    if (condition & G_IO_IN) {
        char buf[65536];
        for (;;) {
            ssize_t n = read(c->fd_, buf, sizeof buf);
            if (n > 0) { c->in_.append(buf, size_t(n)); continue; }
            if (n == 0 || (errno != EAGAIN && errno != EINTR)) { c->watch_ = 0; c->Disconnect(); return G_SOURCE_REMOVE; }
            break;
        }
        size_t nl;
        while ((nl = c->in_.find('\n')) != std::string::npos) {
            std::string line = c->in_.substr(0, nl);
            c->in_.erase(0, nl + 1);
            Json j;
            if (Json::Parse(line, j) && c->onMessage_) c->onMessage_(j);
            if (c->fd_ < 0) return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

}  // namespace canon_eos_webcam
