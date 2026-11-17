# winmouse

[Bahasa Indonesia](README.id.md)

Exact Windows 11 pointer ballistics for Linux / Omarchy (Hyprland) — reverse engineered from `win32kbase.sys` and verified bit-for-bit across **600,000 packets** against Microsoft's own machine code (see [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md)).

---

## Why This Exists

I dual-boot **Windows 11** and **Linux (Omarchy / Hyprland)** on the same laptop for two completely different work streams — one workflow lives on Windows, while my software engineering and server work lives on Linux.

Every time I rebooted between the two OSes, my pointer muscle memory broke. Windows moves the cursor using its own 1–20 sensitivity slider and **"Enhance pointer precision"** (`EPP`) 16.16 fixed-point acceleration curve, whereas Linux (`libinput`) uses an entirely different acceleration curve. Constantly switching environments felt disorienting and forced endless sensitivity tweaking just to feel natural again.

Trying to approximate Windows sensitivity manually with `libinput` sliders never felt right — so instead of guessing, I disassembled Windows 11's actual pointer pipeline in `win32kbase.sys`, verified a C implementation bit-for-bit against the Windows binary in the Unicorn CPU emulator, and wrapped it in a hot-switchable Linux `evdev` → `uinput` daemon.

---

## How It Works

`winmouse` runs as a lightweight single-process C daemon without replacing your existing Linux/Hyprland mouse setup. You can flip between two modes at any time:

- **Linux mode (daemon stopped):** physical mice go straight to `libinput`, using your normal `~/.config/hypr/input.lua` settings untouched.
- **Windows 11 mode (daemon running):** grabs USB and Bluetooth mice (`ID_INPUT_MOUSE=1`), transforms each HID frame in 16.16 fixed-point integer math (matching Windows's slider gain, DPI normalization, 5-point acceleration curve, segment-transition gain averaging, and subpixel remainder carry-over), and re-emits events through a virtual device named `WinMouse Virtual Pointer` that Hyprland treats as `flat` (1:1).

Safety & scope:
- The laptop's built-in I2C touchpad is never grabbed or modified.
- Keyboards stay isolated and inaccessible (`70-winmouse.rules` grants `uaccess` ACL only to `/dev/uinput` and mouse `event*` nodes for the active local session — no `input` group membership needed).
- If the daemon is stopped or crashes, the Linux kernel automatically releases the device grab and mice revert instantly to native Linux behaviour.

---

## Install

Requires `gcc`, `systemd` user session, `uinput` kernel module, and `sudo`/`pkexec` once for the udev rule.

```sh
./install.sh
```

By default, `install.sh`:
1. Compiles `src/winmouse.c` cleanly (`-O2 -Wall -Wextra`) to `build/winmouse`.
2. Installs `winmouse` and `winmouse-ctl` to `~/.local/bin/` and the user service to `~/.config/systemd/user/winmouse.service`.
3. Seeds `~/.config/winmouse/config` with default Windows 11 acceleration curves (`SmoothMouseXCurve` / `SmoothMouseYCurve`, sensitivity `8`, `EPP=1`, `dpi=96`).
4. Installs `/etc/udev/rules.d/70-winmouse.rules` and `/etc/modules-load.d/winmouse.conf` (skip root install with `./install.sh --user-only`).

Linux mode stays active after installation until you switch to Windows mode.

---

## Usage

From the **Omarchy** menu (**Setup → Mouse Sensitivity**), or via `winmouse-ctl`:

```sh
winmouse-ctl mode windows     # Enable Windows 11 pointer ballistics
winmouse-ctl mode linux       # Return to Hyprland / libinput default
winmouse-ctl toggle           # Flip between Linux and Windows modes
winmouse-ctl speed 8          # Set Windows pointer speed slider (1–20; 10 = Windows default)
winmouse-ctl epp on|off|toggle # Toggle "Enhance pointer precision"
winmouse-ctl status           # Show active mode and settings
```

Configuration lives in `~/.config/winmouse/config`; changes via `winmouse-ctl` send `SIGHUP` to reload live without dropping the mouse.

### Hyprland / Omarchy Flat Profile Note

`winmouse` outputs pre-accelerated deltas, so Hyprland should treat `"WinMouse Virtual Pointer"` (or `winmouse-virtual-pointer`) with a flat `1:1` profile (`accel_profile = flat`) so `libinput` does not apply a second acceleration curve on top of Windows ballistics.

---

## Reverse Engineering & Bit-for-Bit Verification

`winmouse` transcribes five routines from `win32kbase.sys` (PDB GUID `7BD4193FFA935A0483172A97C84B455D1`):

- `CMouseAcceleration::MOUSE_SENSITIVITY_INFO::UpdateMouseSensitivity`
- `GetNormalizedMouseSensitivityFactor`
- `CMouseProcessor::ApplyAccelerationToDelta` (EPP off)
- `CDeviceAcceleration::_BuildAccelerationCurve`
- `CDeviceAcceleration::Accelerate` (EPP on)

`verify/verify_against_windows.py` loads a real `win32kbase.sys` binary into the Unicorn CPU emulator and compares its exact register/memory outputs against `winmouse --simulate`:

```sh
gcc -O2 -Wall -Wextra -o build/winmouse src/winmouse.c
uv run --with unicorn python verify/verify_against_windows.py /path/to/win32kbase.sys build/winmouse
# OK: 600000 packets identical to win32kbase.sys across sensitivity 1-20,
#     EPP on/off, DPI 96/120/144/168/192
```

For the full disassembly notes and fixed-point equations, read [docs/REVERSE-ENGINEERING.md](docs/REVERSE-ENGINEERING.md).

---

## Project Layout

```text
├── src/winmouse.c                  # evdev grab + Windows 11 fixed-point math + uinput daemon
├── bin/winmouse-ctl                # CLI controller (mode, toggle, speed, epp, status)
├── system/
│   ├── 70-winmouse.rules           # uaccess ACL rule for /dev/uinput & mouse event nodes only
│   ├── winmouse-modules.conf       # loads uinput at boot
│   └── winmouse.service            # systemd user service with SIGHUP live reload
├── verify/
│   └── verify_against_windows.py   # Unicorn CPU emulator harness (600,000-packet check)
├── docs/
│   └── REVERSE-ENGINEERING.md      # Disassembly walkthrough & Hyprland DPI mapping
├── install.sh
└── uninstall.sh
```

## Uninstall

```sh
./uninstall.sh
```

## License

[MIT](LICENSE)
