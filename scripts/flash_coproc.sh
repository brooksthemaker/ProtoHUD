#!/usr/bin/env bash
# ── flash_coproc.sh ──────────────────────────────────────────────────────────
# Flash the ProtoHUD button/voice coprocessor (RP2350) from the CM5 over USB —
# no BOOTSEL button press. It resets the running firmware into the UF2 bootloader
# (the Arduino "1200-baud touch"), then writes the image with picotool and
# reboots into it.
#
# The touch needs firmware already running to reset. A blank board (or one held
# in BOOTSEL) has no serial port to touch, so flash those directly instead:
#   picotool load -x firmware/button_coproc/pico/.pio/build/coproc_voice/firmware.uf2
#
# Usage:
#   scripts/flash_coproc.sh path/to/firmware.uf2      # flash a prebuilt image
#   scripts/flash_coproc.sh --build                   # build the env below, then flash
#   scripts/flash_coproc.sh --env coproc fw.uf2       # plain (non-voice) build
#   scripts/flash_coproc.sh --device /dev/serial/by-id/...-if00 fw.uf2
#
# Requires: picotool  (sudo apt install picotool). Building also needs PlatformIO.
# picotool may need sudo or a udev rule (99-picotool.rules) for USB access.
#
# ProtoHUD can keep running: when the Pico resets, its serial port drops and the
# HUD's CoprocInputs auto-reconnects afterward. Stop the HUD first only if you
# want a clean, uncontended log.
set -euo pipefail

# The firmware's USB identity, as udev names it: usb-<mfr>_<product>_<serial>.
# Resolved by glob because the board serial differs per unit; --device overrides
# (needed for a board still running stock-named firmware, e.g. first flash).
DEVICE="$(ls /dev/serial/by-id/usb-ProtoHUD_Buttons*-if00 2>/dev/null | head -1 ||
          true)"
DEVICE="${DEVICE:-/dev/serial/by-id/usb-ProtoHUD_Buttons-if00}"
FW_DIR="$(cd "$(dirname "$0")/.." && pwd)/firmware/button_coproc/pico"
ENV="coproc_voice"
uf2=""
do_build=0

while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2;;
    --env)    ENV="$2";    shift 2;;
    --build)  do_build=1;  shift;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    -*) echo "unknown option: $1" >&2; exit 2;;
    *)  uf2="$1"; shift;;
  esac
done

command -v picotool >/dev/null 2>&1 || {
  echo "picotool not found — run scripts/install_coproc_tools.sh" >&2; exit 1; }

# Optionally build the UF2 first.
if [ "$do_build" = 1 ]; then
  command -v pio >/dev/null 2>&1 || {
    echo "pio (PlatformIO) not found — run scripts/install_coproc_tools.sh" >&2; exit 1; }
  echo "[build] pio run -e $ENV"
  ( cd "$FW_DIR" && pio run -e "$ENV" )
  uf2="$(ls -t "$FW_DIR/.pio/build/$ENV"/*.uf2 2>/dev/null | head -1 || true)"
fi

[ -n "$uf2" ] && [ -f "$uf2" ] || {
  echo "no .uf2 given or found — pass a path, or use --build" >&2; exit 1; }
echo "[flash] image: $uf2"

# 1) Reset the running Pico into the UF2 bootloader (Arduino 1200-baud touch).
if [ -e "$DEVICE" ]; then
  echo "[reset] 1200-baud touch on $DEVICE"
  stty -F "$DEVICE" 1200 2>/dev/null || true
  # pyserial is the reliable way to open-at-1200-then-close; fall back to a
  # raw open/close of the tty if it isn't installed.
  if ! python3 - "$DEVICE" <<'PY' 2>/dev/null
import sys, time
import serial
p = serial.Serial(sys.argv[1], 1200)
time.sleep(0.15)
p.close()
PY
  then
    ( exec 3<>"$DEVICE"; exec 3>&- ) 2>/dev/null || true
  fi
else
  echo "[reset] $DEVICE not present — assuming the board is already in BOOTSEL"
fi

# 2) Wait for the RP2 bootloader to enumerate.
echo -n "[wait] for BOOTSEL device"
ok=0
for _ in $(seq 1 30); do
  if picotool info >/dev/null 2>&1; then ok=1; break; fi
  echo -n "."; sleep 0.5
done
echo
[ "$ok" = 1 ] || {
  echo "BOOTSEL device did not appear. Hold BOOTSEL while plugging in and retry," >&2
  echo "or check picotool USB permissions (sudo / udev rule)." >&2; exit 1; }

# 3) Flash and run (-x reboots into the app after loading).
echo "[flash] picotool load -x"
picotool load -x "$uf2"

# 4) Wait for the app's serial port to come back, then show its HELLO (fw=…).
# Re-glob rather than reusing $DEVICE: flashing firmware with different USB
# strings (or a first flash from a stock-named board) changes the by-id path.
echo -n "[wait] for the ProtoHUD Buttons port to re-appear"
for _ in $(seq 1 30); do
  NEW_DEV="$(ls /dev/serial/by-id/usb-ProtoHUD_Buttons*-if00 2>/dev/null | head -1 || true)"
  [ -n "$NEW_DEV" ] && { DEVICE="$NEW_DEV"; break; }
  echo -n "."; sleep 0.5
done
echo
echo "[done] flashed. HELLO from the new firmware:"
timeout 3 head -n 1 "$DEVICE" 2>/dev/null || echo "  (no line yet — reconnect to confirm the fw= version)"
