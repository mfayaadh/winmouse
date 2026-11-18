#!/usr/bin/env bash
set -euo pipefail

BIN="${1:-./build/winmouse}"

# 1. EPP off at Windows default sensitivity (10) and 96 DPI must be exact 1:1 unity.
out_unity="$(printf '0 0\n1 0\n10 5\n-25 12\n' | "$BIN" --simulate 10 0 96)"
expected_unity=$'0 0\n1 0\n10 5\n-25 12'
if [[ "$out_unity" != "$expected_unity" ]]; then
  echo "FAIL: unity simulation mismatch" >&2
  printf 'Got:\n%s\nExpected:\n%s\n' "$out_unity" "$expected_unity" >&2
  exit 1
fi

# 2. EPP on at sensitivity 10, 96 DPI: verify Windows 11 curve points, factor, and output.
out_epp="$(printf '1 0\n10 5\n-25 12\n' | WINMOUSE_PRINT_CURVE=1 "$BIN" --simulate 10 1 96)"
expected_epp=$'point 0 0\npoint 98633 56111\npoint 286720 217084\npoint 885391 995312\npoint 9175040 23264925\nsegment 37282 0\nsegment 56088 -28302\nsegment 85191 -155626\nsegment 176058 -1383230\nfactor 256\n0 0\n8 4\n-39 19'
if [[ "$out_epp" != "$expected_epp" ]]; then
  echo "FAIL: EPP curve/packet simulation mismatch" >&2
  printf 'Got:\n%s\nExpected:\n%s\n' "$out_epp" "$expected_epp" >&2
  exit 1
fi

# 3. Verify sensitivity 8 EPP on 96 DPI and high-DPI (192) produce valid integer pairs and zero -> zero.
for cfg in "8 1 96" "20 1 192" "1 0 120"; do
  read -r sens epp dpi <<<"$cfg"
  res="$(printf '0 0\n5 -3\n' | "$BIN" --simulate "$sens" "$epp" "$dpi")"
  first_line="$(printf '%s\n' "$res" | sed -n '1p')"
  second_line="$(printf '%s\n' "$res" | sed -n '2p')"
  if [[ "$first_line" != "0 0" ]] || [[ ! "$second_line" =~ ^-?[0-9]+\ -?[0-9]+$ ]]; then
    echo "FAIL: invalid output for config '$cfg': $res" >&2
    exit 1
  fi
done

echo "OK: winmouse --simulate regression checks passed"
