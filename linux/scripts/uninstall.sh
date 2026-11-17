#!/usr/bin/env bash
# Removes what scripts/install.sh installed (the udev rules need sudo and are asked about).
# Settings in ~/.config/canon-eos-webcam are kept.
set -euo pipefail

systemctl --user disable --now canon-eos-webcam.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/canon-eos-webcam.service"
rm -f "$HOME/.local/bin/canon-eos-webcam" "$HOME/.local/bin/canon-eos-webcam-panel"
rm -f "$HOME/.local/share/applications/canon-eos-webcam-panel.desktop"
systemctl --user daemon-reload

rules=(/etc/udev/rules.d/70-canon-eos-webcam.rules /etc/udev/rules.d/99-canon-eos-webcam-nogvfs.rules)
present=()
for r in "${rules[@]}"; do [ -e "$r" ] && present+=("$r"); done
if [ "${#present[@]}" -gt 0 ]; then
  echo "The udev rules ${present[*]} need sudo to be removed."
  read -r -p "Remove them with sudo? [y/N] " ans
  if [[ "$ans" =~ ^[yY] ]]; then
    sudo rm -f "${present[@]}"
    sudo udevadm control --reload
  fi
fi
echo "Removed. Your settings (~/.config/canon-eos-webcam) were kept."
