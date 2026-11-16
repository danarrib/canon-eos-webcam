# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

canon-eos-webcam turns a Canon EOS (tested: Rebel T3 / 1100D) into a native Windows 11 webcam using only the standard
PTP/WPD driver and `MFCreateVirtualCamera`, with optional NVIDIA Maxine effects on the GPU. Windows only, C++20
(MSVC). README.md is the usage reference (commands, environment variables, effects, measured performance;
README.pt-BR.md is its Portuguese translation); `docs/spec-v2-gpu-panel.md` is the v2 specification and
`docs/linux-briefing.md` the briefing for a future Linux port.

**Language:**
- Source code, identifiers, comments, log messages, diagnostic output (`status`, `test`, `process`, `cam`,
  `eosprobe`), docs and commit messages are in **English**.
- The user interface is in English by default with a **Brazilian Portuguese** translation: control panel, tray
  menu, the virtual camera's warning screens and the everyday console commands (`start`, `stop`, `autostart`,
  `install`, `uninstall`, `panel`, usage). Every UI string lives in `src/common/i18n.h` as
  `X(Id, "English", "Português")`; never hard-code UI text elsewhere. Use `TrW(Str::Id)` for Win32/GDI,
  `Tr(Str::Id)` for UTF-8/printf (keep the same printf specifiers, in the same order, in both languages), and
  `Msg` for texts built far from the UI (pipeline, bridge), rendered with `Text(msg)` on the panel and
  `Text(msg, Lang::En)` in the log.
- Language selection: `CANON_EOS_WEBCAM_LANG` (`en`|`pt`), then `[ui] language` in settings.ini (`auto`|`en`|`pt`, read at
  startup), then the Windows display language. The DLL runs as LocalService, so it follows the system language.
- Keep README.md and README.pt-BR.md in sync when changing either one.

## Building

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Release --target canon-eos-webcam   # a single target
```

Targets: `canon-eos-webcam-source` (DLL), `canon-eos-webcam` (console app), `canon-eos-webcam-bg` (same code, `WIN32` subsystem, with
`CANON_EOS_WEBCAM_BACKGROUND` defined → tray icon + panel), `maxine-proxy` (static library), `eosprobe`
(`tools/eosprobe`). The CRT is static on purpose: the DLL runs inside the Frame Server and must not depend on the
VC++ Redistributable. Builds with `/W4 /permissive-`; keep the build warning-free.

## Testing

There is no unit test framework. Verification is done with the app's own subcommands:

```powershell
$env:CANON_EOS_WEBCAM_LOCAL = '1'                       # channel in Local\ → no admin and no Frame Server
.\build\Release\canon-eos-webcam.exe run                # bridge in the console (stop the installed one first: canon-eos-webcam stop)
.\build\Release\canon-eos-webcam.exe test --direct      # loads the DLL in its own process, measures frames/s and CPU
.\build\Release\canon-eos-webcam.exe process in.jpg --bench 100   # pipeline over a JPEG, no camera
.\build\Release\canon-eos-webcam.exe cam                # diagnostics of the camera settings (with the bridge stopped)
```

Useful environment variables for tests: `CANON_EOS_WEBCAM_SETTINGS` (another settings.ini), `CANON_EOS_WEBCAM_LOG`,
`NV_VIDEO_EFFECTS_PATH` (an empty folder simulates a missing Maxine runtime), `CANON_EOS_WEBCAM_FORCE_CPU_BLUR`,
`CANON_EOS_WEBCAM_THREADS`, `CANON_EOS_WEBCAM_DUMP_EVENTS`, `CANON_EOS_WEBCAM_CUDA_SPIN`, `CANON_EOS_WEBCAM_PANEL_NOACTIVATE=1` (the panel appears
without stealing focus), `CANON_EOS_WEBCAM_LANG=en|pt` (check UI changes in both languages).

**Rule for testing the UI:** the user works on this same machine. Never use synthetic input (SendInput, SendKeys,
clicks/typing via UI Automation), never steal focus and never open modal dialogs. Test the panel with
`CANON_EOS_WEBCAM_PANEL_NOACTIVATE=1`, window messages to our own HWNDs, `PrintWindow` captures (from a DPI-aware process,
or the capture comes out cropped) and settings files. Repeat this rule in the prompt of any subagent that
touches the UI.

Logs: `C:\ProgramData\CanonEosWebcam\canon-eos-webcam.log` (bridge, statistics every 60 s) and `source.log` (DLL).

## Installing (deploy)

`scripts/deploy.ps1` must run as administrator (copies `build\Release` to `C:\Program Files\CanonEosWebcam`, stops the
Frame Server to free the DLL, renames files in use to `*.old-*` and runs `regsvr32`). This shell is not elevated:
run it with `Start-Process pwsh -Verb RunAs -ArgumentList '-ExecutionPolicy','Bypass','-File','scripts\deploy.ps1'`,
warn the user that a UAC prompt will appear and batch DLL changes to ask for elevation only a few times.
Then, without elevation: `canon-eos-webcam install` (creates the virtual camera) and `canon-eos-webcam start` / `autostart on`.

## Architecture

Two processes in different sessions, connected by shared memory:

- **`src/source/` → `canon-eos-webcam-source.dll`**: an `IMFMediaSource` registered in HKLM (CLSID in `shared.h`), loaded
  by the Windows Frame Server as LocalService in session 0. Lean on purpose: it signals that it wants frames,
  copies the most recent ready NV12 and draws the warning screens (`renderer.cpp`, GDI) when the camera, the
  bridge or matching versions are missing. **Never add WIC, Maxine, CUDA or heavy dependencies here.**
- **`src/app/` → `canon-eos-webcam.exe` / `canon-eos-webcam-bg.exe`**: the bridge, in the user session.
  - `canon_camera.*`: PTP via WPD (Canon vendor operations: `SetRemoteMode`, `EVFOutputDevice`,
    `GetViewFinderData`, `GetEvent`, property writes, including the Picture Style `0xD150…` worked out on the
    camera itself).
  - `bridge.*`: two worker threads. The capture thread is the **only owner of the PTP session** (connects,
    fetches the JPEGs, reads the events and runs the commands queued by the panel between frames). The
    processing thread always takes the most recent JPEG and drops the others, so capture keeps going at ~29 fps
    even with slow processing. Live View only turns on with an active consumer or the panel open, turns off 5 s
    later, and is turned back on automatically if something knocks it down.
  - `pipeline.*`: JPEG → WIC decode → 16:9 crop anchored at the bottom (1056×594) → optional Maxine Denoise /
    Artifact Reduction / Green Screen mask / Super Resolution 2× (`maxine.*`) → CPU scaling + LUT adjustments +
    background compositing + NV12 1920×1080 (`imaging.*`, AVX2 with a plain C++ fallback). Changing an effect's
    mode recreates only that effect, "hot", without stopping the frames. Without the Maxine runtime or on a GPU
    failure, the effects become `Unavailable`/`Failed` and the pipeline carries on on the CPU only.
  - `settings.*`: `SettingsStore` with a version counter; consumers compare the version from `Get(&version)` to
    detect changes, a thread writes `%APPDATA%\CanonEosWebcam\settings.ini` with a delay (debounce) and external edits to
    the file are reloaded.
  - `panel.*`: tray icon + Win32 control panel (`canon-eos-webcam-bg` only). `main.cpp` dispatches the subcommands.
- **`src/common/shared.h`**: the frame channel. Mapping `Global\CanonEosWebcamFrames2` with a 3-slot ring (seqlock) of
  ready NV12 1920×1080 frames, plus the producer/consumer heartbeats and the `ProducerState`. **The DLL creates**
  the mapping (users don't have `SeCreateGlobalPrivilege`); the app only opens it and keeps the handle open. The
  names end in `2` so an installed v1 never mixes with v2. Changing the layout requires bumping `kLayoutVersion`
  and treating the installed DLL + bridge as a matched pair (otherwise the DLL shows "incompatible versions").
- **`src/common/i18n.h`**: the UI strings (English + Brazilian Portuguese) and the language selection, shared by
  the DLL and the app.
- **`third_party/maxine`**: headers and proxies (MIT) that load `NVVideoEffects.dll` / `NVCVImage.dll`
  dynamically from the installed runtime. Nothing is linked against the CUDA Toolkit; keep it that way.

## Constraints

- Keep Canon's EOS Webcam Utility / EDSDK out of the way; its UMDF driver knocks down our Live View when it is
  active (the user keeps that device disabled). The point of the project is not to depend on unusual drivers.
- The virtual camera offers a single format (NV12 1920×1080 at 30 fps); with several, some programs first opened
  at 4:3 and switched later.
- The camera property values live in the camera itself, not in settings.ini; the available values come from the
  camera events, and only the properties the current mode announces are enabled.
- The project was renamed from eoscam to canon-eos-webcam. The only intentional references to the old name are
  the upgrade paths: `kLegacyCameraName` (main.cpp), `kLegacyRunValue` (autostart.h), the settings copy in
  `SettingsStore::DefaultPath` and the cleanup in `scripts/deploy.ps1`.
