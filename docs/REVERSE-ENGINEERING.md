# How Windows 11 moves the pointer (win32kbase.sys)

Source: `C:\Windows\System32\win32kbase.sys` (Windows 11, October 2026 build,
PDB `win32kbase.pdb`, GUID `7BD4193FFA935A0483172A97C84B455D1`, public symbols
from the Microsoft symbol server). Reference configuration values correspond to
`HKCU\Control Panel\Mouse` defaults with Windows slider 8:

| Value | This machine |
|---|---|
| `MouseSensitivity` | `8` |
| `MouseSpeed` | `1` (Enhance pointer precision **on**) |
| `SmoothMouseXCurve` | `0, 0x6E15, 0x14000, 0x3DC29, 0x280000` (Windows default) |
| `SmoothMouseYCurve` | `0, 0x111FD, 0x42400, 0x12FC00, 0x1BBC000` (Windows default) |

All values are 16.16 fixed point; all math is 64-bit integer.

## Speed slider: `UpdateMouseSensitivity` @ `0x1401A6920`

Gain in 1/256 units, stored at `MOUSE_SENSITIVITY_INFO+4`:

```
s <= 2  : s * 8                 (1/32, 1/16)
s <= 10 : (s*256 - 512) >> 3    (1/8 ... 1.0)
s > 10  : (s*256 - 1536) >> 2   (1.25 ... 3.5)
```

## EPP off: `CMouseProcessor::ApplyAccelerationToDelta` @ `0x14004E624`

`GetNormalizedMouseSensitivityFactor` (`0x14004ED08`) multiplies the gain by
`(gain*dpi + 48) / 96` when the monitor under the cursor is above 96 DPI. A gain of
exactly 256 passes the delta through unchanged (only a random subpixel is set).
Otherwise each axis is `n = gain*d + remainder; out = n / 256; remainder = n % 256`.

## EPP on, curve: `CDeviceAcceleration::_BuildAccelerationCurve` @ `0x1401B1A24`

Called per monitor from `ResetAccelerationCurves` (`0x14014E624`) with that
monitor's DPI (`node+0x24`); the result is stored at `node+0xD8`.

```
sens  = (MouseSensitivity << 16) / 10        # EPP-on slider is linear, 8 -> 0.8
scale = (max(dpi, 96) << 16) / 120           # 96 DPI -> 0.8, 120 DPI -> 1.0
X[i]  = (SmoothMouseXCurve[i] * 3.5)         # 0x38000 in 16.16
Y[i]  = ((scale * SmoothMouseYCurve[i]) >> 16) * sens >> 16
slope[i]     = ((Y[i+1]-Y[i]) << 16) / (X[i+1]-X[i])
intercept[i] = Y[i] - (slope[i] * X[i] >> 16)
```

## EPP on, per packet: `CDeviceAcceleration::Accelerate` @ `0x14004E8F8`

Runs once per mouse report (not per unit of time, so the result depends on
polling rate, the same as on Windows):

1. `speed = max(|dx|,|dy|) + min(|dx|,|dy|)/2` (16.16, an octagonal approximation
   of the vector length).
2. Segment `i` is the first point with `speed <= X[i+1]`, clamped to the last
   segment (the curve is extrapolated beyond `X[4]`).
3. `gain = (intercept[i] << 16) / speed + slope[i]`, i.e. `Y(speed)/speed`.
4. If the previous packet used a **lower** segment, the gain is averaged with
   that segment's line: `gain = (gain + gain_prev) >> 1`. This smooths
   accelerating into a steeper segment and is easy to miss in reimplementations.
5. `out = (d << 16) * gain >> 16 + remainder`; the integer part is truncated
   toward zero and the 16-bit fraction is kept as the remainder for the next
   packet (one remainder per axis, shared by all mice).

## Verification

`verify/verify_against_windows.py` loads the original `win32kbase.sys` into the
Unicorn CPU emulator and runs Microsoft's own machine code for
`UpdateMouseSensitivity`, the DPI normalisation, `_BuildAccelerationCurve`, the
`Accelerate` core and the EPP-off branch, then compares every value with
`winmouse --simulate`:

```
OK: 600000 packets identical to win32kbase.sys across sensitivity 1-20,
    EPP on/off, DPI 96/120/144/168/192
```

Mutation check: removing the segment-averaging rule or changing the speed
formula each makes the harness fail on the first few packets.

## Mapping to Hyprland

Windows outputs physical pixels and builds the curve with the monitor's DPI, so
on a 125% monitor it moves 1.25× more physical pixels. Hyprland moves the cursor
in logical pixels, which already scale by the monitor scale. Building the curve
for 96 DPI and sending the result through a flat, 1:1 libinput device therefore
gives the same physical movement as Windows on every monitor where the Hyprland
scale equals the Windows display scale. The only remaining difference is integer
rounding on scaled monitors (Windows rounds in physical pixels, winmouse in
logical pixels), which is below one pixel and never accumulates.
