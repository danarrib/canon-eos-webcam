// Timestamped log lines on stderr (the systemd journal captures them).
#pragma once

namespace canon_eos_webcam {

// printf-style. Thread-safe.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));


}  // namespace canon_eos_webcam
