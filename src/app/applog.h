// Timestamped log on stdout (the console in "canon-eos-webcam run"; ProgramData\CanonEosWebcam\canon-eos-webcam.log in canon-eos-webcam-bg).
// Log messages are always in English.
#pragma once

namespace canon_eos_webcam {

// Thread-safe: several threads (capture, processing, panel) write to the same log.
void Log(const char* fmt, ...);

}  // namespace canon_eos_webcam
