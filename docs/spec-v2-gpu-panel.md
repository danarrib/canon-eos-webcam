# canon-eos-webcam v2: GPU processing (NVIDIA Maxine) and control panel

Status: specification approved by the user on 2026-11-16 (implemented). Read it together with `README.md`.

## Goal

1. Move image processing to the bridge (`canon-eos-webcam-bg.exe` / `canon-eos-webcam run`), on the GPU, with the NVIDIA
   Maxine Video Effects, and deliver **1920×1080** to the virtual camera.
2. Create a **control panel** (tray icon + window) with image adjustments, AI effects, virtual
   background and the camera's own settings, applied live and persisted.

## Starting point (v1)

- `src/app/canon_camera.*`: PTP over WPD (`PtpDevice`, `CanonCamera`). Live View via
  `GetViewFinderData` (1056×704 JPEG, 4:2:2, ~29 fps). Turns Live View back on by itself if it stops.
- `src/app/main.cpp`: commands `run/start/stop/autostart/install/uninstall/status/list/test`.
  `CmdRun` is the bridge loop: opens the channel, turns the camera on when `ConsumerActive()`, publishes
  the JPEGs. `canon-eos-webcam-bg.exe` is the same code with `CANON_EOS_WEBCAM_BACKGROUND` (WIN32, log to a file).
- `src/common/shared.h`: channel `Global\CanonEosWebcamFrames1` (the DLL creates it; the app opens it), 3 slots
  with a seqlock, consumer and producer heartbeats. `CANON_EOS_WEBCAM_LOCAL=1` uses `Local\`.
- `src/source/*`: `canon-eos-webcam-source.dll` (Frame Server, LocalService, session 0). Decodes the JPEG with
  WIC, crops 16:9 **anchored at the bottom**, scales to 1280×720 and converts to NV12. Shows warning
  screens (`FrameRenderer::RenderMessage`) when there are no frames.

## v2 architecture

```
Camera ──PTP──> bridge (user session)
                 ├─ capture/processing thread (the current CmdRun loop, extended)
                 │    JPEG → decode (WIC, CPU) → 16:9 crop from the bottom (1056×594)
                 │    → [Denoise] → [Artifact Reduction]            (GPU, Maxine, at the source resolution)
                 │    → [Green Screen: mask]                         (GPU, Maxine, at the source resolution)
                 │    → Super Resolution 2× (2112×1188) or plain upscaling
                 │    → downscale to 1920×1080
                 │    → image adjustments (brightness/contrast/gamma/temperature/tint/saturation)
                 │    → background compositing (solid color / blur / image) using the mask
                 │    → NV12 1920×1080 → shared channel
                 ├─ UI thread (tray + panel), canon-eos-webcam-bg only
                 └─ PTP command queue for the camera settings (run by the capture thread, which is
                    the only owner of the PTP session)
DLL (Frame Server): copies the ready NV12 into the sample. Only decodes/renders the warning screens.
```

### Shared channel (v2 layout)

- New names: `Global\CanonEosWebcamFrames2` / `Global\CanonEosWebcamFrameReady2` (and `Local\…2`). This way an
  installed v1 and v2 never get mixed up. `kLayoutVersion = 2`.
- The slots now hold **NV12 1920×1080** (3,110,400 bytes). Put in the slot header: `format` (NV12
  fourcc), `width`, `height`, `stride`. Maximum slot size: 4 MB.
- The rest of the protocol (seqlock, heartbeats, producer states) does not change.

### DLL

- Offers **only NV12 1920×1080 @ 30 fps** (`kSizes`). Keep the comment explaining why there is a
  single format (programs opened at 4:3 and switched later).
- `RenderJpeg` is no longer used on the normal path. The DLL copies the NV12 from the slot (validating
  the size). Keep `RenderMessage` for the warning screens, now at 1080p.
- If the slot comes in a size different from the negotiated format, show a warning screen
  ("incompatible canon-eos-webcam versions: reinstall"), never crash.

### Pipeline in the bridge

Create a new module, for example `src/app/pipeline.{h,cpp}` (class `FramePipeline`), plus
`src/app/maxine.{h,cpp}` (loading and wrapping of the effects).

**Maxine Video Effects SDK**
- The runtime is installed in `C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects` (version
  0.7.6, `NVVideoEffects.dll` 0.8.1). The models (in `models\`) exist for the user's GPU (sm 89 = RTX
  4080): `SR_2x/3x/4x_{con,agg}`, `AR-{con,agg}`, `Denoise-{con,agg}` and `AIGS_288x512_*`.
- The headers and the "proxies" (which load the DLL dynamically) are in the public
  **NVIDIA/MAXINE-VFX-SDK** repository on GitHub (MIT license): `nvvfx/include/` (`nvVideoEffects.h`,
  `nvCVImage.h`, `nvCVStatus.h`, …) and `nvvfx/src/` (`nvVideoEffectsProxy.cpp`, `nvCVImageProxy.cpp`).
  Use the tag/version closest to 0.7.6. Copy only the needed files to `third_party/maxine/` (with the
  LICENSE) and version them.
- Locate the runtime through the `NV_VIDEO_EFFECTS_PATH` variable or, if it does not exist, through the
  default path above. Set `NVVFX_MODEL_DIRECTORY` to the `models` folder.
- **Do not depend on the CUDA Toolkit or nvcc.** Use only the SDK API: `NvCVImage_Alloc`,
  `NvCVImage_Transfer` (CPU↔GPU and format conversions, u8↔f32, chunky↔planar),
  `NvVFX_CudaStreamCreate` and the effects. Whatever the SDK doesn't do, do it on the CPU.
- Effects and modes (confirm the semantics of `NVVFX_MODE` in the headers and documentation):
  - Denoise: off / con / agg. It is temporal, so keep the state object (`NVVFX_STATE`) and reset it
    when the resolution or the mode changes.
  - Artifact Reduction: off / con / agg.
  - Super Resolution 2×: off / con / agg. With off, the upscaling is plain bilinear/Fant.
  - Green Screen (AIGS): produces the mask at the source resolution (before Super Resolution). The mask
    is upscaled to 1080p during compositing.
- When a mode changes, recreate only the affected effect, as a "hot" swap: the current frame uses the
  old effect until the new one is loaded (`NvVFX_Load` can take seconds). When the webcam is opened,
  preload the enabled effects.
- If the runtime is missing or initialization fails, every AI effect becomes unavailable (tell the
  panel) and the pipeline carries on on the CPU: decode → crop → scale to 1080p → adjustments → NV12.
  Green Screen is disabled in that case.

**Image adjustments** (on the camera image, before compositing with the background)
- Brightness (−100…+100), Contrast (−100…+100), Gamma (0.5…2.0, default 1.0), Temperature (−100…+100,
  warm/cool: R/B gains), Tint (−100…+100, green/magenta: G gain), Saturation (0…200%, default 100%).
- Suggested implementation: a 256-entry LUT per channel, recomputed only when the values change
  (brightness, contrast, gamma, temperature and tint), plus saturation applied during the NV12
  conversion (scaling U/V around 128) or through an RGB matrix. Parallelize in bands with `std::thread`
  or a simple pool, if needed to fit the budget.

**Virtual background** (only with Green Screen on)
- Modes: **solid color** (picked with `ChooseColor`), **blur** (strength 0…100; use the SDK `BgBlur`
  effect if the runtime has it, otherwise a separable blur on the CPU at reduced resolution) and **image
  from disk** (PNG/JPG via WIC; fill 1920×1080 with a center crop, "cover"; reload only when the path
  changes).
- Alpha compositing with the upscaled and smoothed mask (avoid jagged edges).

**Performance budget** (measure and log every 60 s)
- 1920×1080 at ≥ 25 fps with Super Resolution + Artifact Reduction on.
- GPU and CPU time per stage, as a moving average (for the panel and the log).
- Without AI effects: bridge CPU < 25% of a core.

### Camera settings over PTP

Use the libgphoto2 codes and encodings (`camlibs/ptp2/ptp.h`, `ptp.c` and `config.c`) **as a
reference**; it is LGPL, so don't copy code, only values and formats.

- Current values and lists of allowed values: parse the `GetEvent` (0x9116) records:
  `PropValueChanged` (0xC189) and `AvailListChanged` (0xC18A). Keep a cache
  `prop → current value + allowed values` and feed the panel from it.
- Write with `SetDevicePropValueEx` (0x9110), already used in `CanonCamera::SetPropEx`.
- Properties on the panel (only those the camera announces; the others appear disabled):
  - Exposure compensation (`ExpCompensation`, 0xD104; encoded in thirds of EV)
  - ISO (0xD103)
  - White balance (0xD109): auto, daylight, shade, cloudy, tungsten, fluorescent, flash, custom; and
    color temperature in Kelvin (check the code in libgphoto2) when the mode is Kelvin
  - Picture Style (0xD110): Standard, Portrait, Landscape, Neutral, Faithful, Monochrome
  - Parameters of the active Picture Style (contrast, saturation, color tone, sharpness), if the model
    allows it (check how libgphoto2 handles the `PictureStyle*` properties)
  - Auto Lighting Optimizer, if announced
- Panel commands go into a thread-safe queue, run by the capture thread between frames. Failures show
  up on the panel ("the camera refused: automatic mode?").
- **canon-eos-webcam does not persist the camera settings**: the camera itself keeps them.

### Settings

- File `%APPDATA%\CanonEosWebcam\settings.ini` (`GetPrivateProfileString`/`WritePrivateProfileString`), with the
  sections `[image]`, `[effects]`, `[background]` and `[ui]`.
- A `Settings` struct with a version counter, protected by a mutex, that the pipeline reads once per
  frame. On change, write to disk with a debounce (~500 ms).

### Panel (`canon-eos-webcam-bg.exe` only)

- **Tray icon** (`Shell_NotifyIcon`). Menu: "Open panel", "Start with Windows" (checked according to
  `HKCU\…\Run\CanonEosWebcam`), "Exit". Double-click opens the panel.
- **Window** in native Win32 (no new dependencies), with a live preview on the left (~640×360, ~15 fps,
  from the last processed frame in memory, converted to BGRA and drawn with `StretchDIBits`) and
  controls on the right, in groups:
  - **Image:** sliders for brightness, contrast, gamma, temperature, tint and saturation, each with its
    value next to it and double-click to reset; a "Restore defaults" button.
  - **AI effects:** Super Resolution (off/con/agg), Artifact Reduction (off/con/agg), Denoise
    (off/con/agg). Disabled, with the reason, if Maxine is not available.
  - **Background:** Green Screen on/off; mode (solid color / blur / image); a "Color…" button, blur
    strength, an "Image…" button (`IFileOpenDialog`, PNG/JPG) with the file name.
  - **Camera:** exposure compensation, ISO, white balance (with Kelvin when applicable), Picture Style
    and its parameters, ALO.
  - **Status bar:** bridge state (waiting / live / no camera), input/output fps and processing time per
    frame.
- **With the panel open, the bridge turns Live View on** even when no program is using the webcam (the
  panel counts as a consumer), so settings can be adjusted before a call. On close, back to the normal
  rule (off 5 s after there is no consumer).
- New `canon-eos-webcam panel` command: opens the panel of the already running `canon-eos-webcam-bg` (find its hidden window
  and send it a message); if it is not running, start it and then open the panel.
- The capture loop now runs on its own thread in `canon-eos-webcam-bg`; the main thread runs the message queue.
  `canon-eos-webcam run` (console) uses the same pipeline, without tray or panel.

## Out of scope

- Installing v2 in `C:\Program Files` (needs administrator rights; the user/orchestrator does it).
- Camera controls exposed to other programs through `IKsControl`.

## Acceptance criteria

1. Builds without warnings (`/W4`) with `cmake --build build --config Release`.
2. Local test (no admin, see "How to test"): `canon-eos-webcam test --direct --size 1920x1080` delivers ≥ 25 fps
   with Super Resolution + Artifact Reduction on in `settings.ini`, and the saved frame looks good (open
   the JPG and check: crop from the bottom, colors, mask edges).
3. Every effect, mode and background mode tested at least once, with a frame saved as evidence in a
   temporary test folder.
4. Without Maxine (simulate it, for example, with `NV_VIDEO_EFFECTS_PATH` pointing to an empty folder),
   the pipeline falls back to the CPU without crashing.
5. The panel opens and closes, the settings show up in the preview and persist across restarts, and the
   tray works. Take a screenshot of the window (`PrintWindow`) and save it as evidence.
6. Camera settings: the list of values comes from the camera; changing the exposure compensation and the
   white balance changes the image (compare saved frames).
7. README updated.

## How to test without administrator rights

- First of all, stop the installed bridge (it holds the `Local\CanonEosWebcamProducer` mutex):
  `& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" stop`. **When done, start it again:**
  `& "C:\Program Files\CanonEosWebcam\canon-eos-webcam.exe" start` (the user's webcam depends on it).
- Full pipeline without the Frame Server: with `$env:CANON_EOS_WEBCAM_LOCAL='1'`, run
  `build\Release\canon-eos-webcam.exe run` in the background and `build\Release\canon-eos-webcam.exe test --direct`.
- The camera must be on; it is already connected over USB.
- Do **not** run `scripts\deploy.ps1` or anything that asks for UAC.
