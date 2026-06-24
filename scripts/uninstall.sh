#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
#  ProtoHUD Uninstaller — reverses scripts/install.sh
#  Supported: Raspberry Pi OS Bookworm · Debian Trixie + RPT packages
#  Run from the project root:
#    chmod +x scripts/uninstall.sh && ./scripts/uninstall.sh
#
#  By default this removes ProtoHUD's SYSTEM integration but leaves shared
#  things (apt packages, group membership, boot config, your config.json and
#  saved faces) untouched, since other software on the Pi may rely on them.
#
#  What it removes by default (in order):
#    1. Systemd      — stop + disable protohud.service, remove unit + drop-in
#    2. CPU governor — reset to ondemand (install.sh forced performance)
#    3. udev         — remove /etc/udev/rules.d/99-protohud.rules, reload
#    4. Android      — remove v4l2loopback.conf + /etc/modules line, unload mod
#    5. VITURE       — remove libglasses.so / libcarina_vio.so from /usr/local/lib
#    6. sudoers      — remove /etc/sudoers.d/protohud (from install_sudoers.sh)
#    7. Lingering    — disable user-session lingering (re-enabled by install.sh)
#    8. Symlink      — remove the ./protohud convenience symlink
#
#  Opt-in destructive extras (off unless you pass the flag):
#    --boot     Revert the lines install.sh added to the boot config
#               (gpu_mem, camera_auto_detect, dtparam=i2c_arm=on)
#    --groups   Remove ${REAL_USER} from gpio/dialout/video/render/audio/input
#    --build    Delete the build/ directory
#    --purge    Everything above PLUS delete state/ (rollback markers + config
#               backups) and config/config.json (your live settings). Implies
#               --boot --groups --build.
#    --packages Print the apt packages install.sh added (does NOT remove them;
#               removal is left to you since they are system-shared).
#
#  Other flags:
#    --yes / -y  Skip the confirmation prompt (non-interactive).
#    --dry-run   Show what would happen without changing anything.
#    -h/--help   This help.
# ══════════════════════════════════════════════════════════════════════════════
set -uo pipefail

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}[info]${RESET}  $*"; }
ok()      { echo -e "${GREEN}[ ok ]${RESET}  $*"; }
warn()    { echo -e "${YELLOW}[warn]${RESET}  $*"; }
fatal()   { echo -e "${RED}[FAIL]${RESET}  $*" >&2; exit 1; }
section() { echo -e "\n${BOLD}══ $* ══${RESET}"; }

# ── Args ──────────────────────────────────────────────────────────────────────
DO_BOOT=0; DO_GROUPS=0; DO_BUILD=0; DO_PURGE=0; DO_PACKAGES=0
ASSUME_YES=0; DRY_RUN=0

for arg in "$@"; do
    case "${arg}" in
        --boot)     DO_BOOT=1 ;;
        --groups)   DO_GROUPS=1 ;;
        --build)    DO_BUILD=1 ;;
        --purge)    DO_PURGE=1; DO_BOOT=1; DO_GROUPS=1; DO_BUILD=1 ;;
        --packages) DO_PACKAGES=1 ;;
        -y|--yes)   ASSUME_YES=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        -h|--help)  sed -n '2,37p' "$0"; exit 0 ;;
        *)          fatal "unknown option: ${arg}  (try --help)" ;;
    esac
done

# run a privileged command, honouring --dry-run
run() {
    if [[ "${DRY_RUN}" == "1" ]]; then
        echo -e "  ${YELLOW}(dry-run)${RESET} $*"
    else
        "$@"
    fi
}

# ── Locate project root ───────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

REAL_USER="${SUDO_USER:-${USER:-$(whoami)}}"

section "ProtoHUD uninstall"
info "Project root: ${PROJECT_ROOT}"
info "User:         ${REAL_USER}"
[[ "${DRY_RUN}" == "1" ]] && warn "DRY RUN — nothing will actually be changed"

echo ""
echo "This will remove ProtoHUD's system integration:"
echo "  • systemd service + DBus drop-in"
echo "  • udev rules (/etc/udev/rules.d/99-protohud.rules)"
echo "  • v4l2loopback config (/etc/modprobe.d/v4l2loopback.conf, /etc/modules)"
echo "  • VITURE SDK libraries (/usr/local/lib/libglasses.so, libcarina_vio.so)"
echo "  • sudoers rule (/etc/sudoers.d/protohud)"
echo "  • the ./protohud symlink and CPU-governor / lingering tweaks"
echo ""
[[ "${DO_BOOT}"   == "1" ]] && echo -e "  ${YELLOW}--boot${RESET}    will also revert boot-config lines (gpu_mem, camera_auto_detect, i2c)"
[[ "${DO_GROUPS}" == "1" ]] && echo -e "  ${YELLOW}--groups${RESET}  will also remove ${REAL_USER} from gpio/dialout/video/render/audio/input"
[[ "${DO_BUILD}"  == "1" ]] && echo -e "  ${YELLOW}--build${RESET}   will also delete ${BUILD_DIR}"
[[ "${DO_PURGE}"  == "1" ]] && echo -e "  ${RED}--purge${RESET}   will also delete state/ and config/config.json (live settings!)"
echo ""
echo "It does NOT remove apt packages (shared with the rest of the system)."
echo ""

if [[ "${ASSUME_YES}" != "1" && "${DRY_RUN}" != "1" ]]; then
    read -r -p "Proceed? [y/N] " reply
    case "${reply}" in
        [yY]|[yY][eE][sS]) ;;
        *) info "Aborted — nothing changed."; exit 0 ;;
    esac
fi

# Verify sudo up front (most steps touch /etc or /usr/local).
if [[ "${DRY_RUN}" != "1" ]]; then
    command -v sudo &>/dev/null || fatal "sudo not found"
    sudo -v || fatal "sudo authentication failed"
fi

# ── 1. Systemd service ────────────────────────────────────────────────────────
section "1 / 8  Systemd service"

SERVICE_FILE="/etc/systemd/system/protohud.service"
OVERRIDE_DIR="/etc/systemd/system/protohud.service.d"

if command -v systemctl &>/dev/null; then
    if systemctl list-unit-files 2>/dev/null | grep -q "^protohud.service"; then
        run sudo systemctl stop protohud.service    2>/dev/null || true
        run sudo systemctl disable protohud.service 2>/dev/null || true
        ok "Stopped and disabled protohud.service"
    else
        info "protohud.service not registered — skipping stop/disable"
    fi
fi

REMOVED_UNIT=0
if [[ -f "${SERVICE_FILE}" ]]; then
    run sudo rm -f "${SERVICE_FILE}"; REMOVED_UNIT=1
    ok "Removed ${SERVICE_FILE}"
else
    info "No service unit at ${SERVICE_FILE}"
fi
if [[ -d "${OVERRIDE_DIR}" ]]; then
    run sudo rm -rf "${OVERRIDE_DIR}"; REMOVED_UNIT=1
    ok "Removed ${OVERRIDE_DIR} (DBus session drop-in)"
fi
[[ "${REMOVED_UNIT}" == "1" ]] && run sudo systemctl daemon-reload && ok "systemd reloaded"

# ── 2. CPU governor ───────────────────────────────────────────────────────────
section "2 / 8  CPU governor"
if [[ -d /sys/devices/system/cpu/cpu0/cpufreq ]]; then
    run sudo sh -c 'echo ondemand | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null' \
        && ok "CPU governor reset to ondemand" \
        || warn "Could not reset CPU governor (non-fatal)"
else
    info "No cpufreq governor control on this system — skipping"
fi

# ── 3. udev rules ─────────────────────────────────────────────────────────────
section "3 / 8  udev rules"
UDEV_RULES="/etc/udev/rules.d/99-protohud.rules"
if [[ -f "${UDEV_RULES}" ]]; then
    run sudo rm -f "${UDEV_RULES}"
    run sudo udevadm control --reload-rules 2>/dev/null || true
    run sudo udevadm trigger 2>/dev/null || true
    ok "Removed ${UDEV_RULES} and reloaded udev"
else
    info "No udev rules at ${UDEV_RULES}"
fi

# ── 4. Android mirror (v4l2loopback) ──────────────────────────────────────────
section "4 / 8  Android mirror (v4l2loopback)"
V4L2_CONF="/etc/modprobe.d/v4l2loopback.conf"
MODULES_FILE="/etc/modules"

if [[ -f "${V4L2_CONF}" ]]; then
    # Only delete the file if it's the one we wrote (mentions AndroidMirror).
    if grep -q "AndroidMirror" "${V4L2_CONF}" 2>/dev/null; then
        run sudo rm -f "${V4L2_CONF}"
        ok "Removed ${V4L2_CONF}"
    else
        warn "${V4L2_CONF} exists but wasn't written by ProtoHUD — left in place"
    fi
fi

if grep -qE "^v4l2loopback$" "${MODULES_FILE}" 2>/dev/null; then
    run sudo sed -i '/^v4l2loopback$/d' "${MODULES_FILE}"
    ok "Removed v4l2loopback from ${MODULES_FILE}"
fi

if lsmod 2>/dev/null | grep -q "^v4l2loopback"; then
    run sudo modprobe -r v4l2loopback 2>/dev/null \
        && ok "Unloaded v4l2loopback module" \
        || warn "Could not unload v4l2loopback (in use?) — gone after reboot"
fi

# ── 5. VITURE XR SDK libraries ────────────────────────────────────────────────
section "5 / 8  VITURE XR SDK libraries"
VITURE_REMOVED=0
for lib in libglasses.so libcarina_vio.so; do
    if [[ -f "/usr/local/lib/${lib}" ]]; then
        run sudo rm -f "/usr/local/lib/${lib}"
        info "Removed /usr/local/lib/${lib}"
        VITURE_REMOVED=1
    fi
done
if [[ "${VITURE_REMOVED}" == "1" ]]; then
    run sudo ldconfig
    ok "VITURE SDK libraries removed and ldconfig refreshed"
else
    info "No VITURE SDK libraries in /usr/local/lib"
fi

# ── 6. sudoers rule ───────────────────────────────────────────────────────────
section "6 / 8  sudoers rule"
SUDOERS_FILE="/etc/sudoers.d/protohud"
if [[ -f "${SUDOERS_FILE}" ]]; then
    run sudo rm -f "${SUDOERS_FILE}"
    ok "Removed ${SUDOERS_FILE}"
else
    info "No sudoers rule at ${SUDOERS_FILE} (install_sudoers.sh not run)"
fi

# ── 7. User-session lingering ─────────────────────────────────────────────────
section "7 / 8  User-session lingering"
if command -v loginctl &>/dev/null; then
    if loginctl show-user "${REAL_USER}" --property=Linger 2>/dev/null | grep -q "Linger=yes"; then
        run sudo loginctl disable-linger "${REAL_USER}" 2>/dev/null \
            && ok "Disabled lingering for ${REAL_USER}" \
            || warn "Could not disable lingering (non-fatal)"
    else
        info "Lingering not enabled for ${REAL_USER}"
    fi
fi

# ── 8. Convenience symlink + opt-in extras ────────────────────────────────────
section "8 / 8  Symlink + project files"
if [[ -L "${PROJECT_ROOT}/protohud" ]]; then
    run rm -f "${PROJECT_ROOT}/protohud"
    ok "Removed symlink ${PROJECT_ROOT}/protohud"
else
    info "No ./protohud symlink"
fi

# ── Opt-in: revert boot config ────────────────────────────────────────────────
if [[ "${DO_BOOT}" == "1" ]]; then
    section "Extra: boot config"
    if [[ -f /boot/firmware/config.txt ]]; then
        BOOT_CFG="/boot/firmware/config.txt"
    elif [[ -f /boot/config.txt ]]; then
        BOOT_CFG="/boot/config.txt"
    else
        BOOT_CFG=""
        warn "No boot config found — skipping"
    fi
    if [[ -n "${BOOT_CFG}" ]]; then
        for line in "gpu_mem=256" "camera_auto_detect=1" "dtparam=i2c_arm=on"; do
            if grep -qE "^[[:space:]]*${line}$" "${BOOT_CFG}" 2>/dev/null; then
                run sudo sed -i "\|^[[:space:]]*${line}$|d" "${BOOT_CFG}"
                info "Removed '${line}' from ${BOOT_CFG}"
            fi
        done
        warn "camera_auto_detect / i2c may be wanted by other software — verify ${BOOT_CFG}"
        ok "Boot config reverted (reboot to apply)"
    fi
fi

# ── Opt-in: remove from groups ────────────────────────────────────────────────
if [[ "${DO_GROUPS}" == "1" ]]; then
    section "Extra: user groups"
    for grp in gpio dialout video render audio input; do
        if id -nG "${REAL_USER}" 2>/dev/null | grep -qw "${grp}"; then
            run sudo gpasswd -d "${REAL_USER}" "${grp}" 2>/dev/null \
                && info "Removed ${REAL_USER} from ${grp}" \
                || warn "Could not remove ${REAL_USER} from ${grp}"
        fi
    done
    warn "dialout/video/audio are common groups — removal may affect other apps"
    ok "Group changes apply after the next login/reboot"
fi

# ── Opt-in: build dir ─────────────────────────────────────────────────────────
if [[ "${DO_BUILD}" == "1" ]]; then
    section "Extra: build directory"
    if [[ -d "${BUILD_DIR}" ]]; then
        run rm -rf "${BUILD_DIR}"
        ok "Removed ${BUILD_DIR}"
    else
        info "No build directory at ${BUILD_DIR}"
    fi
fi

# ── Opt-in: purge runtime state + live config ─────────────────────────────────
if [[ "${DO_PURGE}" == "1" ]]; then
    section "Extra: purge runtime state"
    if [[ -d "${PROJECT_ROOT}/state" ]]; then
        run rm -rf "${PROJECT_ROOT}/state"
        ok "Removed ${PROJECT_ROOT}/state (rollback markers + config backups)"
    fi
    if [[ -f "${PROJECT_ROOT}/config/config.json" ]]; then
        run rm -f "${PROJECT_ROOT}/config/config.json"
        ok "Removed config/config.json (live settings)"
        info "config/config.example.json (tracked defaults) is left in place"
    fi
fi

# ── Packages (info only) ──────────────────────────────────────────────────────
if [[ "${DO_PACKAGES}" == "1" ]]; then
    section "apt packages (NOT removed)"
    echo "install.sh added these packages. They are shared with the rest of the"
    echo "system, so this script does not remove them. To remove manually:"
    echo ""
    echo "  sudo apt-get remove --purge \\"
    echo "      cmake ninja-build device-tree-compiler \\"
    echo "      libcamera-dev libopencv-dev nlohmann-json3-dev libgpiod-dev \\"
    echo "      libasound2-dev v4l2loopback-dkms adb i2c-tools libdbus-1-dev \\"
    echo "      kdeconnect libglfw3-dev"
    echo "  sudo apt-get autoremove"
    echo ""
    warn "Review the list before running — several are general-purpose libraries."
fi

# ── Summary ───────────────────────────────────────────────────────────────────
section "Done"
echo ""
if [[ "${DRY_RUN}" == "1" ]]; then
    echo -e "${YELLOW}Dry run complete — no changes were made.${RESET}"
else
    echo -e "${GREEN}${BOLD}ProtoHUD system integration removed.${RESET}"
fi
echo ""
echo "Still present (by design):"
echo "  • the source tree itself — delete it with:  rm -rf ${PROJECT_ROOT}"
[[ "${DO_BUILD}"  != "1" ]] && echo "  • build/        — remove with --build or 'rm -rf ${BUILD_DIR}'"
[[ "${DO_GROUPS}" != "1" ]] && echo "  • group membership — remove with --groups"
[[ "${DO_BOOT}"   != "1" ]] && echo "  • boot config lines — revert with --boot"
echo "  • apt packages  — see --packages for the list"
echo ""
if [[ "${DO_BOOT}" == "1" || "${DO_GROUPS}" == "1" ]]; then
    echo -e "${YELLOW}Reboot to fully apply boot-config / group changes:  sudo reboot${RESET}"
fi
