#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
#  ProtoHUD post-install health check
#  Supported: Raspberry Pi OS Bookworm · Debian Trixie + RPT packages
#  Run after reboot to verify all hardware and software is ready.
#    chmod +x scripts/check.sh && ./scripts/check.sh
# ══════════════════════════════════════════════════════════════════════════════
set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

pass()  { echo -e "  ${GREEN}[PASS]${RESET}  $*"; }
fail()  { echo -e "  ${RED}[FAIL]${RESET}  $*"; FAIL=1; }
warn()  { echo -e "  ${YELLOW}[WARN]${RESET}  $*"; }
info()  { echo -e "  ${CYAN}      ${RESET}  $*"; }
section(){ echo -e "\n${BOLD}── $* ──${RESET}"; }

FAIL=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

echo -e "${BOLD}Project root:${RESET} ${PROJECT_ROOT}"
echo -e "${BOLD}Build dir:${RESET}    ${BUILD_DIR}"
echo ""

# ── 1. User groups ────────────────────────────────────────────────────────────
section "User groups"
REQUIRED_GROUPS=(gpio dialout video render audio input)
for grp in "${REQUIRED_GROUPS[@]}"; do
    if id -nG "${USER}" | grep -qw "${grp}"; then
        pass "In group: ${grp}"
    else
        fail "NOT in group: ${grp}  →  sudo usermod -aG ${grp} ${USER}  then reboot"
    fi
done

# ── 2. ProtoHUD binary ────────────────────────────────────────────────────────
section "Binary"
if [[ -f "${BUILD_DIR}/protohud" ]]; then
    pass "Binary exists: ${BUILD_DIR}/protohud"
else
    fail "Binary not found — run:  ./scripts/install.sh"
fi

if [[ -L "${PROJECT_ROOT}/protohud" ]]; then
    pass "Root symlink exists: ${PROJECT_ROOT}/protohud"
else
    warn "Root symlink missing — run install.sh or:  ln -sf build/protohud protohud"
fi

# ── 3. VITURE XR SDK libraries ────────────────────────────────────────────────
section "VITURE XR SDK"
for lib in libglasses.so libcarina_vio.so; do
    if [[ -f "/usr/local/lib/${lib}" ]]; then
        pass "${lib} installed in /usr/local/lib"
    elif [[ -f "${BUILD_DIR}/${lib}" ]]; then
        warn "${lib} found in build/ but not /usr/local/lib — run install.sh"
    else
        fail "${lib} not found — run:  ./scripts/install.sh"
    fi
done

# ── 4. RP2350 helmet audio (USB capture) ─────────────────────────────────────
section "RP2350 Helmet Audio (USB)"
if arecord -l 2>/dev/null | grep -qi "helmet\|HelmetAudio\|b350\|1209"; then
    CARD_LINE=$(arecord -l 2>/dev/null | grep -i "helmet\|HelmetAudio\|b350\|1209" | head -1)
    pass "RP2350 detected: ${CARD_LINE}"
else
    warn "RP2350 not detected — audio will be unavailable until connected"
    info "Expected: 'Helmet Audio 6-Mic' (USB VID:PID 1209:b350)"
    info "Connect via USB-C then re-run this check, or set audio.enabled=false in config.json"
    info "List all audio inputs: arecord -l"
fi

# ── 5. Audio output devices ───────────────────────────────────────────────────
section "Audio outputs"

check_output() {
    local label="$1" card="$2"
    if aplay -l 2>/dev/null | grep -qi "${card}"; then
        pass "${label}: $(aplay -l 2>/dev/null | grep -i "${card}" | head -1 | sed 's/^[[:space:]]*//')"
    else
        warn "${label} not detected (connect device or ignore if unused)"
        info "List all outputs: aplay -l"
    fi
}

check_output "VITURE XR Glasses" "VITURE\|VITUREXRGlasses"
check_output "Headphones (3.5mm)" "Headphones\|bcm2835.*Headphones"
check_output "HDMI audio"         "vc4hdmi\|HDMI"

# ── 6. CSI cameras (libcamera) ───────────────────────────────────────────────
section "CSI cameras (OWLsight)"

# IPA modules required for Pi cameras — check all known locations:
#   Bullseye/Pi OS:  /usr/lib/libcamera/
#   Bookworm/Trixie: /usr/lib/aarch64-linux-gnu/libcamera/  (multiarch)
IPA_FOUND=false
for IPA_DIR in /usr/lib/libcamera /usr/lib/aarch64-linux-gnu/libcamera \
               /usr/local/lib/libcamera /usr/local/lib/aarch64-linux-gnu/libcamera; do
    if [[ -d "${IPA_DIR}" ]] && ls "${IPA_DIR}"/*.so* &>/dev/null 2>&1; then
        IPA_FOUND=true
        info "libcamera IPA modules: ${IPA_DIR}"
        break
    fi
done
if ! ${IPA_FOUND}; then
    # libcamera-ipa package installed but path unknown — check via dpkg as fallback
    if dpkg -l libcamera-ipa 2>/dev/null | grep -q "^ii"; then
        IPA_FOUND=true
        info "libcamera IPA modules: installed via libcamera-ipa package"
    else
        warn "libcamera IPA modules not found — install: sudo apt install libcamera-ipa"
    fi
fi

# rpicam-hello (Bookworm/Trixie) or libcamera-hello (Bullseye)
CAM_TOOL=""
if command -v rpicam-hello &>/dev/null; then
    CAM_TOOL="rpicam-hello"
elif command -v libcamera-hello &>/dev/null; then
    CAM_TOOL="libcamera-hello"
fi

if [[ -n "${CAM_TOOL}" ]]; then
    CAM_LIST=$(${CAM_TOOL} --list-cameras 2>&1)
    CAM_COUNT=$(echo "${CAM_LIST}" | grep -c "^\[" 2>/dev/null || true)
    if [[ "${CAM_COUNT}" -ge 2 ]]; then
        pass "${CAM_COUNT} camera(s) found (${CAM_TOOL})"
    elif [[ "${CAM_COUNT}" -eq 1 ]]; then
        warn "Only 1 camera detected (expected 2 OWLsight cameras) — check CSI ribbon cables"
    else
        warn "No libcamera cameras detected — check CSI cables and camera_auto_detect in boot config"
    fi
else
    warn "Neither rpicam-hello nor libcamera-hello found — install: sudo apt install rpicam-apps"
fi

# ── 7. v4l2loopback (Android mirror) ─────────────────────────────────────────
section "v4l2loopback (Android mirror)"
if lsmod 2>/dev/null | grep -q "^v4l2loopback"; then
    DEV=$(ls /dev/video4 2>/dev/null && echo "/dev/video4 ready" || echo "module loaded, /dev/video4 may appear after modprobe")
    pass "v4l2loopback loaded — ${DEV}"
else
    warn "v4l2loopback not loaded"
    info "Load now:  sudo modprobe v4l2loopback video_nr=4 card_label=AndroidMirror exclusive_caps=1"
    info "Auto-load on boot is configured via /etc/modules — may need reboot"
fi

# ── 8. Boot config ────────────────────────────────────────────────────────────
section "Boot config"
for BOOT_CFG in /boot/firmware/config.txt /boot/config.txt; do
    [[ -f "${BOOT_CFG}" ]] || continue
    if grep -qE "^[[:space:]]*gpu_mem=256" "${BOOT_CFG}"; then
        pass "gpu_mem=256 set in ${BOOT_CFG}"
    else
        warn "gpu_mem=256 not found in ${BOOT_CFG} — add:  echo 'gpu_mem=256' | sudo tee -a ${BOOT_CFG}"
    fi
    if grep -qE "^[[:space:]]*camera_auto_detect=1" "${BOOT_CFG}"; then
        pass "camera_auto_detect=1 set"
    else
        warn "camera_auto_detect=1 not found — add:  echo 'camera_auto_detect=1' | sudo tee -a ${BOOT_CFG}"
        info "Then reboot for cameras to be detected"
    fi
    break
done

# ── 9. scrcpy (optional) ──────────────────────────────────────────────────────
section "scrcpy (optional — Android mirror)"
if command -v scrcpy &>/dev/null; then
    pass "scrcpy: $(scrcpy --version 2>&1 | head -1)"
else
    warn "scrcpy not installed — Android mirror unavailable (non-critical)"
    info "Install separately:  sudo apt install scrcpy"
    info "Or build from source: https://github.com/Genymobile/scrcpy"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}══ Result ══${RESET}"
if [[ ${FAIL} -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}All required checks passed — ready to launch ProtoHUD.${RESET}"
    echo ""
    echo "  Run:  ./protohud"
    echo "   or:  ./scripts/run.sh"
else
    echo -e "${RED}${BOLD}One or more checks failed — fix the items marked [FAIL] above, then re-run this script.${RESET}"
fi
echo ""
