// Simple log in C:\ProgramData\EosCam\source.log (the DLL runs inside a service, without a console; the
// deploy script grants LocalService write access to that folder). Messages are in English.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "shared.h"

namespace eoscam {

inline void Log(const char* fmt, ...) {
    static SRWLOCK lock = SRWLOCK_INIT;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu:%lu] %s\r\n", t.wYear, t.wMonth,
                     t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(),
                     GetCurrentThreadId(), msg);
    OutputDebugStringA(line);

    AcquireSRWLockExclusive(&lock);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\source.log", kDataDir);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        if (size.QuadPart < 8ll * 1024 * 1024) {  // keeps the log from growing without bound
            DWORD w = 0;
            WriteFile(f, line, DWORD(n > 0 ? n : 0), &w, nullptr);
        }
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&lock);
}

}  // namespace eoscam
