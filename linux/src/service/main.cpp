// canon-eos-webcam (Linux): the service and its diagnostic subcommands.
#include "../common/i18n.h"
#include "../common/util.h"
#include "bridge.h"
#include "canon_camera.h"
#include "pipeline.h"
#include "settings.h"
#include "log.h"

#include <algorithm>
#include <csignal>
#include <ctime>
#include <iterator>
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

int CmdProcess(int argc, char** argv) {
    if (argc < 4) { fputs(Tr(Str::CliUsage), stdout); return 2; }
    std::string text;
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) { printf("process: can't read %s\n", argv[2]); return 1; }
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SettingsStore store;
    store.Load(SettingsPath());
    Settings s = store.Get();
    Pipeline pipe;
    std::vector<uint8_t> out, rgb;
    PipelineTimes tm;
    if (!pipe.Process(jpeg.data(), jpeg.size(), s, out, &rgb, &tm)) { printf("process: couldn't decode %s\n", argv[2]); return 1; }
    std::ofstream o(argv[3], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    printf("process: %dx%d -> crop y=%d h=%d -> %dx%d %s (%zu bytes), %.1f ms (decode %.1f, adjust %.1f, scale %.1f, convert %.1f)\n",
           pipe.SrcWidth(), pipe.SrcHeight(), pipe.CropY(), pipe.CropHeight(), kOutWidth, kOutHeight, FormatName(s.format),
           out.size(), tm.totalMs, tm.decodeMs, tm.adjustMs, tm.scaleMs, tm.convertMs);
    if (argc > 4) {
        std::vector<uint8_t> j;
        if (EncodeJpegRgb(rgb.data(), kOutWidth, kOutHeight, 92, j)) {
            std::ofstream d(argv[4], std::ios::binary);
            d.write(reinterpret_cast<const char*>(j.data()), std::streamsize(j.size()));
            printf("process: debug JPEG written to %s\n", argv[4]);
        }
    }
    if (argc > 5) {  // timing loop: process <in> <out> <debug|-> <iterations>
        int n = atoi(argv[5]);
        auto t0 = NowSec();
        for (int i = 0; i < n; i++) pipe.Process(jpeg.data(), jpeg.size(), s, out, nullptr, nullptr);
        printf("process: %d iterations, %.2f ms each\n", n, (NowSec() - t0) * 1000.0 / n);
    }
    return 0;
}

double CpuSeconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

int CmdBench(int argc, char** argv) {
    int seconds = argc > 2 ? std::max(1, atoi(argv[2])) : 10;
    SettingsStore store;
    store.Load(SettingsPath());
    Bridge bridge(store);
    bridge.SetBench(true);
    bridge.Start();
    // wait for the first frame (the camera needs ~1.2 s after Live View is turned on)
    uint64_t t0 = NowMs();
    while (!g_stop && bridge.Stats().framesOut == 0 && NowMs() - t0 < 15000)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (bridge.Stats().framesOut == 0) {
        printf("bench: no frames (camera not available?)\n");
        bridge.Stop();
        return 1;
    }
    printf("bench: first frame after %llu ms, measuring %d s\n", static_cast<unsigned long long>(NowMs() - t0), seconds);
    bridge.ResetBenchCounters();
    double cpu0 = CpuSeconds();
    uint64_t start = NowMs();
    for (int i = 0; i < seconds && !g_stop; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        BridgeStats st = bridge.Stats();
        printf("  t=%2ds in %.1f fps  out %.1f fps  %.1f ms/frame\n", i + 1, st.fpsIn, st.fpsOut, st.msProcess);
        fflush(stdout);
    }
    double elapsed = double(NowMs() - start) / 1000.0, cpu = CpuSeconds() - cpu0;
    uint64_t cap, proc, restarts;
    double sum, dec, adj, sc, cv;
    bridge.FillBenchTotals(cap, proc, sum, dec, adj, sc, cv, restarts);
    printf("bench: capture %.1f fps (%llu frames), processed %.1f fps\n", double(cap) / elapsed,
           static_cast<unsigned long long>(cap), double(proc) / elapsed);
    if (proc)
        printf("bench: %.2f ms/frame (decode %.2f, adjust %.2f, scale %.2f, convert %.2f)\n", sum / double(proc),
               dec / double(proc), adj / double(proc), sc / double(proc), cv / double(proc));
    printf("bench: CPU %.0f%% of one core, reconnections %llu\n", cpu / elapsed * 100.0,
           static_cast<unsigned long long>(restarts));
    bridge.Stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    InitLang(SettingsPath());
    InstallSignals();
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "probe") return CmdProbe();
    if (cmd == "process") return CmdProcess(argc, argv);
    if (cmd == "bench") return CmdBench(argc, argv);
    fputs(Tr(Str::CliUsage), stdout);
    return cmd.empty() || cmd == "help" || cmd == "--help" ? 0 : 2;
}
