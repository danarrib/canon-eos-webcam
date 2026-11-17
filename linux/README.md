# canon-eos-webcam for Linux

Turns a Canon EOS (tested: Rebel T3 / 1100D) into a **PipeWire virtual camera** that Chrome, Google Meet and any
other PipeWire/portal application can use. It talks to the camera directly over USB (PTP, through libusb), so it
needs **no gphoto2, no ffmpeg, no v4l2loopback and no kernel module**. Português: [README.pt-BR.md](README.pt-BR.md).

Dependencies (all from the official Arch repositories, stable ABIs): `libusb`, `libjpeg-turbo` and `libpipewire` in
the service; additionally `gtk4` in the settings panel. Build tools: `cmake`, a C++20 compiler, `pkgconf`.

## Install

```bash
sudo pacman -S --needed cmake libusb libjpeg-turbo pipewire gtk4   # build + run dependencies
linux/scripts/install.sh
```

`install.sh` builds, installs `canon-eos-webcam` and `canon-eos-webcam-panel` into `~/.local/bin`, the systemd user
unit, the launcher entry ("Canon webcam settings") and starts the service. It also offers to install the udev rule
**with `sudo`** (it warns first; skip it with `--no-udev`). The rule is usually unnecessary: systemd already gives
the logged-in user access to PTP cameras. Make sure `~/.local/bin` is in your `PATH`.

The service runs all the time (it costs nothing while idle) and the camera **Canon EOS Webcam** always exists in
PipeWire, even before the Canon is turned on: it then shows a dark gray frame. Live View is turned on only while
someone uses the camera (a PipeWire consumer such as Chrome, or the open panel) and turned off 5 s after the last
one leaves.

**Only one program can own the camera at a time.** Stop any old gphoto2 / v4l2loopback chain before starting this
one. If another program holds the camera the log says which one.

### Chrome / Google Meet

Chrome reads cameras through the xdg-desktop-portal when PipeWire camera support is on. Open
`chrome://flags/#enable-webrtc-pipewire-camera`, set it to **Enabled** and restart Chrome. `xdg-desktop-portal` must
be running (it is on Omarchy). Then "Canon EOS Webcam" appears in the camera list of Meet. (Check the portal with
`busctl --user get-property org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.Camera IsCameraPresent`,
which must say `b true` while the service runs.)

The picture is 1280×720 at 30 fps in a single pixel format (YUY2 by default).

## Settings panel

Run `canon-eos-webcam-panel` (or "Canon webcam settings" in the launcher). Left: live preview (mouse wheel or the
buttons zoom to 100 / 200 / 300 %, drag to pan, double-click to fit; zoomed in it requests the full 1280-wide frame
so checking focus is meaningful). Right: the six **image adjustments** done by canon-eos-webcam (brightness, contrast,
gamma, temperature, tint, saturation) and the **camera settings** written to the Canon itself (exposure
compensation, ISO, white balance, color temperature, Picture Style with contrast / sharpness / saturation / color
tone, Auto Lighting Optimizer). Only what the camera's current mode allows is enabled. The camera sliders send when
you release them. A second launch just brings the first window to the front. Opening the panel turns Live View on;
closing it turns it off 5 s later. The panel never affects the webcam: if it hangs, the service carries on.

`canon-eos-webcam-panel --snapshot out.png [--preview frame.jpg]` renders the panel with demo data to a PNG without
showing a window (used for checking the layout).

## Configuration

`~/.config/canon-eos-webcam/settings.ini` (the same file format as the Windows version; sections this program doesn't
own are preserved). Edits are applied within a second while the service runs.

```ini
[image]            ; same keys and ranges as on Windows
brightness = 0     ; -100..100
contrast = 0       ; -100..100
gamma = 1.00       ; 0.50..2.00
temperature = 0    ; -100..100
tint = 0           ; -100..100
saturation = 100   ; 0..200
[ui]
language = auto    ; auto | en | pt
[linux]
crop = bottom      ; bottom | center | top : which 16:9 band of the 3:2 Live View frame is used
format = yuy2      ; yuy2 | i420 | nv12 : PipeWire pixel format (read at startup)
loglevel = info    ; info | debug
```

Language: `CANON_EOS_WEBCAM_LANG` (`en`|`pt`), then `[ui] language`, then `LANG` / `LC_MESSAGES`.
Other environment variables: `CANON_EOS_WEBCAM_SETTINGS` (another settings file), `CANON_EOS_WEBCAM_SOCKET`,
`CANON_EOS_WEBCAM_DUMP_EVENTS=1` (log every camera property event in hex).

## Command line

| Command | |
|---|---|
| `canon-eos-webcam run` | the service in the foreground (what systemd starts) |
| `canon-eos-webcam get` | state of the running service (camera, fps, image adjustments, camera properties) |
| `canon-eos-webcam set <key> <value>` | `image.{brightness,contrast,gamma,temperature,tint,saturation}`, `cam.{exp,iso,wb,kelvin,style,alo}`, `cam.style.{contrast,sharpness,saturation,tone}`; camera values accept numbers (`0x81`) or names (`daylight`, `standard`, ISO `400`) |
| `canon-eos-webcam probe` | opens the camera, saves one frame to `/tmp/canon-eos-webcam-probe.jpg`, turns everything off |
| `canon-eos-webcam bench [seconds]` | capture + processing without PipeWire: fps, ms per frame, CPU |
| `canon-eos-webcam process in.jpg out.yuv [debug.jpg]` | runs the pipeline over a JPEG, no camera |
| `canon-eos-webcam cam` | (service stopped) lists every camera property with its allowed values and raw bytes |

## Diagnosing

* `journalctl --user -u canon-eos-webcam -f` (statistics every 60 s; connection problems say in plain words what is
  wrong, e.g. which process holds the camera, or that the udev rule is missing).
* `canon-eos-webcam probe` and `canon-eos-webcam cam` test the camera without PipeWire (stop the service first:
  `systemctl --user stop canon-eos-webcam`).
* `pw-dump | grep -B3 -A12 canon-eos-webcam` or `wpctl status`: the node is `Video/Source`, role `Camera`.
* Test consumer: `gst-launch-1.0 pipewiresrc ! fakesink` (some WirePlumber setups refuse the automatic link of a
  GStreamer consumer; `pw-link canon-eos-webcam:capture_1 <consumer>:input_1` links by hand).

## How it works

`canon-eos-webcam run` has four threads: **capture** (the only owner of the USB/PTP session: connects, fetches the
Live View JPEGs at ~30 fps, reads camera events, runs queued property writes between frames, reconnects by itself),
**processing** (always takes the most recent JPEG: TurboJPEG decode → 16:9 crop → image adjustments → bilinear scale
to 1280×720 → YUV; there is an empty stage for the AI effects of the Windows version), **PipeWire** (the stream is
the graph driver, triggered by a 30 Hz timer only while a consumer is linked; it repeats the last frame or sends a
placeholder) and **control** (a Unix socket, `$XDG_RUNTIME_DIR/canon-eos-webcam.sock`, one JSON object per line,
that the panel and `get`/`set` use; also watches `settings.ini` with inotify). The panel is a separate GTK4 program
that only speaks to that socket (`socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/canon-eos-webcam.sock`, then
`{"cmd":"get"}`). The Canon commands and the Picture Style handling are the ones of the Windows version
(`src/app/canon_camera.cpp`, `src/app/panel.cpp`).

## Remove

```bash
linux/scripts/uninstall.sh
```
