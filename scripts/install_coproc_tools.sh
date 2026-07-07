#!/usr/bin/env bash
# ── install_coproc_tools.sh ──────────────────────────────────────────────────
# Install the toolchain to BUILD and FLASH the ProtoHUD coprocessor firmware
# from the CM5 (self-build path — `scripts/flash_coproc.sh --build`). OPTIONAL:
# only needed if you reflash the RP2350 coprocessor. For the ProtoHUD host build
# dependencies see scripts/install_deps.sh.
#
# Tested on Raspberry Pi OS Bookworm / Debian Trixie (aarch64).
set -euo pipefail

sudo apt-get update

# picotool — writes the UF2 into the RP2350 over USB (used by flash_coproc.sh).
if ! sudo apt-get install -y picotool 2>/dev/null; then
  echo "[warn] 'picotool' isn't in apt on this release."
  echo "       Build it from source: https://github.com/raspberrypi/picotool"
fi

# PlatformIO Core — builds the firmware and pulls the RP2350 / earlephilhower
# toolchain on the first 'pio run'. Installed via pipx to respect Debian's
# externally-managed Python (PEP 668).
sudo apt-get install -y pipx
pipx install platformio || pipx upgrade platformio || true
pipx ensurepath

# Let picotool reach RP2350 boards without sudo: the plugdev group gets access
# to VID 2e8a (both the running app and the BOOTSEL bootloader enumerate under
# it). uaccess also grants the logged-in user directly.
RULE=/etc/udev/rules.d/99-picotool.rules
sudo tee "$RULE" >/dev/null <<'EOF'
# Raspberry Pi RP2040/RP2350 (app + BOOTSEL) — no sudo needed for picotool.
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0660", GROUP="plugdev", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger || true
sudo usermod -aG plugdev,dialout "${SUDO_USER:-$USER}" || true

echo ""
echo "Coprocessor toolchain installed."
echo "  Reflash from source:  scripts/flash_coproc.sh --build"
echo "  Log out and back in once so the new group membership + PATH take effect."
