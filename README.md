# eoscam: Canon EOS as a native Windows 11 webcam

**English** · [Português (Brasil)](README.pt-BR.md)

Uses an old Canon EOS (tested with the **EOS Rebel T3 / 1100D**) as a webcam, without Canon's drivers:
only the standard Windows PTP/WPD driver and the Windows 11 virtual camera API
(`MFCreateVirtualCamera`). Version 2 processes the image on the GPU with **NVIDIA Maxine** and adds a
control panel with a tray icon.

| | EOS Webcam Utility | eoscam v2 |
|---|---|---|
| Resolution | ~480×360 | **1920×1080** (16:9 crop of the 1056×704 Live View, with optional Super Resolution) |
| Frames/s | — | ~28 (the T3 Live View delivers ~29) |
| Bridge CPU, without AI effects | high | ~19% of a core |
| Effects | — | Super Resolution, artifact reduction, noise reduction, background removal/replacement, image adjustments |
| Camera settings | — | exposure, ISO, white balance, Picture Style, ALO (from the panel, over PTP) |
| Live View | always on | on only while some program uses the webcam (or the panel is open) |

## How it works

```
Canon T3 ──USB/PTP (WPD driver)──> eoscam-bg.exe / eoscam run          (user session)
                                    capture thread: PTP, Live View, camera settings
                                       │ 1056×704 JPEG (always the most recent one)
                                    processing thread:
                                       JPEG → WIC → 16:9 crop from the bottom (1056×594)
                                       → [Denoise] → [Artifact Reduction]       GPU (Maxine)
                                       → [Green Screen: mask]                   GPU (Maxine)
                                       → [Super Resolution 2×]                  GPU (Maxine)
                                       → adjustments + background + NV12 1920×1080   CPU
                                       │
                                  shared memory Global\EosCamFrames2
                                       │  ready NV12 1920×1080 frames
                                       ▼
                         eoscam-source.dll  (loaded by the Frame Server, LocalService)
                         only copies the frame; draws the warning screens
                                       │
                                       ▼
                 virtual camera "Canon EOS Webcam (eoscam)"  →  Teams, Zoom, browser, OBS...
```

- **`eoscam-source.dll`**: media source (IMFMediaSource) registered in HKLM. The Frame Server loads it
  when a program opens the webcam. It signals on the shared channel that it wants frames; without
  frames, it shows a screen explaining what is missing. It is lean on purpose: it decodes nothing and
  **does not load Maxine or CUDA** inside the Frame Server. If the bridge is from another version (frame
  of a different size), it shows "Incompatible eoscam versions: reinstall". The v2 channel
  (`EosCamFrames2`) never mixes with the v1 one.
- **`eoscam.exe` / `eoscam-bg.exe`**: the bridge to the camera and the image pipeline. Turns Live View
  on (`SetRemoteMode`, `EVFOutputDevice = PC`) when the source asks for frames (or the panel is open),
  fetches frames with `GetViewFinderData` and turns Live View off 5 s after nobody uses the webcam
  anymore. If other software knocks Live View down, it turns it back on automatically.
  `eoscam-bg.exe` is the same code without a console, with a **tray icon and panel**.
- **`tools/eosprobe`**: a tool that measures the Live View over PTP.

Offered format: only NV12 1920×1080 at 30 fps, 16:9 crop anchored at the bottom (drops the top). With
several options, some programs first opened at 640×480 (4:3) and switched to 16:9 a second later.

## AI effects (NVIDIA Maxine)

They need the **NVIDIA Video Effects runtime** (Maxine VFX SDK installer, "End-user Redistributables")
in `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects` (or in the folder given by the
`NV_VIDEO_EFFECTS_PATH` variable) and an RTX GPU; the models are specific to each architecture
(`SR_2x_*`, `AR-*`, `Denoise-*`, `AIGS_*` in `models\`). Nothing is compiled against the CUDA Toolkit:
the project only uses the SDK API (`NvVFX_*`, `NvCVImage_*`), loaded dynamically (`third_party/maxine`,
MIT license).

| Effect | Modes | Notes |
|---|---|---|
| Super Resolution 2× | off / conservative / aggressive | 1056×594 → 2112×1188, then downscaled to 1920×1080. Off: bilinear upscaling |
| Artifact reduction | off / conservative / aggressive | at the source resolution |
| Noise reduction | off / conservative / aggressive | temporal (keeps state); the SDK only accepts widths and heights that are multiples of 8, so it runs on the bottom 592 rows |
| Remove the background (Green Screen) | on/off | mask at the source resolution, smoothed and upscaled during compositing |
| Background | solid color, blur (0–100), PNG/JPG image | the blur uses the SDK `BackgroundBlur` with the mask; the image fills 1920×1080 ("cover") |

Changing a mode recreates only the affected effect, "hot": frames keep using the old model while the
new one loads (hundreds of ms). When the webcam is opened, the enabled effects are preloaded. Without
the runtime (or if the GPU fails) the effects become unavailable (the panel shows why) and the pipeline
carries on on the CPU only: decode, crop, scale, adjust, convert.

**Image adjustments** (on the camera image, before the background): brightness, contrast, gamma,
temperature (R/B gains), tint (green/magenta) and saturation, applied through LUTs.

## Control panel

Tray icon (menu: **Open panel**, **Start with Windows**, **Exit**; double-click opens the panel) or
`eoscam panel`. With the panel open the bridge turns Live View on even when no program is using the
webcam, so you can adjust things before a call; closing the window (the bridge stays in the tray) goes
back to the normal rule. It has a live preview, **Image** (double-clicking a slider resets it), **AI
effects**, **Background** and **Camera** groups, and a status bar with the bridge state, input and
output frames/s and the processing time per frame. The settings live in
`%APPDATA%\EosCam\settings.ini` (sections `[image]`, `[effects]`, `[background]`, `[ui]`) and are
written ~0.5 s after each change.

**Language.** The user interface (panel, tray menu, warning screens of the virtual camera and the
everyday commands) is available in English and Brazilian Portuguese. The language comes, in this order,
from the `EOSCAM_LANG` variable (`en` or `pt`), from the `language` key in the `[ui]` section of
`settings.ini` (`auto`, `en` or `pt`; read when the bridge starts) and from the Windows display language
(Portuguese → `pt`, anything else → `en`). The warning screens are drawn by the DLL inside the Frame
Server (LocalService account), so they follow the system language. Logs and diagnostic output are
always in English.

**Camera (PTP; the values are stored by the camera itself, not by eoscam).** The list of values comes
from the camera (`GetEvent`: `PropValueChanged` and `AvailListChanged`); only the settings the current
camera mode announces are enabled. On the T3 in automatic mode: exposure compensation, white balance
(auto, daylight, shade, cloudy, tungsten, fluorescent, flash, custom), Picture Style and its parameters
(contrast, sharpness, saturation, color tone) work; ISO only shows up in the P/Tv/Av/M modes, and the
Auto Lighting Optimizer may be refused depending on the mode ("the camera refused"). The color
temperature in Kelvin is only offered if the camera lists the color temperature mode (the T3 doesn't).
The Picture Style parameters are not handled by libgphoto2: the structure (`0xD150`…`0xD155`, user
styles at `0xD160`…) was worked out by reading and writing on the camera itself.

## Building

Requires Visual Studio 2022 (C++), CMake and the Windows SDK 10.0.22000+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Installing

```powershell
# 1. As administrator: copies to C:\Program Files\EosCam and registers the DLL
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1

# 2. As a regular user:
& "C:\Program Files\EosCam\eoscam.exe" install          # creates the virtual camera
& "C:\Program Files\EosCam\eoscam.exe" autostart on     # background bridge, starts with Windows
& "C:\Program Files\EosCam\eoscam.exe" panel            # opens the control panel
```

The camera must be on, in a photo mode (P/Av/Tv/M, for more settings) and with Live View enabled in the
menu. If the webcam has not been opened by any program yet, the `Global\` channel only comes into
existence when the Frame Server loads the DLL; the panel works anyway.

## Commands

```
eoscam run                  bridge in this console (to watch the log live; no tray or panel)
eoscam start | stop         background bridge (eoscam-bg.exe, with tray and panel)
eoscam panel                opens the panel (starts eoscam-bg if it is not running)
eoscam autostart on|off     start the background bridge together with Windows
eoscam install | uninstall  create/remove the virtual camera
eoscam status               channel state (frames, producer, consumer)
eoscam list                 cameras Windows can see
eoscam test [--seconds N] [--size WxH] [--out f.jpg] [--direct]
                            opens the webcam like a regular app and measures frames/s and CPU
eoscam process in.jpg [--out f.jpg] [--bench N]
                            runs a JPEG through the pipeline (no camera): tests effects and measures timings
eoscam cam [--set PROP=VALUE]... [--out f.jpg]
                            diagnostics (with the bridge stopped): lists the settings the camera announces,
                            writes properties (hexadecimal) and saves a processed frame after each one
```

Logs: `C:\ProgramData\EosCam\eoscam.log` (bridge; every 60 s it records frames/s, CPU and the time per
stage) and `source.log` (source, inside the Frame Server).

## Performance (RTX 4080, T3 Live View at ~29 fps)

Measured with `eoscam test --direct --size 1920x1080` and the real camera; times per frame as a moving
average.

| Configuration | Frames/s delivered | Bridge CPU | Per stage (ms) |
|---|---|---|---|
| No AI effects | 26–28 (camera limit) | ~19% of a core | decode 2.6 + scale/adjust/NV12 3.5 (CPU) |
| Super Resolution + Artifact Reduction | 28 | ~80% | GPU: SR 6.0, AR 1.8, upload 0.2, download 0.9; CPU: 2.7 + 4.1 |
| Everything on (SR, AR, Denoise, blurred background) | 27.6 | ~165% | GPU: SR 5.7, AR 1.5, DN 1.7, mask+blur 3.3, upload/download 1.3; CPU: 3.2 + 12.4 |

Without effects the bridge fits in less than 25% of a core (AVX2 code with a plain C++ fallback). With
AI on, CPU goes up because the SDK waits for each GPU stage synchronously (the GPU measures 2–6 ms per
stage on the first measurement and gets slower when idle).

## Development

`eoscam test --direct` loads the DLL inside its own process, without registration or the Frame Server.
With `EOSCAM_LOCAL=1` in the environment (for both `run` and `test`), the channel uses the `Local\`
namespace, so everything can be tested without administrator rights:

```powershell
$env:EOSCAM_LOCAL = '1'
.\build\Release\eoscam.exe run             # in one terminal (stop the installed bridge first: eoscam stop)
.\build\Release\eoscam.exe test --direct   # in another
```

Useful environment variables: `EOSCAM_SETTINGS` (another `settings.ini`), `EOSCAM_LOG` (eoscam-bg log),
`NV_VIDEO_EFFECTS_PATH` (another Maxine runtime folder; an empty folder simulates its absence),
`EOSCAM_FORCE_CPU_BLUR=1` (uses the CPU background blur), `EOSCAM_THREADS` (image pool threads),
`EOSCAM_DUMP_EVENTS=1` (logs the camera property events), `EOSCAM_LANG=en|pt` (user-interface
language).

The user-interface strings live in `src/common/i18n.h`, each with its English and Portuguese versions
side by side; to add a string, add an `X(Id, "English", "Português")` line to the list.

To update the installed DLL, run `scripts\deploy.ps1` again (as admin): it stops the Frame Server to
free the file.

## Uninstalling

```powershell
& "C:\Program Files\EosCam\eoscam.exe" autostart off
& "C:\Program Files\EosCam\eoscam.exe" stop
& "C:\Program Files\EosCam\eoscam.exe" uninstall
pwsh -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Uninstall   # as admin
```

## Conflict with EOS Webcam Utility

The EOS Webcam Utility driver (UMDF, uses the EDSDK) talks to the camera whenever the Windows Frame
Server becomes active and knocks down eoscam's Live View. eoscam recovers on its own in less than 1 s,
but ideally disable the "EOS Webcam Utility" device in Device Manager or uninstall EOS Webcam Utility.

## Licenses

The eoscam code is distributed under the MIT license (see `LICENSE`). `third_party/maxine` contains
headers and proxies from the NVIDIA Maxine Video Effects SDK (MIT, `third_party/maxine/LICENSE`); the
Maxine runtime and models are installed separately, by NVIDIA's installer, under their own license.

eoscam is an independent project, not affiliated with, or endorsed by, Canon or NVIDIA. Canon, EOS,
NVIDIA and Maxine are trademarks of their respective owners and appear here only to indicate
compatibility.
