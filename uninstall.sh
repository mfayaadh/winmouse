#!/bin/bash
# Remove winmouse and return to the Linux mouse settings.
set -euo pipefail
systemctl --user disable --now winmouse.service 2>/dev/null || true
rm -f "$HOME/.local/bin/winmouse" "$HOME/.local/bin/winmouse-ctl" \
  "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/winmouse.service"
systemctl --user daemon-reload
if [[ -t 0 ]]; then sudo=sudo; else sudo=pkexec; fi
$sudo rm -f /etc/udev/rules.d/70-winmouse.rules /etc/modules-load.d/winmouse.conf
echo "winmouse removed (config kept in ~/.config/winmouse)."
