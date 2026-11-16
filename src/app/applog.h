// Timestamped log on stdout (the console in "eoscam run"; ProgramData\EosCam\eoscam.log in eoscam-bg).
// Log messages are always in English.
#pragma once

namespace eoscam {

// Thread-safe: several threads (capture, processing, panel) write to the same log.
void Log(const char* fmt, ...);

}  // namespace eoscam
