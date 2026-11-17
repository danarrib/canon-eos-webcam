// canon-eos-webcam (Linux): the service and its diagnostic subcommands.
#include "../common/i18n.h"
#include "../common/util.h"
#include "../common/camera_values.h"
#include "../common/json.h"
#include "bridge.h"
#include "control.h"
#include "pw_output.h"
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
#include <cmath>
#include <fstream>
#include <functional>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
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


int CmdRun() {
    SettingsStore store;
    store.Load(SettingsPath());
    Settings s = store.Get();
    Log("canon-eos-webcam service starting (settings %s, format %s)", SettingsPath().c_str(), FormatName(s.format));
    Bridge bridge(store);
    PwOutput pw(bridge, s.format);
    Control control(bridge, store, &pw);
    std::string err;
    if (!control.Listen(err)) {
        Log("%s", err.c_str());
        if (err.find("already owns") != std::string::npos) fprintf(stderr, Tr(Str::CliAlreadyRunning), SocketPath().c_str());
        return 1;
    }
    bridge.SetOnChange([&] { control.Wake(); });
    bridge.Start();
    pw.Start();
    control.Run(g_stop);
    Log("shutting down");
    pw.Stop();
    bridge.Stop();  // turns Live View off, leaves remote mode and closes the session
    return 0;
}

// ---- clients of the running service (get / set)

int ConnectService() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string p = SocketPath();
    strncpy(addr.sun_path, p.c_str(), sizeof addr.sun_path - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        fprintf(stderr, Tr(Str::CliNoService), p.c_str());
        return -1;
    }
    return fd;
}

// Reads lines until pred returns true for one of them (or the timeout). Returns false on timeout/disconnect.
bool ReadUntil(int fd, int timeoutMs, const std::function<bool(const Json&)>& pred, std::string& buf) {
    uint64_t end = NowMs() + uint64_t(timeoutMs);
    for (;;) {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            Json j;
            if (Json::Parse(line, j) && pred(j)) return true;
        }
        int left = int(int64_t(end) - int64_t(NowMs()));
        if (left <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) return false;
        char tmp[65536];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf.append(tmp, size_t(n));
    }
}

int CmdGet() {
    int fd = ConnectService();
    if (fd < 0) return 1;
    const char* req = "{\"cmd\":\"get\"}\n";
    if (write(fd, req, strlen(req)) < 0) return 1;
    std::string buf;
    int got = 0;
    bool ok = ReadUntil(fd, 3000, [&](const Json& j) {
        const std::string& t = j["type"].Str();
        if (t == "status") {
            printf("state     : %s\n", j["state"].Str().c_str());
            printf("camera    : %s%s\n", j["camera"].Bool() ? "connected" : "not connected",
                   j["camera"].Bool() ? (" (" + j["name"].Str() + ")").c_str() : "");
            printf("fps       : in %.1f, out %.1f, %.1f ms/frame\n", j["fps_in"].Num(), j["fps_out"].Num(), j["ms"].Num());
            printf("consumers : %d PipeWire, %d panel(s); pipewire %s\n", j["consumers"].Int(), j["panels"].Int(),
                   j["pipewire"].Bool() ? "connected" : "not connected");
            got++;
        } else if (t == "image") {
            printf("image     : brightness %d, contrast %d, gamma %.2f, temperature %d, tint %d, saturation %d\n",
                   j["brightness"].Int(), j["contrast"].Int(), j["gamma"].Num(), j["temperature"].Int(), j["tint"].Int(),
                   j["saturation"].Int());
            got++;
        } else if (t == "camera") {
            printf("camera props (gen %d):\n", j["gen"].Int());
            for (auto& kv : j["props"].Members()) {
                uint32_t code = uint32_t(strtoul(kv.first.c_str(), nullptr, 16));
                const Json& p = kv.second;
                if (PropName(code).empty()) continue;
                printf("  %s %-28s %s\n", kv.first.c_str(), PropName(code).c_str(),
                       p.Has("value") ? PropValueText(code, uint32_t(p["value"].Num())).c_str() : "-");
            }
            if (j["props"].Has("D110")) {  // parameters of the current Picture Style
                char key[16];
                snprintf(key, sizeof key, "%04X", StylePropFor(uint32_t(j["props"]["D110"]["value"].Num())));
                std::vector<uint8_t> raw;
                const std::string& hx = j["props"][key]["raw"].Str();
                for (size_t i = 0; i + 1 < hx.size(); i += 2) raw.push_back(uint8_t(strtoul(hx.substr(i, 2).c_str(), nullptr, 16)));
                int v[4];
                if (GetStyleParam(raw, 0, v[0]) && GetStyleParam(raw, 1, v[1]) && GetStyleParam(raw, 2, v[2]) && GetStyleParam(raw, 3, v[3]))
                    printf("  style parameters: contrast %d, sharpness %d, saturation %d, color tone %d\n", v[0], v[1], v[2], v[3]);
            }
            got++;
        }
        return got >= 3;
    }, buf);
    close(fd);
    if (!ok) { fprintf(stderr, "%s\n", Tr(Str::CliTimeout)); return 1; }
    return 0;
}

int CmdSet(int argc, char** argv) {
    if (argc < 4) { fputs(Tr(Str::CliUsage), stdout); return 2; }
    std::string key = argv[2], value = argv[3];
    int fd = ConnectService();
    if (fd < 0) return 1;
    Json req = Json::Object();
    req.Set("cmd", "set");
    req.Set("key", key);
    char* end = nullptr;
    double d = strtod(value.c_str(), &end);
    if (!value.empty() && *end == 0 && value.compare(0, 2, "0x") != 0) req.Set("value", d);
    else req.Set("value", value);
    req.Set("id", 1);
    std::string line = req.Dump() + "\n";
    if (write(fd, line.data(), line.size()) < 0) return 1;
    std::string buf;
    int rc = 1;
    bool ok = ReadUntil(fd, 12000, [&](const Json& j) {
        if (j["type"].Str() != "result") return false;
        if (j["ok"].Bool()) { fputs(Tr(Str::CliSetOk), stdout); rc = 0; return true; }
        const std::string& e = j["error"].Str();
        char msg[200];
        if (e == "bad_key") snprintf(msg, sizeof msg, Tr(Str::CliBadKey), key.c_str());
        else if (e == "bad_value") snprintf(msg, sizeof msg, Tr(Str::CliBadValue), value.c_str());
        else if (e == "out_of_range") snprintf(msg, sizeof msg, "%s", Tr(Str::CliOutOfRange));
        else if (e == "refused") snprintf(msg, sizeof msg, "%s", Tr(Str::CliRefused));
        else if (e == "not_connected") snprintf(msg, sizeof msg, "%s", Tr(Str::CliNotConnected));
        else snprintf(msg, sizeof msg, "%s", e.c_str());
        printf(Tr(Str::CliSetFailed), msg);
        return true;
    }, buf);
    close(fd);
    if (!ok) { fprintf(stderr, "%s\n", Tr(Str::CliTimeout)); return 1; }
    return rc;
}

// Lists every known camera property (the service must be stopped: only one process can own the session).
int CmdCam() {
    CanonCamera cam;
    std::string err;
    if (!cam.Connect(err)) { printf("cam: %s\n", err.c_str()); return 1; }
    printf("camera: %s\n", cam.Name().c_str());
    // keep Live View running for a moment (some properties are announced after the first frames)
    std::vector<uint8_t> jpeg;
    cam.SetEventInterval(250);
    uint64_t t0 = NowMs();
    while (NowMs() - t0 < 2500 && !g_stop) {
        if (cam.GetFrame(jpeg) != CanonCamera::Frame::Ok) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    cam.PollEvents();
    for (auto& kv : cam.AllProps()) {
        const PropState& p = kv.second;
        std::string name = PropName(kv.first);
        printf("%04X %-30s", kv.first, name.empty() ? "" : name.c_str());
        if (p.hasValue) printf(" = %s (0x%X)", PropValueText(kv.first, p.value).c_str(), p.value);
        printf("\n");
        if (!p.raw.empty() && p.raw.size() > 4) {
            printf("     raw:");
            for (size_t i = 0; i < p.raw.size() && i < 64; i++) printf(" %02X", p.raw[i]);
            printf("\n");
        }
        if (p.hasList && !p.list.empty()) {
            printf("     allowed:");
            for (uint32_t v : p.list) printf(" %s", PropValueText(kv.first, v).c_str());
            printf("\n");
        }
        if (p.hasValue && ((kv.first >= 0xD150 && kv.first <= 0xD155) || (kv.first >= 0xD160 && kv.first <= 0xD162))) {
            int v[4];
            if (GetStyleParam(p.raw, 0, v[0]) && GetStyleParam(p.raw, 1, v[1]) && GetStyleParam(p.raw, 2, v[2]) &&
                GetStyleParam(p.raw, 3, v[3]))
                printf("     contrast %d, sharpness %d, saturation %d, color tone %d\n", v[0], v[1], v[2], v[3]);
        }
    }
    cam.Disconnect();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    InitLang(SettingsPath());
    InstallSignals();
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "probe") return CmdProbe();
    if (cmd == "run") return CmdRun();
    if (cmd == "get") return CmdGet();
    if (cmd == "set") return CmdSet(argc, argv);
    if (cmd == "cam") return CmdCam();
    if (cmd == "process") return CmdProcess(argc, argv);
    if (cmd == "bench") return CmdBench(argc, argv);
    fputs(Tr(Str::CliUsage), stdout);
    return cmd.empty() || cmd == "help" || cmd == "--help" ? 0 : 2;
}
