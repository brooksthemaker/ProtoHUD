#!/bin/bash
# ProtoHUD watchdog — restarts after crash, exits cleanly on normal shutdown.
#
# Usage: scripts/watchdog.sh [args forwarded to ProtoHUD]
#
# Exit code 0 from ProtoHUD = user quit cleanly → watchdog exits.
# Any other exit code (crash, signal kill) → watchdog waits RESTART_DELAY
# seconds and relaunches, up to MAX_RESTARTS times before giving up.
#
# Signals sent to the watchdog are forwarded to the child process:
#   SIGTERM/SIGINT → child is killed cleanly (exit 0), watchdog exits.

BINARY="$(cd "$(dirname "$0")/.." && pwd)/build/ProtoHUD"
MAX_RESTARTS=10
RESTART_DELAY=3

if [ ! -x "$BINARY" ]; then
    echo "[watchdog] ERROR: binary not found or not executable: $BINARY" >&2
    exit 1
fi

child_pid=""

# Forward SIGTERM/SIGINT to child so it can exit cleanly (exit 0 → no restart).
_forward_signal() {
    if [ -n "$child_pid" ]; then
        kill -TERM "$child_pid" 2>/dev/null
    fi
}
trap _forward_signal SIGTERM SIGINT

count=0
while true; do
    echo "[watchdog] Starting ProtoHUD (attempt $((count + 1)))..."
    "$BINARY" "$@" &
    child_pid=$!
    wait "$child_pid"
    exit_code=$?
    child_pid=""

    if [ $exit_code -eq 0 ]; then
        echo "[watchdog] ProtoHUD exited cleanly. Shutting down."
        exit 0
    fi

    count=$((count + 1))
    echo "[watchdog] ProtoHUD exited with code $exit_code (crash $count/$MAX_RESTARTS)."

    if [ $count -ge $MAX_RESTARTS ]; then
        echo "[watchdog] Max restarts reached. Giving up." >&2
        exit 1
    fi

    echo "[watchdog] Restarting in ${RESTART_DELAY}s..."
    sleep "$RESTART_DELAY"
done
