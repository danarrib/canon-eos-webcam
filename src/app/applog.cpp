#include "applog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace eoscam {

void Log(const char* fmt, ...) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

}  // namespace eoscam
