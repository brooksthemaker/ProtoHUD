#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
#  ProtoHUD Uninstaller — reverses scripts/install.sh
#  Run from the project root:
#    chmod +x scripts/uninstall.sh && ./scripts/uninstall.sh
#
#  Default (safe) mode removes only ProtoHUD-specific artifacts:
#    • systemd service + DBus drop-in (stopped & disabled first)
#    • /etc/udev/rules.d/99-protohud.rules
#    • /etc/sudoers.d/protohud
#    • /etc/modprobe.d/v4l2loopback.conf  + the /etc/modules line
#    • VITURE SDK libs in /usr/local/lib (only if they match this repo's copies)
#    • the ./protohud convenience symlink and build/ directory
#    • resets the CPU governor to ondemand
#
#  It deliberately does NOT touch shared/system settings by default — boot
#  config lines (gpu_mem, camera_auto_detect, i2c_arm), apt packages, group
#  membership, and user-session lingering are left alone because other software
#  may rely on them. Pass --purge to also remove those, and the script will
#  prompt before each. Pass --dry-run to print what would happen without
#  changing anything. --yes skips the final confirmation.
# ══════════════════════════════════════════════════════════════════════════════
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
info()    { echo -e "${CYAN}[info]${RESET}  $*"; }
ok()      { echo -e "${GREEN}[ ok ]${RESET}  $*"; }
warn()    { echo -e "${YELLOW}[warn]${RESET}  $*"; }
fatal()   { echo -e "${RED}[FAIL]${RESET}  $*" >&2; exit 1; }
section() { echo -e "\n${BOLD}══ $* ══${RESET}"; }

# ── Options ───────────────────────────────────────────────────────────────────
PURGE=false; DRY_RUN=false; ASSUME_YES=false
for arg in "$@"; do
    case "${arg}" in
        --purge)   PURGE=true ;;
        --dry-run) DRY_RUN=true ;;
        --yes|-y)  ASSUME_YES=true ;;
        -h|--help)
            sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) fatal "Unknown option: ${arg} (try --help)" ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
REAL_USER="${SUDO_USER:-${USER:-$(whoami)}}"

# run / would-run a command, honouring --dry-run
run() {
    if ${DRY_RUN}; then echo -e "${YELLOW}[dry]${RESET}   $*"; else eval "$@"; fi
}
# yes/no prompt (auto-yes under --yes; never prompts under --dry-run)
confirm() {
    ${DRY_RUN} && return 0
    ${ASSUME_YES} && return 0
    local reply
    read -r -p "$(echo -e "${YELLOW}?${RESET} $1 [y/N] ")" reply
    [[ "${reply}" =~ ^[Yy]$ ]]
}

if ! command -v sudo &>/dev/null; then fatal "sudo is required"; fi

section "ProtoHUD uninstall"
info "Project root : ${PROJECT_ROOT}"
info "User         : ${REAL_USER}"
${DRY_RUN} && warn "DRY RUN — no changes will be made"
${PURGE}   && warn "PURGE mode — will also offer to revert shared system settings"
echo ""
if ! ${DRY_RUN} && ! ${ASSUME_YES}; then
    confirm "Proceed with uninstall?" || { info "Aborted."; exit 0; }
fi
run "sudo -v"

# ── Systemd service + drop-in ─────────────────────────────────────────────────
section "Systemd service"
SERVICE_FILE="/etc/systemd/system/protohud.service"
OVERRIDE_DIR="/etc/systemd/system/protohud.service.d"
if systemctl list-unit-files 2>/dev/null | grep -q '^protohud\.service'; then
    run "sudo systemctl stop protohud.service 2>/dev/null || true"
    run "sudo systemctl disable protohud.service 2>/dev/null || true"
    ok "Service stopped & disabled"
else
    info "No protohud.service registered"
fi
[[ -f "${SERVICE_FILE}" ]] && { run "sudo rm -f '${SERVICE_FILE}'"; ok "Removed ${SERVICE_FILE}"; } \
                           || info "No service unit file"
[[ -d "${OVERRIDE_DIR}" ]] && { run "sudo rm -rf '${OVERRIDE_DIR}'"; ok "Removed ${OVERRIDE_DIR}"; } \
                           || info "No service drop-in dir"
run "sudo systemctl daemon-reload 2>/dev/null || true"

# ── udev rules ────────────────────────────────────────────────────────────────
section "udev rules"
UDEV_RULES="/etc/udev/rules.d/99-protohud.rules"
if [[ -f "${UDEV_RULES}" ]]; then
    run "sudo rm -f '${UDEV_RULES}'"
    run "sudo udevadm control --reload-rules 2>/dev/null || true"
    run "sudo udevadm trigger 2>/dev/null || true"
    ok "Removed ${UDEV_RULES} and reloaded"
else
    info "No udev rules file"
fi

# ── sudoers ───────────────────────────────────────────────────────────────────
section "sudoers rule"
SUDOERS="/etc/sudoers.d/protohud"
[[ -f "${SUDOERS}" ]] && { run "sudo rm -f '${SUDOERS}'"; ok "Removed ${SUDOERS}"; } \
                      || info "No sudoers rule"

# ── v4l2loopback (Android mirror) ─────────────────────────────────────────────
section "Android mirror (v4l2loopback)"
V4L2_CONF="/etc/modprobe.d/v4l2loopback.conf"
MODULES_FILE="/etc/modules"
if [[ -f "${V4L2_CONF}" ]] && grep -q "ProtoHUD\|AndroidMirror" "${V4L2_CONF}" 2>/dev/null; then
    run "sudo rm -f '${V4L2_CONF}'"; ok "Removed ${V4L2_CONF}"
else
    info "No ProtoHUD v4l2loopback config"
fi
if grep -q "^v4l2loopback" "${MODULES_FILE}" 2>/dev/null; then
    run "sudo sed -i '/^v4l2loopback$/d' '${MODULES_FILE}'"; ok "Removed v4l2loopback from ${MODULES_FILE}"
fi
if lsmod 2>/dev/null | grep -q "^v4l2loopback"; then
    run "sudo modprobe -r v4l2loopback 2>/dev/null || true"
    info "Unloaded v4l2loopback (the v4l2loopback-dkms package is left installed)"
fi

# ── VITURE SDK libraries ──────────────────────────────────────────────────────
section "VITURE SDK libraries"
VITURE_LIB_SRC="${PROJECT_ROOT}/vendor/viture/lib/aarch64"
removed_any=false
for lib in libglasses.so libcarina_vio.so; do
    dst="/usr/local/lib/${lib}"
    if [[ -f "${dst}" ]]; then
        # Only remove a copy that matches this repo's (don't clobber another
        # app's lib of the same name).
        if [[ -f "${VITURE_LIB_SRC}/${lib}" ]] && cmp -s "${VITURE_LIB_SRC}/${lib}" "${dst}"; then
            run "sudo rm -f '${dst}'"; ok "Removed ${dst}"; removed_any=true
        else
            warn "${dst} differs from this repo's copy — leaving it (remove by hand if it's ours)"
        fi
    fi
done
${removed_any} && run "sudo ldconfig"

# ── Build artifacts + symlink ─────────────────────────────────────────────────
section "Build artifacts"
SYMLINK="${PROJECT_ROOT}/protohud"
[[ -L "${SYMLINK}" ]] && { run "rm -f '${SYMLINK}'"; ok "Removed symlink ${SYMLINK}"; } \
                      || info "No ./protohud symlink"
if [[ -d "${BUILD_DIR}" ]]; then
    if confirm "Delete the build directory (${BUILD_DIR})?"; then
        run "rm -rf '${BUILD_DIR}'"; ok "Removed ${BUILD_DIR}"
    else
        info "Kept ${BUILD_DIR}"
    fi
fi

# ── CPU governor ──────────────────────────────────────────────────────────────
section "CPU governor"
if [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    run "echo ondemand | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1 || true"
    ok "CPU governor reset to ondemand"
fi

# ── Optional: shared system settings (--purge only) ───────────────────────────
if ${PURGE}; then
    section "Purge: shared system settings"
    BOOT_CFG=""
    [[ -f /boot/firmware/config.txt ]] && BOOT_CFG="/boot/firmware/config.txt"
    [[ -z "${BOOT_CFG}" && -f /boot/config.txt ]] && BOOT_CFG="/boot/config.txt"
    if [[ -n "${BOOT_CFG}" ]]; then
        warn "These boot lines may be shared with other software / the desktop:"
        warn "  gpu_mem=256, camera_auto_detect=1, dtparam=i2c_arm=on"
        if confirm "Remove ProtoHUD's boot-config lines from ${BOOT_CFG}?"; then
            for key in "gpu_mem=256" "camera_auto_detect=1" "dtparam=i2c_arm=on"; do
                run "sudo sed -i '\#^${key}\$#d' '${BOOT_CFG}'"
            done
            ok "Removed boot-config lines (reboot to apply)"
        else
            info "Kept boot-config lines"
        fi
    fi
    if command -v loginctl &>/dev/null && \
       loginctl show-user "${REAL_USER}" --property=Linger 2>/dev/null | grep -q "Linger=yes"; then
        if confirm "Disable user-session lingering for ${REAL_USER}?"; then
            run "sudo loginctl disable-linger '${REAL_USER}' 2>/dev/null || true"
            ok "Disabled lingering"
        fi
    fi
    warn "Group membership (gpio/dialout/video/render/audio/input) is left intact."
    warn "Remove by hand if wanted, e.g.:  sudo gpasswd -d ${REAL_USER} gpio"
    warn "apt packages are left installed. Remove the ProtoHUD-only ones with:"
    warn "  sudo apt-get autoremove --purge v4l2loopback-dkms"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
section "Done"
${DRY_RUN} && { warn "Dry run complete — nothing was changed."; exit 0; }
ok "ProtoHUD uninstalled."
info "The source tree itself is untouched — delete it with: rm -rf '${PROJECT_ROOT}'"
${PURGE} || info "Shared system settings (boot config, groups, packages) were left alone. Re-run with --purge to review them."
echo ""
