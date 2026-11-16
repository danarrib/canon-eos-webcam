# Briefing: canon-eos-webcam for Linux (Omarchy)

## Goal

Make the Canon EOS Rebel T3 / 1100D show up as a webcam in **Google Chrome / Google Meet** on Omarchy (Arch +
Hyprland), with a single program of our own that:

- talks **directly to the camera over the PTP protocol** (Canon vendor commands), through **libusb**;
- decodes the Live View JPEG, crops it to 16:9 and delivers **1280×720 at ~30 fps**;
- publishes the image as a **PipeWire virtual camera** (a `Video/Source` node), with no kernel module;
- has a **settings panel** with a live preview, like the Windows version. It handles two things: the **image
  adjustments** done by canon-eos-webcam (brightness, contrast, gamma, temperature, tint and saturation) and the **camera
  settings**, written to the Canon itself over PTP (exposure compensation, ISO, white balance, color
  temperature, Picture Style with contrast/sharpness/saturation/color tone, and Auto Lighting Optimizer).

What it replaces: the current `gphoto2 --capture-movie | ffmpeg | v4l2loopback` chain, which breaks on every
update (v4l2loopback DKMS module, gvfs grabbing the camera, three processes joined by pipes with no recovery).

**Not allowed:** gphoto2/libgphoto2, ffmpeg, v4l2loopback, akvcam, DKMS or any kernel module. The only allowed
dependencies are user-space libraries with a stable ABI from the official Arch repositories: `libusb` (1.0),
`libjpeg-turbo` (TurboJPEG API) and `libpipewire` (0.3) in the service, plus **`gtk4`** in the panel only (the
service must not depend on GTK). The compiler and CMake are build dependencies.

## Context: the Windows version

There is already a working Windows 11 version in this same repository (`canon-eos-webcam`). It uses the standard Windows
PTP/WPD driver and `MFCreateVirtualCamera`. The reference files are:

- `src/app/canon_camera.cpp` / `.h`: the Canon PTP commands and the Live View/recovery logic. **This is the main
  reference.** There the transport is WPD; here it becomes USB bulk through libusb, but the command sequence,
  the parameters and the parsing are the same.
- `src/app/bridge.cpp`: the capture and processing threads, on-demand Live View and reconnection.
- `src/app/pipeline.cpp` / `imaging.cpp`: the 16:9 crop anchored at the bottom, the image adjustments
  (`BuildAdjust` and `AdjustPixel`), scaling and the conversion to YUV.
- `src/app/panel.cpp`: the panel (controls, names of the camera values, Picture Style). `src/app/settings.*`:
  writing the settings with a debounce and reloading them when the file is edited from the outside.
- `src/common/i18n.h`: the user-interface strings in English and Brazilian Portuguese (reuse the same texts).
- `README.md` and `CLAUDE.md`: overview and conventions.

Don't change any of the Windows code. The Linux code lives in its own folder (`linux/`), with its own
`CMakeLists.txt`, not depending on the root CMake.

## Conventions

- C++20 (GCC or Clang), building without warnings with `-Wall -Wextra`.
- **Language:** source code, comments, log messages, diagnostic output and commit messages are in **English**,
  like the rest of the project. The user interface (panel, `.desktop` entry, error messages shown to the user) is
  in English by default, with a Brazilian Portuguese translation picked from `CANON_EOS_WEBCAM_LANG` (`en`|`pt`), then
  `language` in the `[ui]` section of `settings.ini` (`auto`|`en`|`pt`), then the locale (`LANG`/`LC_MESSAGES`
  starting with `pt` → Portuguese). Keep the strings in a table like `src/common/i18n.h`, with both languages
  side by side. `linux/README.md` in English, plus `linux/README.pt-BR.md`.
- **User interface:** the user works on this same machine. Never use synthetic input (ydotool, wtype, xdotool
  and the like), never steal focus and never open windows or dialogs unless asked. To test, use the program
  itself, logs, `pw-dump`/`pw-cli` and command-line tools. Repeat this rule in the prompt of any subagent.
- Make small commits, one per completed and tested step.

## Architecture

Two programs:

- **`canon-eos-webcam`**: the service, running as a **systemd user service** (`systemctl --user`). It does all the work and
  is the only one that talks to the camera.
- **`canon-eos-webcam-panel`**: the settings panel (GTK4), opened by the user when they want. It doesn't talk to the camera
  or process images: it talks to the service over a Unix socket (see "Control channel"). Closing the panel, or it
  hanging, doesn't affect the webcam.

The service has four threads, in the same spirit as the Windows version:

1. **Capture**: the **only owner of the USB/PTP session**. Connects, turns Live View on, fetches the JPEGs in a
   loop, reads the camera events every now and then and reconnects by itself if something goes wrong. Each new
   JPEG goes into a single-item "mailbox" (overwriting the previous one). **Between frames**, it runs the
   property-write commands queued by the panel. No other thread touches the camera.
2. **Processing**: always takes the **most recent** JPEG and drops the others. This way capture keeps going at
   ~30 fps even if processing falls behind. Does decode → crop → scale → YUV and hands the frame to the output.
3. **PipeWire**: the `pw_thread_loop` with the output stream.
4. **Control**: serves the panel socket.

### On-demand Live View

As on Windows, Live View **stays on only while someone is consuming the camera**. Consumers are Chrome (through
PipeWire) **or the open panel** (so the preview and the camera settings work even without a call in progress):

- when the PipeWire stream goes to `PW_STREAM_STATE_STREAMING` (a consumer, such as Chrome, started), capture
  connects and turns Live View on;
- when it leaves `STREAMING` (the consumer closed), wait **5 s** and then turn Live View off and end the PTP
  session. This avoids toggling when Meet switches cameras or reloads.

Without a consumer, the process sits idle, with CPU ≈ 0.

While the camera is not available (off, disconnected or the first frame hasn't arrived yet), send a placeholder
frame, for example plain dark gray. This way Chrome doesn't show a "frozen" black screen and the camera doesn't
vanish from the list.

## Protocol: PTP over USB (libusb)

### Finding and opening the camera

- Canon vendor: `idVendor = 0x04a9`. Look for the interface of class **6** (Still Image), subclass 1, protocol 1.
  It has a bulk IN endpoint, a bulk OUT one and an interrupt IN one (the last one is not needed: Canon delivers
  the events through the `GetEvent` command).
- Call `libusb_set_auto_detach_kernel_driver(h, 1)` and then `libusb_claim_interface`. If it returns
  `LIBUSB_ERROR_BUSY`, another process has the camera. It is usually `gvfsd-gphoto2` or a leftover gphoto2; the
  log must say so in plain words, not just repeat the error code.
- Use the libusb hotplug API (if `libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)`) or look for the camera every
  2 s while there is a consumer and it is not connected.

### PTP container format (little-endian)

```
u32 total size (12-byte header + payload)
u16 type: 1 = command, 2 = data, 3 = response, 4 = event
u16 code (operation or response)
u32 transaction id (incremented on every operation; the same in command, data and response)
... u32 parameters (up to 5) or data
```

Flow of an operation:

1. send the **command** container through bulk OUT;
2. if there is data **for the camera** (e.g. `SetDevicePropValueEx`): send a **data** container (type 2 header
   with the same code + payload) through bulk OUT;
3. if there is data **coming from the camera** (e.g. `GetViewFinderData`, `GetEvent`): read it through bulk IN.
   The first packet carries the header with the total size; keep reading until it is complete. If the size is a
   multiple of `wMaxPacketSize`, the camera sends a zero-length packet at the end, which must also be consumed;
4. read the **response** container (type 3) through bulk IN. Code `0x2001` means OK.

Use large read buffers (≥ 512 KiB; a Live View frame is ~60–150 KiB) and short timeouts (~2 s). On a timeout or
I/O error, call `libusb_clear_halt` on the endpoints. If that doesn't fix it, use the **Device Reset** class
request (`bmRequestType 0x21`, `bRequest 0x66`) or `libusb_reset_device`, and reconnect from scratch.

### Canon sequence (copied from the Windows version, already tested on this camera)

Codes:

| Name | Code |
|---|---|
| `OpenSession` | `0x1002` (parameter: session id, e.g. 1) |
| `CloseSession` | `0x1003` |
| `SetDevicePropValueEx` (Canon) | `0x9110` (no parameters; data: `[u32 12][u32 prop][u32 value]`) |
| `SetRemoteMode` (Canon) | `0x9114` (parameter: 1 on, 0 off) |
| `SetEventMode` (Canon) | `0x9115` (parameter: 1 on, 0 off) |
| `GetEvent` (Canon) | `0x9116` (no parameters; data from the camera) |
| `GetViewFinderData` (Canon) | `0x9153` (parameters: `0x00100000, 0, 0`; data from the camera) |
| `EVFOutputDevice` property | `0xD1B0` (2 = PC, 0 = off) |
| `EVFMode` property | `0xD1B3` (1) |
| OK response | `0x2001` |
| `SessionAlreadyOpen` response | `0x201E` (treat as success) |
| `DeviceBusy` response | `0x2019` (frame not ready yet: try again) |
| Canon "not ready" response | `0xA102` (same) |

**Turning Live View on** (must be idempotent; the same function turns it back on):

```
OpenSession(1)                      // only when connecting
SetRemoteMode(1)                    // if it fails, the connection failed
SetEventMode(1)
GetEvent                            // the first one dumps the complete state; may be discarded or parsed
SetDevicePropValueEx(EVFMode = 1)
SetDevicePropValueEx(EVFOutputDevice = 2)   // if it fails, the connection failed
```

**Turning off** (when disconnecting):

```
SetDevicePropValueEx(EVFOutputDevice = 0)
SetEventMode(0)
SetRemoteMode(0)
CloseSession
```

Also turn off on SIGTERM/SIGINT, so the camera isn't left stuck in remote mode.

**Capture loop:**

- `GetViewFinderData(0x00100000, 0, 0)`. If the response is `0xA102` or `0x2019`, wait ~3 ms and try again; this
  doesn't count as an error. More than 10 errors of another kind in a row: disconnect and reconnect from scratch.
- The data comes in blocks `[u32 size][u32 type][data]`. The JPEG is the **type 1** block whose data starts with
  `FF D8`. Ignore the other blocks.
- Every ~1 s, call `GetEvent` to drain the camera event queue. If it fills up, the camera stops responding. The
  buffer is a sequence of records `[u32 size][u32 type]...` terminated by a record of size 8 and type 0. For now
  nothing needs to be parsed.
- **Recovery:** starting 2.5 s after connecting (the first frame takes ~1.2 s), if more than 800 ms pass without
  a good frame, run the "turn Live View on" sequence again (at most once a second). After 3 restarts without any
  good frame, disconnect and reconnect from scratch.

The T3 Live View frame is a **1056×704** JPEG at ~30 fps.

## Image processing

1. Decode with TurboJPEG (`tj3Decompress8` or equivalent, to RGB, or straight to planar YUV with
   `tj3DecompressToYUVPlanes8`, whichever is simpler and cheaper).
2. **16:9 crop anchored at the bottom:** the full width (1056) and the bottom **594** rows, dropping the top 110
   (usually wall/ceiling). The crop position (bottom/center/top) must be a configuration option.
3. **Image adjustments** (see their own section), applied at the crop resolution, which is smaller than the
   output and therefore cheaper. Skip the step when every adjustment is at its default.
4. Scale to **1280×720** (bilinear is enough) and convert to the PipeWire format.
5. If decoding a frame fails, drop it without bringing anything down.

At 720p this is light for the CPU. Measure and log the average frames/s and the time per frame every 60 s, like
the Windows version does. Keep the pipeline organized in stages (decode → crop → [effects] → scale → convert),
with a clear spot to insert effects later (see "Future: Maxine").

## PipeWire virtual camera

- `pw_stream` with `PW_DIRECTION_OUTPUT` and the properties:
  - `media.class = "Video/Source"`
  - `media.role = "Camera"` (the xdg-desktop-portal camera portal, used by Chrome, filters on it)
  - `node.name = "canon-eos-webcam"`, `node.description = "Canon EOS Webcam"`
- Fixed format: **1280×720, 30/1 fps**. Start with `SPA_VIDEO_FORMAT_YUY2`; if Chrome doesn't accept it, also
  offer `I420`/`NV12`. Test which one Chrome picks and keep only what is needed. On Windows, offering a single
  format avoided headaches.
- Frame pacing: the stream can be the graph *driver* (`PW_STREAM_FLAG_DRIVER` + `pw_stream_trigger_process` on a
  30 Hz timer), always delivering the last processed frame (repeated, if no new one arrived). Use the approach
  that works reliably with Chrome and document the choice.
- Use the stream's `state_changed` for the on-demand Live View (see "Architecture").

**In Chrome:** camera support through PipeWire may need the `chrome://flags/#enable-webrtc-pipewire-camera` flag
("PipeWire Camera support") enabled. Check it on the installed version and document it in the README. Also confirm
that `xdg-desktop-portal` is running and that "Canon EOS Webcam" shows up in the camera list when Meet is
opened. Ask the user to do this final test in the browser, without automating clicks.

## Image adjustments (done by canon-eos-webcam)

The same six as the Windows version, with the same ranges and formulas, so a `settings.ini` works on both
systems:

| Adjustment | Range | Default |
|---|---|---|
| Brightness | −100..+100 | 0 |
| Contrast | −100..+100 | 0 |
| Gamma | 0.50..2.00 | 1.00 |
| Temperature (negative = cool/bluish, positive = warm) | −100..+100 | 0 |
| Tint (negative = green, positive = magenta) | −100..+100 | 0 |
| Saturation | 0..200 % | 100 % |

Implementation (copied from `BuildAdjust`/`AdjustPixel` in `src/app/imaging.cpp`): a 256-entry LUT per channel,
recomputed only when the adjustments change, plus saturation in fixed point.

```
tg = temperature/100 × 0.25        mg = tint/100 × 0.25
gain R = 1 + tg     gain G = 1 − mg     gain B = 1 − tg
c  = contrast × 2.55
cf = 259 × (c + 255) / (255 × (259 − c))
br = brightness/100 × 0.4
for each channel and i in 0..255:
    x = min(1, i/255 × channel gain)
    x = x ^ (1/gamma)
    x = (x − 0.5) × cf + 0.5 + br
    lut[i] = round(clamp(x, 0, 1) × 255)

per pixel: r,g,b = lut_r[r], lut_g[g], lut_b[b]
if saturation ≠ 100 %:
    s = round(saturation × 128 / 100)
    gray = (54 r + 183 g + 19 b) >> 8
    each channel = clamp(gray + ((channel − gray) × s >> 7))
```

The values live in `~/.config/canon-eos-webcam/settings.ini`, section `[image]`, with the keys `brightness`, `contrast`,
`gamma`, `temperature`, `tint` and `saturation` (the same as on Windows). The service writes ~0.5 s after the last
change (debounce), and a hand edit to the file is reloaded automatically (inotify). This way everything can be
tested without the panel. Missing or invalid values fall back to the default.

## Camera settings (PTP)

These do **not** go to `settings.ini`: they are stored in the camera itself. The possible values come from the
camera, and **only the settings the current mode (the camera's mode dial) announces are enabled**.

### Reading: `GetEvent` events

The `GetEvent` (`0x9116`) buffer is a sequence of records `[u32 size][u32 type][data]`, terminated by a record of
size 8 and type 0. Two types matter (offsets counted from the start of the record):

- **`0xC189` PropValueChanged**: `[8] u32 property code`, `[12] u32 value` (or a structure, for the Picture
  Style). Store the value and also the raw bytes from `[12]` to the end of the record.
- **`0xC18A` AvailListChanged**: `[8] u32 code`, `[12] u32 data type`, `[16] u32 count`, `[20] values`, 4 bytes
  each. This is the list of values allowed in the current mode.

Keep a cache `code → {value, list, raw bytes}` with a generation counter that goes up on every change. The first
`GetEvent` after turning on event mode brings the complete state. With the panel open, read the events every
**250 ms** (instead of 1 s), so the panel quickly reflects what changes on the camera, including through its own
buttons.

`CANON_EOS_WEBCAM_DUMP_EVENTS=1` must print every `0xC189`/`0xC18A` event in hexadecimal, for diagnostics.

### Writing

`SetDevicePropValueEx` (`0x9110`), data `[u32 12][u32 property][u32 value]`. For the Picture Style, data
`[u32 total size][u32 property][u32 words...]`. Response `0x2001` means accepted. Anything else means the camera
refused (normal in some modes), and the panel must show "the camera refused" instead of a code. After an
accepted write, update the cache optimistically; the next `PropValueChanged` confirms or corrects it.

### Properties and encodings (checked on the T3)

| Setting | Property | Values |
|---|---|---|
| Exposure compensation | `0xD104` | signed byte in 1/8 EV; the camera uses ±3 and ±5 for 1/3 and 2/3. Show as "−1 2/3 EV", "0", "+1/3 EV" (round `n×3/8` to thirds) |
| ISO | `0xD103` | `0x00` and `0x01` = Auto, `0x48` = 100, `0x50` = 200, `0x58` = 400, `0x60` = 800, `0x68` = 1600, `0x70` = 3200, `0x78` = 6400 (full table in `panel.cpp`, `kIsoNames`). Only shows up in the P/Tv/Av/M modes; in the others, show "(auto)" |
| White balance | `0xD109` | 0 Auto, 1 Daylight, 8 Shade, 2 Cloudy, 3 Tungsten, 4 Fluorescent, 5 Flash, 6 Custom, 9 Color temperature (K), 10–12 Custom PC-1..3 |
| Color temperature | `0xD10A` | Kelvin, 2500..10000. Enabled only if the white balance list has the value 9 **and** it is selected (the T3 doesn't list 9, so it appears disabled) |
| Picture Style | `0xD110` | use only the low byte: `0x81` Standard, `0x82` Portrait, `0x83` Landscape, `0x84` Neutral, `0x85` Faithful, `0x86` Monochrome, `0x87` Auto, `0x88` Fine Detail, `0x21`–`0x23` User Def. 1–3 |
| Auto Lighting Optimizer | `0xD1C1` | 0 Standard, 1 Low, 2 Strong, 3 Disabled. Show only if the camera reports a value < `0x100`; may be refused depending on the mode |

Unknown names appear in hexadecimal (`0x4B`), without hiding the value.

### Picture Style parameters (worked out on the camera itself; libgphoto2 doesn't handle them)

Each style has its own parameter property: `0x81`→`0xD150`, `0x82`→`0xD151`, `0x83`→`0xD152`, `0x84`→`0xD153`,
`0x85`→`0xD154`, `0x86`→`0xD155`, `0x21`→`0xD160`, `0x22`→`0xD161`, `0x23`→`0xD162`.

The value (the raw bytes of the `PropValueChanged`, from offset 12) is a list of `u32` words. It has **28 bytes**
for the factory styles, with the parameters starting at word **1**, and **32 bytes** for the user styles (there is
one extra word with the base style), with the parameters starting at word **2**. From there, the words are, in
this order and as signed integers:

| Parameter | Range |
|---|---|
| Contrast | −4..+4 |
| Sharpness | 0..7 |
| Saturation | −4..+4 |
| Color tone | −4..+4 |

To change a parameter: take the parameter property of the **current** style, copy **all** the words from the
cache, replace only the word of the parameter and write the whole list back with `SetDevicePropValueEx`. If the
current style has no known parameters or the cache has fewer than 28 bytes, leave the four controls disabled.

## Control channel (service ↔ panel)

Unix socket at `$XDG_RUNTIME_DIR/canon-eos-webcam.sock`, accessible only by the user. Use a simple text protocol, one
message per line (e.g. JSON lines), so it can be tested with `socat`. It must cover:

- **state**: camera connected or not, its name, state (idle / starting / live / no camera), input and output
  frames/s, whether Chrome is consuming;
- **image adjustments**: read and change (the service applies them on the next frame and writes `settings.ini`);
- **camera settings**: the whole cache (value, allowed list and raw bytes per property) with the generation
  number, notifications when it changes, and write requests (which go into the capture thread's queue and answer
  "accepted" or "the camera refused");
- **preview**: with the panel subscribed, the service sends the **final** frame (after the adjustments, the same
  one Chrome gets), downscaled to ~640×360, at most 15 fps. For example, as a JPEG re-encoded with TurboJPEG
  (cheap and small). Stop sending when the panel leaves;
- **panel open**: while a panel is connected, it counts as a consumer for the on-demand Live View.

If the panel doesn't read in time, the service drops preview frames: the panel must never slow the webcam down.

## Settings panel (`canon-eos-webcam-panel`, GTK4)

A regular window (Hyprland manages it like any other), with a `.desktop` entry ("Canon webcam settings", with a
Portuguese `Name[pt_BR]`) to open it from the Omarchy launcher. A second instance only brings the first one to the
front. Texts in English by default, with the Portuguese translation (see "Conventions").

Layout, as on Windows:

- **Preview** on the left, live, keeping the 16:9 aspect ratio, with zoom (Fit / 100 % / 200 % / 300 %, mouse
  wheel to zoom, drag to pan, double-click to fit). It is for checking focus and sharpness.
- **Image**: the six sliders, with the value next to each (gamma with two decimals, saturation in %), and the
  "Restore defaults" button. The image changes while the user drags.
- **Camera (settings stored in the camera itself)**: an information line ("Canon EOS 1100D (only the settings the
  current mode allows)" or "Camera disconnected or turned off: settings unavailable."); combo boxes for exposure
  compensation, ISO, white balance, Picture Style and Auto Lighting Optimizer, filled with the **list announced by
  the camera**; sliders for color temperature and for the four Picture Style parameters. The camera sliders **only
  send on release** (each write is a USB operation). Controls the current mode doesn't announce are disabled
  ("n/a"). The panel updates itself when the cache generation changes (including when the user turns the camera's
  mode dial).
- **Status bar**: camera state, frames/s and whether the webcam is in use.
- An "AI effects (NVIDIA Maxine)" group does **not** go in now (see "Future"), but leave room for it in the
  layout.

If the service is not running, the panel says so and tries to reconnect by itself, without hanging.

**Testing the panel without synthetic input:** the logic (building the lists, names, encodings, Picture Style)
lives outside the widgets and can be tested through the control channel and the subcommands. To check the looks,
if possible, make the panel render itself to a PNG without showing the window (e.g. `canon-eos-webcam-panel --snapshot
file.png`, using `gtk_widget_snapshot` and the GSK renderer); if that is not possible, ask the user to open the
panel and look. Never simulate clicks or keys.

## Permissions, udev and conflicts

- File `/etc/udev/rules.d/70-canon-eos-webcam.rules`, in `/etc` so no package update overwrites it:
  - `SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", TAG+="uaccess"` (access for the session user, no root or groups).
  - Hiding the camera from gvfs: the gvfs gphoto2 volume monitor relies on the `ID_GPHOTO2` udev property. Check
    whether gvfs (`gvfs-gphoto2`) is installed. If it is, consider clearing that property for the Canon in a rule
    that runs after the libgphoto2 ones (e.g. `99-canon-eos-webcam-nogvfs.rules`). Confirm it works (`claim_interface` no
    longer returns BUSY with the file manager open) before adopting it.
- **Before touching the system:** survey what exists of the old solution (`v4l2loopback` module loaded through
  `/etc/modules-load.d` or `/etc/modprobe.d`, systemd services with gphoto2/ffmpeg, scripts in the Hyprland
  autostart). Show the list to the user and **ask before disabling or removing** anything. The two can't run
  together, because only one process can have the camera.

## Installation and service

- Install to `~/.local/bin/canon-eos-webcam`. Service at `~/.config/systemd/user/canon-eos-webcam.service`, with
  `Restart=on-failure`, `WantedBy=default.target` and a dependency on `pipewire.service`. No configuration inside
  the Omarchy files (`~/.config/hypr`, etc.), so its updates don't interfere.
- The service is **always running** (cheap without a consumer) and the PipeWire node always exists, so the camera
  shows up in Chrome even before the Canon is turned on.
- Configuration in `~/.config/canon-eos-webcam/settings.ini`: `[image]` section (image adjustments, see above), `[ui]`
  section (`language`) and `[linux]` section (crop position, format, log level). External edits are reloaded
  automatically.
- The panel installs `~/.local/bin/canon-eos-webcam-panel` and `~/.local/share/applications/canon-eos-webcam-panel.desktop`.
- Log to the journal (stdout/stderr with `systemd`), in English.
- A `scripts/install.sh` (in `linux/`) that builds, installs the binary, the unit and the udev rule (the latter
  with `sudo`, warning the user first) and runs `udevadm control --reload` + `systemctl --user enable --now canon-eos-webcam`.

## Subcommands (for testing without the browser)

- `canon-eos-webcam probe`: finds the camera, opens the session, turns Live View on, saves **one** JPEG to
  `/tmp/canon-eos-webcam-probe.jpg`, turns everything off and shows a summary (device, endpoints, timings).
- `canon-eos-webcam bench [seconds]`: captures and processes without PipeWire and measures capture frames/s, processing
  time and CPU.
- `canon-eos-webcam run`: the service in the foreground (what the systemd unit runs).
- `canon-eos-webcam process in.jpg out.yuv`: runs the pipeline over a JPEG, without a camera, with the image adjustments
  from `settings.ini`.
- `canon-eos-webcam cam`: with the service stopped, connects and lists every known camera property (code, name, current
  value, allowed list and raw bytes), like `canon-eos-webcam cam` on Windows.
- `canon-eos-webcam get` / `canon-eos-webcam set <property> <value>`: talk to the running service through the socket (e.g.
  `canon-eos-webcam set image.brightness 20`, `canon-eos-webcam set cam.wb 1`, `canon-eos-webcam set cam.style.sharpness 5`). They are for
  testing the settings without the panel.

## Steps and acceptance criteria

1. **Basic PTP:** `canon-eos-webcam probe` saves a valid 1056×704 JPEG from the T3. Running `probe` 10 times in a row works
   every time (the camera leaves remote mode properly).
2. **Continuous capture:** `canon-eos-webcam bench 60` keeps **≥ 28 fps** of capture. Unplugging and replugging the cable
   and turning the camera off and on during the test recover by themselves, without restarting the process.
3. **Pipeline:** `canon-eos-webcam process` produces a 1280×720 frame with the right crop (check by converting the `.yuv`
   with some tool already installed or by writing a debug PNG).
4. **PipeWire:** the node shows up in `pw-dump`/`wpctl status` with the right name and class. A test consumer
   (e.g. `gst-launch-1.0 pipewiresrc target-object=canon-eos-webcam ! fakesink`, if GStreamer is installed) gets frames at
   ~30 fps. Without a consumer, Live View turns off in ~5 s and the process CPU stays ≈ 0.
5. **Chrome/Meet:** "Canon EOS Webcam" shows up in Meet and the image is smooth (test done by the user).
6. **Image adjustments:** `canon-eos-webcam process` with known values in `settings.ini` gives the expected result (compare
   pixels with the formulas). Editing the file with the service running changes the image in less than 1 s, and
   `canon-eos-webcam set image.*` writes to the file. Measure the cost: with adjustments on, capture stays ≥ 28 fps.
7. **Camera settings:** `canon-eos-webcam cam` lists the properties in the table. Through `canon-eos-webcam set`, changing the white
   balance, exposure compensation, Picture Style and one of its parameters changes the image and is confirmed by
   `PropValueChanged`. A refused value returns the message "the camera refused", without bringing capture down.
8. **Panel:** live preview with zoom, the six image adjustments and the camera settings enabled according to the
   mode, in both languages. Opening the panel without any call turns Live View on; closing it turns it off in
   ~5 s. The open panel doesn't reduce the webcam frames/s. The final usage test is done by the user.
9. **Robustness:** the service starts by itself at login, survives a `pacman -Syu` with a kernel change (nothing
   is compiled against the kernel) and recovers from a PipeWire restart (`systemctl --user restart pipewire`).
10. **README** in `linux/README.md` (and `linux/README.pt-BR.md`): installation, Chrome flag, panel,
    configuration, how to diagnose (`probe`, `cam`, journal) and how to remove it.

## Future: Maxine (not now)

The NVIDIA Maxine Video Effects SDK has a Linux version (aimed at Ubuntu/RHEL and containers, with
CUDA/TensorRT). Later on Denoise, Artifact Reduction, virtual background/blur and Super Resolution can be turned
on, as on Windows. Therefore:

- leave the effects stage as an interface in the pipeline (takes and returns an RGB/BGR image of the crop, before
  scaling), with no implementation for now;
- in the panel, the "AI effects (NVIDIA Maxine)" and "Virtual background" groups come together with the effects.
  The `settings.ini` keys will be the same as on Windows (`[effects]` and `[background]` sections), so for now the
  service only preserves those sections when writing the file;
- don't link anything against CUDA. On Windows the runtime is loaded dynamically (`dlopen` on Linux) and, if it is
  missing, the effects become unavailable without bringing the pipeline down. Keep that pattern when the time
  comes.
