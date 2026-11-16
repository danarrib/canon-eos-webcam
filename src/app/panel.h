// Tray icon and control panel (canon-eos-webcam-bg.exe only). Native Win32 window, no dependencies.
#pragma once

#include <windows.h>

#include <memory>

#include "bridge.h"
#include "settings.h"

namespace canon_eos_webcam {

// Class of the hidden window that receives the tray clicks and the "canon-eos-webcam panel" messages.
inline constexpr wchar_t kBridgeWindowClass[] = L"CanonEosWebcamBridgeWindow";
// Message that "canon-eos-webcam panel" sends to the hidden window: opens the panel (or brings it to the front).
inline constexpr UINT kMsgOpenPanel = WM_APP + 1;

class PanelApp {
public:
    PanelApp(Bridge& bridge, SettingsStore& store);
    ~PanelApp();
    PanelApp(const PanelApp&) = delete;
    PanelApp& operator=(const PanelApp&) = delete;

    // Creates the hidden window and the tray icon and runs the message loop until exit.
    int Run();
    // From any thread: ends the message loop.
    void RequestQuit();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace canon_eos_webcam
