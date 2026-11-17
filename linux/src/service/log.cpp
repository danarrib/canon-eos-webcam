#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace canon_eos_webcam {

void Log(const char* fmt, ...) {
    static std::mutex mu;
    static const bool journal = getenv("JOURNAL_STREAM") != nullptr;  // journald stamps the lines itself
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(mu);
    if (journal) {
        fprintf(stderr, "%s\n", msg);
    } else {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        tm t;
        localtime_r(&ts.tv_sec, &t);
        fprintf(stderr, "%02d:%02d:%02d.%03d %s\n", t.tm_hour, t.tm_min, t.tm_sec, int(ts.tv_nsec / 1000000), msg);
    }
    fflush(stderr);
}

}  // namespace canon_eos_webcam
