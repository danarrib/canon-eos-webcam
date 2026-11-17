#!/usr/bin/env bash
# Builds canon-eos-webcam and installs it for the current user (no root needed, except for the optional udev
# rule). Usage: scripts/install.sh [--no-udev] [--yes]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
want_udev=1
assume_yes=0
for a in "$@"; do
  case "$a" in
    --no-udev) want_udev=0 ;;
    --yes|-y) assume_yes=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

for tool in cmake g++ pkg-config; do
  command -v "$tool" >/dev/null || { echo "missing build tool: $tool (sudo pacman -S cmake gcc pkgconf)" >&2; exit 1; }
done

echo "==> Building"
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$here/build" -j"$(nproc)"

# Only one process can own the camera: warn about the usual suspects of the old gphoto2/v4l2loopback setup.
if pgrep -x gphoto2 >/dev/null || pgrep -x gvfsd-gphoto2 >/dev/null; then
  echo "WARNING: gphoto2 / gvfsd-gphoto2 is running and will keep the camera busy. Stop it first." >&2
fi
if lsmod 2>/dev/null | grep -q '^v4l2loopback'; then
  echo "note: the v4l2loopback module is loaded. It is not used by canon-eos-webcam; remove it if you no longer need it."
fi

echo "==> Installing to ~/.local"
bin="$HOME/.local/bin"
mkdir -p "$bin" "$HOME/.config/systemd/user" "$HOME/.local/share/applications"
# stop a running instance so the binary can be replaced
systemctl --user stop canon-eos-webcam.service 2>/dev/null || true
install -m 0755 "$here/build/canon-eos-webcam" "$bin/canon-eos-webcam"
install -m 0755 "$here/build/canon-eos-webcam-panel" "$bin/canon-eos-webcam-panel"
install -m 0644 "$here/systemd/canon-eos-webcam.service" "$HOME/.config/systemd/user/canon-eos-webcam.service"
# the launcher may not have ~/.local/bin in its PATH: use the absolute path
sed "s|^Exec=.*|Exec=$bin/canon-eos-webcam-panel|" "$here/canon-eos-webcam-panel.desktop" > "$HOME/.local/share/applications/canon-eos-webcam-panel.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

if [ "$want_udev" = 1 ]; then
  echo
  echo "==> udev rule (needs sudo)"
  echo "The next step uses sudo to copy linux/udev/70-canon-eos-webcam.rules to /etc/udev/rules.d/ and reload udev."
  echo "It gives your logged-in user access to the Canon over USB. On most systems PTP cameras already get this"
  echo "access, so you can skip it with --no-udev."
  if [ "$assume_yes" = 0 ]; then
    read -r -p "Continue with sudo? [y/N] " ans
  else
    ans=y
  fi
  if [[ "$ans" =~ ^[yY] ]]; then
    sudo install -m 0644 "$here/udev/70-canon-eos-webcam.rules" /etc/udev/rules.d/70-canon-eos-webcam.rules
    if pacman -Qq gvfs-gphoto2 >/dev/null 2>&1; then
      echo "gvfs-gphoto2 is installed: adding the rule that keeps it away from the camera"
      sudo install -m 0644 "$here/udev/99-canon-eos-webcam-nogvfs.rules" /etc/udev/rules.d/99-canon-eos-webcam-nogvfs.rules
    fi
    sudo udevadm control --reload
    sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=04a9
  else
    echo "skipped the udev rule"
  fi
fi

echo "==> Starting the service"
systemctl --user daemon-reload
systemctl --user enable --now canon-eos-webcam.service
sleep 1
systemctl --user --no-pager status canon-eos-webcam.service | head -n 6 || true
echo
echo "Done. The camera \"Canon EOS Webcam\" is now available to PipeWire applications."
echo "Settings panel: run canon-eos-webcam-panel (or look for \"Canon webcam settings\" in the launcher)."
echo "Make sure ~/.local/bin is in your PATH. See linux/README.md for the Chrome flag and troubleshooting."
