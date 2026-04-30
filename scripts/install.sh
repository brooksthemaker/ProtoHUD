#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
#  ProtoHUD Installer — Raspberry Pi CM5 (aarch64)
#  Supported: Raspberry Pi OS Bookworm · Debian Trixie + RPT packages
#  Run once from the project root:
#    chmod +x scripts/install.sh && ./scripts/install.sh
#
#  What this does (in order):
#    1. Preflight  — check OS, arch, sudo availability
#    2. Packages   — build tools, libraries, headers
#    3. RP2350     — check for USB audio device
#    4. Boot cfg   — gpu_mem, camera_auto_detect in /boot/firmware/config.txt
#    5. udev       — stable /dev/teensy, /dev/smartknob, /dev/lora symlinks
#    6. Android    — v4l2loopback module config for Android mirror
#    7. Groups     — add current user to gpio / dialout / video / render / audio
#    8. Build      — cmake + ninja the project
#    9. Libraries  — install VITURE SDK .so files to /usr/local/lib
#   10. Service    — optional systemd unit (auto-start on login session)
#   11. Summary    — what changed, what still needs a reboot
# ══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()         { echo -e "${CYAN}[info]${RESET}  $*"; }
ok()           { echo -e "${GREEN}[ ok ]${RESET}  $*"; }
warn()         { echo -e "${YELLOW}[warn]${RESET}  $*"; }
fatal()        { echo -e "${RED}[FAIL]${RESET}  $*" >&2; exit 1; }
section()      { echo -e "\n${BOLD}══ $* ══${RESET}"; }
section_inline(){ echo -e "\n${BOLD}── $* ──${RESET}"; }

# ── Locate project root (directory containing this script/../) ────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# ── Preflight ─────────────────────────────────────────────────────────────────
section "1 / 11  Preflight"

# Determine the real user (works whether run directly or via sudo)
REAL_USER="${SUDO_USER:-${USER:-$(whoami)}}"
REAL_HOME=$(getent passwd "${REAL_USER}" | cut -d: -f6)
info "Installing for user: ${REAL_USER}  (home: ${REAL_HOME})"

ARCH=$(uname -m)
[[ "${ARCH}" == "aarch64" ]] || warn "Expected aarch64, got ${ARCH} — proceeding anyway"

# Verify sudo is available and cache credentials once up front
if ! command -v sudo &>/dev/null; then
    fatal "sudo is not installed. Install it with: su -c 'apt-get install sudo'"
fi
info "Requesting sudo credentials (will be re-used throughout install)..."
sudo -v || fatal "sudo authentication failed"
# Keep sudo alive in the background for the duration of the script
( while true; do sudo -v; sleep 50; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill "${SUDO_KEEPALIVE_PID}" 2>/dev/null' EXIT

# OS detection — Bookworm/Trixie use /boot/firmware/config.txt; Bullseye used /boot/config.txt
if [[ -f /boot/firmware/config.txt ]]; then
    BOOT_CFG="/boot/firmware/config.txt"
    OVERLAY_DIR="/boot/firmware/overlays"
elif [[ -f /boot/config.txt ]]; then
    BOOT_CFG="/boot/config.txt"
    OVERLAY_DIR="/boot/overlays"
else
    fatal "Cannot find /boot/config.txt or /boot/firmware/config.txt"
fi
info "Boot config: ${BOOT_CFG}"
info "Overlay dir: ${OVERLAY_DIR}"

NEEDS_REBOOT=false

# ── Packages ──────────────────────────────────────────────────────────────────
section "2 / 11  System packages"

PKGS=(
    # Build tools
    cmake ninja-build pkg-config git curl
    device-tree-compiler       # dtc — compiles .dts → .dtbo

    # GLES2/EGL (handled separately — GLFW3 has its own fallback below)
    # Trixie uses libgl-dev/libgles-dev/libegl-dev; Bookworm/Bullseye use the
    # -mesa- prefixed names. The mesa names are transitional on Trixie and still
    # resolve correctly, so we use them for backwards compatibility.
    libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev

    # Dear ImGui is fetched via CMake FetchContent — no apt package needed

    # libcamera (OWLsight CSI cameras)
    libcamera-dev

    # OpenCV (USB cameras + MJPEG)
    libopencv-dev

    # nlohmann/json
    nlohmann-json3-dev

    # libgpiod v2 (GPIO buttons — Bookworm/Trixie ship v2; code uses the v2 API)
    libgpiod-dev

    # ALSA (spatial audio)
    libasound2-dev

    # Runtime: ldconfig, udevadm
    kmod udev

    # Android mirror — V4L2 loopback device
    # v4l2loopback-dkms creates a virtual /dev/video* that scrcpy writes into;
    # OpenCV reads those frames so ProtoHUD can display them as an overlay.
    # scrcpy itself is NOT installed here — install it separately afterward.
    v4l2loopback-dkms
    adb
)

# Only install packages that are not already present
PKGS_MISSING=()
for pkg in "${PKGS[@]}"; do
    if dpkg -s "${pkg}" &>/dev/null 2>&1; then
        info "Already installed: ${pkg}"
    else
        PKGS_MISSING+=("${pkg}")
    fi
done

if [[ ${#PKGS_MISSING[@]} -gt 0 ]]; then
    info "Installing ${#PKGS_MISSING[@]} package(s): ${PKGS_MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y "${PKGS_MISSING[@]}"
    ok "Packages installed: ${PKGS_MISSING[*]}"
else
    ok "All packages already installed — skipping apt"
fi

# ── GLFW3 (window + input) ───────────────────────────────────────────────────
# Trixie/Bookworm ship GLFW 3.4 in apt. Bullseye shipped 3.3 and sometimes put
# the pkg-config file in a multiarch path CMake didn't search.
# Strategy: try apt → verify pkg-config finds it → build from source if needed.
section_inline "GLFW3"

glfw3_ok() {
    PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
    pkg-config --exists glfw3 2>/dev/null
}

if glfw3_ok; then
    ok "GLFW3 already found by pkg-config ($(pkg-config --modversion glfw3))"
else
    # Try the apt package first
    if ! dpkg -s libglfw3-dev &>/dev/null 2>&1; then
        sudo apt-get install -y libglfw3-dev 2>/dev/null || true
    fi

    # Some Bullseye installs put the .pc file under the multiarch path only.
    # Export it so both this shell and the later cmake invocation can find it.
    MULTIARCH_PC="/usr/lib/aarch64-linux-gnu/pkgconfig"
    if [[ -f "${MULTIARCH_PC}/glfw3.pc" ]]; then
        export PKG_CONFIG_PATH="${MULTIARCH_PC}:${PKG_CONFIG_PATH:-}"
        info "Added ${MULTIARCH_PC} to PKG_CONFIG_PATH"
    fi

    if glfw3_ok; then
        ok "GLFW3 found via pkg-config after path fix ($(pkg-config --modversion glfw3))"
    else
        warn "libglfw3-dev pkg-config not usable — building GLFW3 from source (~2 min)"

        # Detect available display backends.
        # Pi OS Bookworm 64-bit defaults to Wayland (Labwc/Wayfire); build both
        # X11 and Wayland backends so the binary runs under either compositor.
        _GLFW_X11=ON
        _GLFW_WAYLAND=OFF

        if pkg-config --exists wayland-client xkbcommon 2>/dev/null; then
            _GLFW_WAYLAND=ON
            info "Wayland headers found — building GLFW with Wayland + X11 backends"
        else
            sudo apt-get install -y libwayland-dev libxkbcommon-dev wayland-protocols \
                2>/dev/null \
                && { _GLFW_WAYLAND=ON
                     info "Wayland dev packages installed — building with Wayland + X11 backends"; } \
                || info "Wayland dev packages unavailable — building X11 backend only"
        fi

        sudo apt-get install -y libx11-dev libxrandr-dev libxinerama-dev \
                                libxcursor-dev libxi-dev libxext-dev

        GLFW_TMP=$(mktemp -d /tmp/glfw.XXXXXX)
        git clone --depth 1 --branch 3.4 \
            https://github.com/glfw/glfw.git "${GLFW_TMP}"

        cmake -S "${GLFW_TMP}" -B "${GLFW_TMP}/build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_SHARED_LIBS=ON \
            -DGLFW_BUILD_X11="${_GLFW_X11}" \
            -DGLFW_BUILD_WAYLAND="${_GLFW_WAYLAND}" \
            -DGLFW_BUILD_EXAMPLES=OFF \
            -DGLFW_BUILD_TESTS=OFF \
            -DGLFW_BUILD_DOCS=OFF \
            -GNinja -Wno-dev
        ninja -C "${GLFW_TMP}/build"
        sudo ninja -C "${GLFW_TMP}/build" install
        sudo ldconfig
        rm -rf "${GLFW_TMP}"

        # cmake's default install prefix is /usr/local; ensure pkg-config can find it.
        export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

        if glfw3_ok; then
            ok "GLFW3 built and installed ($(PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}" pkg-config --modversion glfw3))"
        else
            fatal "GLFW3 installation failed — cannot continue"
        fi
    fi
fi

# ── RP2350 helmet audio USB device check ─────────────────────────────────────
section "3 / 11  RP2350 helmet audio (USB)"

# The RP2350 presents itself as a USB Audio Class 2.0 stereo device.
# ALSA enumerates it automatically when connected — no overlay or driver needed.
# Verify the card name after connecting the RP2350:
#   arecord -l | grep -i helmet

if arecord -l 2>/dev/null | grep -qi "helmet\|HelmetAudio"; then
    ok "RP2350 USB audio device detected"
    arecord -l 2>/dev/null | grep -i "helmet\|HelmetAudio" | while read -r line; do
        info "  ${line}"
    done
else
    warn "RP2350 not detected — connect it via USB before running ProtoHUD"
    info "Expected card name: 'Helmet Audio 6-Mic' (VID:PID 1209:b350)"
    info "Verify with: arecord -l"
fi

# ── Boot config ───────────────────────────────────────────────────────────────
section "4 / 11  Boot configuration (${BOOT_CFG})"

boot_has() { grep -qE "^[[:space:]]*$1" "${BOOT_CFG}" 2>/dev/null; }
boot_append() {
    local line="$1"
    if ! boot_has "${line%%=*}"; then
        echo "${line}" | sudo tee -a "${BOOT_CFG}" >/dev/null
        info "Added: ${line}"
        NEEDS_REBOOT=true
    else
        info "Already set: ${line}"
    fi
}

# GPU memory split — 256 MB for the compositor / EGL
boot_append "gpu_mem=256"

# Enable camera auto-detection (required for CSI cameras on all Pi OS variants)
boot_append "camera_auto_detect=1"

ok "Boot config updated: ${BOOT_CFG}"

# ── udev rules ────────────────────────────────────────────────────────────────
section "5 / 11  udev rules"

UDEV_RULES="/etc/udev/rules.d/99-protohud.rules"

sudo tee "${UDEV_RULES}" >/dev/null << 'EOF'
# ── ProtoHUD device rules ──────────────────────────────────────────────────
# Creates stable /dev/teensy, /dev/smartknob, /dev/lora symlinks regardless
# of enumeration order.  Reload with:
#   sudo udevadm control --reload-rules && sudo udevadm trigger

# RP2350 Helmet Audio Processor — USB Audio Class 2.0 (pid.codes VID 0x1209)
# Sets ALSA card name to "HelmetAudio" for stable device string in config.json.
SUBSYSTEM=="sound", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="b350", \
    ATTR{id}="HelmetAudio"

# Teensy 4.1 — PJRC VID 0x16C0, serial CDC ACM
SUBSYSTEM=="tty", ATTRS{idVendor}=="16c0", ATTRS{idProduct}=="0483", \
    SYMLINK+="teensy", MODE="0660", GROUP="dialout"

# SmartKnob — ESP32-S3 native USB (Espressif VID 0x303a)
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", \
    SYMLINK+="smartknob", MODE="0660", GROUP="dialout"

# SmartKnob — alternative: CP2102 USB-UART (Silicon Labs VID 0x10c4)
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", \
    ENV{ID_USB_INTERFACE_NUM}=="00", \
    SYMLINK+="smartknob", MODE="0660", GROUP="dialout"

# RAK4631 LoRa module — Nordic nRF52840 (Nordic VID 0x239a)
SUBSYSTEM=="tty", ATTRS{idVendor}=="239a", \
    SYMLINK+="lora", MODE="0660", GROUP="dialout"

# RAK4631 LoRa — alternative: WCH CH340 (common clone boards)
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", \
    SYMLINK+="lora", MODE="0660", GROUP="dialout"

# USB cameras — readable by video group
SUBSYSTEM=="video4linux", MODE="0660", GROUP="video"

# GPIO — readable by gpio group (libgpiod)
SUBSYSTEM=="gpio", MODE="0660", GROUP="gpio"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
ok "udev rules written to ${UDEV_RULES}"

# Warn if the config.json still uses hard-coded ttyACMx paths
warn "config.json uses hard-coded paths (/dev/ttyACM0 etc.)."
warn "After first boot with devices connected, check ls -la /dev/teensy /dev/smartknob /dev/lora"
warn "and update config/config.json to use those symlinks for stable enumeration."

# ── Android mirror — v4l2loopback ────────────────────────────────────────────
section "6 / 11  Android mirror (v4l2loopback)"

V4L2_CONF="/etc/modprobe.d/v4l2loopback.conf"
MODULES_FILE="/etc/modules"

if ! dpkg -s v4l2loopback-dkms &>/dev/null 2>&1; then
    warn "v4l2loopback-dkms not installed — skipping Android mirror setup"
else
    # Write persistent modprobe options (video_nr=4 → /dev/video4, exclusive_caps
    # forces V4L2 output-only mode so scrcpy's --v4l2-sink works correctly).
    # Install scrcpy separately: https://github.com/Genymobile/scrcpy
    if [[ ! -f "${V4L2_CONF}" ]] || ! grep -q "v4l2loopback" "${V4L2_CONF}" 2>/dev/null; then
        sudo tee "${V4L2_CONF}" >/dev/null << 'EOF'
# v4l2loopback — virtual V4L2 device for Android mirror (ProtoHUD / scrcpy)
# Creates /dev/video4. exclusive_caps=1 is required for scrcpy --v4l2-sink.
options v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1
EOF
        info "Written ${V4L2_CONF}"
    else
        info "${V4L2_CONF} already configured"
    fi

    # Ensure module loads on every boot
    if ! grep -q "^v4l2loopback" "${MODULES_FILE}" 2>/dev/null; then
        echo "v4l2loopback" | sudo tee -a "${MODULES_FILE}" >/dev/null
        info "Added v4l2loopback to ${MODULES_FILE}"
        NEEDS_REBOOT=true
    fi

    # Load it now for immediate use (suppress error — DKMS module may need reboot)
    if ! lsmod | grep -q "^v4l2loopback"; then
        sudo modprobe v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1 \
            && ok  "v4l2loopback loaded: /dev/video4 is ready" \
            || warn "modprobe v4l2loopback failed — /dev/video4 will appear after reboot"
    else
        ok "v4l2loopback already loaded"
    fi

    ok "Android mirror configured"
    info "Usage: connect phone via USB, enable USB debugging, then use"
    info "  Menu → Android Mirror → Start Mirror → Show Overlay"
    info "Enable in config.json: set \"android\".\"enabled\" = true for auto-start."
fi

# ── User groups ───────────────────────────────────────────────────────────────
section "7 / 11  User groups"

GROUPS_NEEDED=(gpio dialout video render audio input)
GROUPS_ADDED=()

# Ensure gpio group exists (may not on all distros)
if ! getent group gpio &>/dev/null; then
    sudo groupadd --system gpio
    info "Created system group: gpio"
fi

for grp in "${GROUPS_NEEDED[@]}"; do
    if getent group "${grp}" &>/dev/null; then
        if ! id -nG "${REAL_USER}" | grep -qw "${grp}"; then
            sudo usermod -aG "${grp}" "${REAL_USER}"
            GROUPS_ADDED+=("${grp}")
            NEEDS_REBOOT=true
        fi
    else
        warn "Group '${grp}' does not exist — skipping"
    fi
done

if [[ ${#GROUPS_ADDED[@]} -gt 0 ]]; then
    ok "Added ${REAL_USER} to groups: ${GROUPS_ADDED[*]}"
else
    ok "User ${REAL_USER} already in all required groups"
fi

# ── Build ProtoHUD ────────────────────────────────────────────────────────────
section "8 / 11  Build ProtoHUD"

mkdir -p "${BUILD_DIR}"

info "Configuring with CMake..."
# Ensure multiarch pkg-config paths are visible to CMake
export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -GNinja \
    -Wno-dev

info "Building ($(nproc) cores)..."
ninja -C "${BUILD_DIR}" -j"$(nproc)"

ok "Build complete: ${BUILD_DIR}/protohud"

# Convenience symlink so that ./protohud works from the project root
ln -sf "${BUILD_DIR}/protohud" "${PROJECT_ROOT}/protohud"
ok "Symlink created: ${PROJECT_ROOT}/protohud → build/protohud"

# ── VITURE XR SDK libraries ───────────────────────────────────────────────────
section "9 / 11  VITURE XR SDK libraries"

VITURE_LIB_SRC="${PROJECT_ROOT}/vendor/viture/lib/aarch64"

if [[ -d "${VITURE_LIB_SRC}" ]]; then
    VITURE_CHANGED=false

    for lib in libglasses.so libcarina_vio.so; do
        if [[ ! -f "/usr/local/lib/${lib}" ]] \
            || ! cmp -s "${VITURE_LIB_SRC}/${lib}" "/usr/local/lib/${lib}"; then
            sudo install -m 755 "${VITURE_LIB_SRC}/${lib}" /usr/local/lib/
            info "Installed: ${lib}"
            VITURE_CHANGED=true
        else
            info "Already up-to-date: ${lib}"
        fi
    done

    if ${VITURE_CHANGED}; then
        sudo ldconfig
        ok "VITURE SDK libraries installed to /usr/local/lib"
    else
        ok "VITURE SDK libraries already up-to-date"
    fi
else
    warn "VITURE SDK not found at ${VITURE_LIB_SRC} — skipping"
fi

# ── Systemd service ───────────────────────────────────────────────────────────
section "10 / 11  Systemd service (optional)"

SERVICE_FILE="/etc/systemd/system/protohud.service"

if [[ -f "${SERVICE_FILE}" ]]; then
    ok "Service already installed: ${SERVICE_FILE}"
else
    sudo tee "${SERVICE_FILE}" >/dev/null << EOF
[Unit]
Description=ProtoHUD XR heads-up display
After=graphical.target local-fs.target sound.target
Wants=graphical.target

[Service]
Type=simple
User=${REAL_USER}
WorkingDirectory=${BUILD_DIR}
ExecStart=${BUILD_DIR}/protohud ${PROJECT_ROOT}/config/config.json
Restart=on-failure
RestartSec=5s

Environment=DISPLAY=:0
Environment=XAUTHORITY=${REAL_HOME}/.Xauthority

SupplementaryGroups=video render gpio dialout audio input

ExecStartPre=/bin/sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor || true'
ExecStopPost=/bin/sh -c 'echo ondemand  | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor || true'

StandardOutput=journal
StandardError=journal

[Install]
WantedBy=graphical.target
EOF

    sudo systemctl daemon-reload
    ok "Service installed: ${SERVICE_FILE}"
    info "Enable auto-start with:  sudo systemctl enable protohud"
    info "Start now with:          sudo systemctl start protohud"
fi

# ── CPU performance governor ──────────────────────────────────────────────────
# Apply immediately; also persisted via the service ExecStartPre above.
if [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null
    ok "CPU governor set to performance"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
section "11 / 11  Summary"

echo ""
echo -e "${BOLD}ProtoHUD build location:${RESET}  ${BUILD_DIR}/protohud"
echo -e "${BOLD}Config file:${RESET}              ${PROJECT_ROOT}/config/config.json"
echo -e "${BOLD}Boot config:${RESET}              ${BOOT_CFG}"
echo ""

echo -e "${BOLD}Quick-start (3 ways to run):${RESET}"
echo ""
echo "  1. From the project root (symlink installed by this script):"
echo "     cd ${PROJECT_ROOT}"
echo "     ./protohud"
echo ""
echo "  2. From the build directory directly:"
echo "     cd ${BUILD_DIR}"
echo "     ./protohud"
echo ""
echo "  3. Via the run script (checks groups, handles config path):"
echo "     ${PROJECT_ROOT}/scripts/run.sh"
echo ""
echo "  — or via systemd to auto-start on boot —"
echo "  sudo systemctl enable --now protohud"
echo "  journalctl -u protohud -f"
echo ""

if ${NEEDS_REBOOT}; then
    echo -e "${YELLOW}${BOLD}┌─────────────────────────────────────────────────────┐${RESET}"
    echo -e "${YELLOW}${BOLD}│  REBOOT REQUIRED to apply:                          │${RESET}"
    if [[ ${#GROUPS_ADDED[@]} -gt 0 ]]; then
        echo -e "${YELLOW}${BOLD}│   • group membership: ${GROUPS_ADDED[*]}         │${RESET}"
    fi
    echo -e "${YELLOW}${BOLD}│   • boot config changes (overlays / gpu_mem)        │${RESET}"
    echo -e "${YELLOW}${BOLD}└─────────────────────────────────────────────────────┘${RESET}"
    echo ""
    echo -e "${YELLOW}Run:  sudo reboot${RESET}"
else
    ok "No reboot required."
fi

echo ""
echo -e "${GREEN}${BOLD}Installation complete.${RESET}"
