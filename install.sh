#!/bin/bash
# Build and install winmouse for the current user. Linux mode stays active
# until Windows mode is picked from the menu or with `winmouse-ctl mode windows`.
set -euo pipefail
cd "$(dirname "$0")"

CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/winmouse"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

mkdir -p build "$HOME/.local/bin" "$CONFIG_DIR" "$UNIT_DIR"
gcc -O2 -Wall -Wextra -o build/winmouse src/winmouse.c
install -m755 build/winmouse bin/winmouse-ctl "$HOME/.local/bin/"
install -m644 system/winmouse.service "$UNIT_DIR/"
systemctl --user daemon-reload

if [[ ! -f $CONFIG_DIR/config ]]; then
  # Values read from this machine's Windows registry (HKCU\Control Panel\Mouse).
  cat >"$CONFIG_DIR/config" <<CONF
# Windows pointer speed slider, 1-20 (10 = Windows default)
sensitivity=8
# "Enhance pointer precision" (MouseSpeed): 1 = on, 0 = off
enhance_pointer_precision=1
# Monitor DPI the curve is built for. 96 makes the pointer match Windows in
# Hyprland logical pixels on every monitor, whatever its scale.
dpi=96
# SmoothMouseXCurve / SmoothMouseYCurve (16.16 fixed point, Windows default)
smooth_mouse_x_curve=0x0,0x6e15,0x14000,0x3dc29,0x280000
smooth_mouse_y_curve=0x0,0x111fd,0x42400,0x12fc00,0x1bbc000
CONF
fi

if [[ ${1:-} != --user-only ]]; then
  root_cmd=(install -Dm644 system/70-winmouse.rules /etc/udev/rules.d/70-winmouse.rules)
  root_script="${root_cmd[*]} && install -Dm644 system/winmouse-modules.conf /etc/modules-load.d/winmouse.conf && modprobe uinput && udevadm control --reload && udevadm trigger --subsystem-match=input --subsystem-match=misc --action=change"
  if [[ -t 0 ]]; then
    sudo bash -c "$root_script"
  else
    pkexec bash -c "cd '$PWD' && $root_script"
  fi
fi

echo "winmouse installed. Switch with: winmouse-ctl mode windows | winmouse-ctl mode linux"
