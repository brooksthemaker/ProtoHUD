#!/usr/bin/env bash
# ProtoHUD restart helper — clean slate, then (re)start.
#
# Kills any running ProtoHUD instance and every helper process / service it
# spawns, then launches a fresh instance. Safe to run repeatedly (idempotent)
# and from a non-interactive context such as a GPIO-button handler.
#
# Usage:
#   scripts/restart.sh                 # stop everything, then start (direct)
#   scripts/restart.sh --service       # ... start via systemd instead
#   scripts/restart.sh --stop          # stop everything, don't start
#   scripts/restart.sh [config.json]   # direct launch with a specific config
#
# Run as root (e.g. from a GPIO handler), or as a user that owns the ProtoHUD
# processes. systemd control uses `sudo -n systemctl restart protohud.service`,
# which scripts/install_sudoers.sh already grants without a password.

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/protohud"
SERVICE="protohud.service"
LOG="/tmp/protohud.log"

# sudo wrapper: empty when already root, non-interactive (-n) otherwise so a
# button handler never blocks on a password prompt.
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo -n"; fi

log() { echo "[protohud-restart] $*"; }

have_service() {
    command -v systemctl >/dev/null 2>&1 &&
    systemctl list-unit-files 2>/dev/null | grep -q "^${SERVICE}"
}

stop_all() {
    # 1. Stop the systemd-managed instance first so its Restart=on-failure
    #    policy can't immediately respawn the binary we're about to kill.
    if have_service; then
        log "stopping ${SERVICE}"
        $SUDO systemctl stop "${SERVICE}" 2>/dev/null || true
    fi

    # 2. Kill the watchdog supervisor and any direct binary instances. `-x`
    #    matches the process name exactly (the binary), so this script and the
    #    pkill calls themselves are never matched.
    log "killing watchdog + protohud instances"
    $SUDO pkill -f 'scripts/watchdog.sh' 2>/dev/null || true
    $SUDO pkill -x protohud              2>/dev/null || true

    # 3. Kill the helper processes ProtoHUD spawns. panel_driver.py blanks the
    #    HUB75 panels from its SIGTERM handler before exiting.
    log "killing helper processes (panel_driver / scheduler / protoface daemon)"
    $SUDO pkill -f panel_driver.py  2>/dev/null || true
    $SUDO pkill -f scheduler_daemon 2>/dev/null || true
    $SUDO pkill -f 'scheduler\.py'  2>/dev/null || true
    $SUDO pkill -f 'run\.py'        2>/dev/null || true

    # 4. Give them a moment to release the display / GPIO / SPI / shm, then
    #    SIGKILL anything that ignored the polite signal.
    sleep 1
    $SUDO pkill -9 -x protohud             2>/dev/null || true
    $SUDO pkill -9 -f 'scripts/watchdog.sh' 2>/dev/null || true
    $SUDO pkill -9 -f panel_driver.py      2>/dev/null || true
}

start() {
    local cfg="${1:-${ROOT}/config/config.json}"

    if [ "${USE_SERVICE}" = "1" ]; then
        if have_service; then
            log "starting via systemd (${SERVICE})"
            $SUDO systemctl start "${SERVICE}"
            return
        fi
        log "WARNING: ${SERVICE} not installed — falling back to direct launch"
    fi

    if [ ! -x "${BIN}" ]; then
        log "ERROR: binary not found at ${BIN} — build with scripts/build.sh"
        exit 1
    fi

    # Detach so a GPIO handler returns immediately. Supervise via the watchdog
    # (auto-restart on crash) when present, else launch the binary directly.
    local launcher="${BIN}"
    if [ -x "${ROOT}/scripts/watchdog.sh" ]; then
        launcher="${ROOT}/scripts/watchdog.sh"
    fi
    log "starting ${launcher} ${cfg}"
    setsid "${launcher}" "${cfg}" >"${LOG}" 2>&1 </dev/null &
    log "launched (pid $!), logging to ${LOG}"
}

USE_SERVICE=0
DO_START=1
CONFIG=""
for arg in "$@"; do
    case "${arg}" in
        --service)   USE_SERVICE=1 ;;
        --stop)      DO_START=0 ;;
        -h|--help)   sed -n '2,16p' "$0"; exit 0 ;;
        -*)          log "unknown option: ${arg}"; exit 2 ;;
        *)           CONFIG="${arg}" ;;
    esac
done

stop_all
[ "${DO_START}" = "1" ] && start "${CONFIG}"
log "done"
