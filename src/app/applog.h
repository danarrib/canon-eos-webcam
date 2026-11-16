// Log com horário em stdout (console no "eoscam run"; arquivo ProgramData\EosCam\eoscam.log no eoscam-bg).
#pragma once

namespace eoscam {

// Thread-safe: várias threads (captura, processamento, painel) escrevem no mesmo log.
void Log(const char* fmt, ...);

}  // namespace eoscam
