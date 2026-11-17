// canon-eos-webcam (Linux): the service and its diagnostic subcommands.
#include "../common/i18n.h"
#include "../common/util.h"
#include "canon_camera.h"
#include "log.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

using namespace canon_eos_webcam;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }
void InstallSignals() {
    struct sigaction sa{};
    sa.sa_handler = OnSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

int CmdProbe() {
    uint64_t t0 = NowMs();
    CanonCamera cam;
    std::string err;
    if (!cam.Connect(err)) {
        printf("probe: %s\n", err.c_str());
        return 1;
    }
    const UsbCameraInfo& u = cam.UsbInfo();
    printf("device : %s (%04x:%04x) bus %u addr %u, interface %u\n", cam.Name().c_str(), u.vendor, u.product,
           u.bus, u.address, u.interface);
    printf("endpts : in 0x%02x out 0x%02x int 0x%02x, max packet %u\n", u.epIn, u.epOut, u.epInt, u.maxPacket);
    printf("connect: %llu ms\n", static_cast<unsigned long long>(NowMs() - t0));
    std::vector<uint8_t> jpeg;
    uint64_t t1 = NowMs();
    int rc = 1;
    while (NowMs() - t1 < 8000 && !g_stop) {
        auto f = cam.GetFrame(jpeg);
        if (f == CanonCamera::Frame::Ok) {
            std::ofstream o("/tmp/canon-eos-webcam-probe.jpg", std::ios::binary);
            o.write(reinterpret_cast<const char*>(jpeg.data()), std::streamsize(jpeg.size()));
            printf("frame  : %zu bytes after %llu ms -> /tmp/canon-eos-webcam-probe.jpg\n", jpeg.size(),
                   static_cast<unsigned long long>(NowMs() - t1));
            rc = 0;
            break;
        }
        if (f == CanonCamera::Frame::Error) {
            printf("probe: capture failed (usb %s, code 0x%04X)\n", UsbErrorName(cam.LastUsbError()), cam.LastCode());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    if (rc) printf("probe: no frame within 8 s\n");
    cam.Disconnect();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    InitLang(SettingsPath());
    InstallSignals();
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "probe") return CmdProbe();
    fputs(Tr(Str::CliUsage), stdout);
    return cmd.empty() || cmd == "help" || cmd == "--help" ? 0 : 2;
}
