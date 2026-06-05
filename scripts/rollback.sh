#!/usr/bin/env bash
# ProtoHUD rollback / recovery helper — return to a known-good build.
#
# Designed to be runnable OUTSIDE ProtoHUD (over SSH, from a recovery shell,
# or by a panicked human) when a freshly-applied update misbehaves or won't
# boot. It restores the code to the commit recorded before the last update,
# puts back the matching config backup, rebuilds, and (optionally) restarts.
#
# Rollback target is chosen in this order:
#   1. An explicit <commit-or-branch> argument, if given.
#   2. --main / --remote-main : hard-reset to origin/main (last resort).
#   3. The "last known good" commit recorded by scripts/update.sh in
#      state/update/last_good.env (the default).
#   4. If no marker exists, the previous HEAD (git reflog HEAD@{1}).
#
# Usage:
#   scripts/rollback.sh                      # to last-known-good, build only
#   scripts/rollback.sh --restart            # ... then restart ProtoHUD
#   scripts/rollback.sh <commit|branch>      # to a specific ref
#   scripts/rollback.sh --main --restart     # nuke to origin/main, restart
#   scripts/rollback.sh --no-build           # checkout only, skip the build
#   scripts/rollback.sh --no-config          # don't restore the config backup
#   scripts/rollback.sh --list               # show the recorded rollback point
#
# This uses `git reset --hard`, which DISCARDS uncommitted changes in tracked
# files. config/config.json is gitignored, so your live settings are left
# alone (and restored from the pre-update backup unless --no-config).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

STATE_DIR="${ROOT}/state/update"
MARKER="${STATE_DIR}/last_good.env"

DO_BUILD=1
DO_RESTART=0
DO_CONFIG=1
USE_MAIN=0
TARGET=""

log() { echo "[protohud-rollback] $*"; }

for arg in "$@"; do
    case "${arg}" in
        --restart)            DO_RESTART=1 ;;
        --no-build)           DO_BUILD=0 ;;
        --no-config)          DO_CONFIG=0 ;;
        --main|--remote-main) USE_MAIN=1 ;;
        --list)
            if [ -f "${MARKER}" ]; then cat "${MARKER}"; else echo "no rollback point recorded"; fi
            exit 0 ;;
        -h|--help)            sed -n '2,38p' "$0"; exit 0 ;;
        -*)                   log "unknown option: ${arg}"; exit 2 ;;
        *)                    TARGET="${arg}" ;;
    esac
done

# Retry a git network command up to 4 times with backoff (mirrors update.sh).
retry_git() {
    local delay=2 i
    for i in 1 2 3 4; do
        if git "$@"; then return 0; fi
        log "git $1 failed (attempt ${i}/4) — retrying in ${delay}s"
        sleep "${delay}"; delay=$((delay * 2))
    done
    log "ERROR: git $* failed after 4 attempts"
    return 1
}

# Pull config backup path out of the marker (sourced safely below).
GOOD_COMMIT=""; GOOD_BRANCH=""; GOOD_CONFIG=""
if [ -f "${MARKER}" ]; then
    # shellcheck disable=SC1090
    . "${MARKER}" 2>/dev/null || true
    GOOD_COMMIT="${LAST_GOOD_COMMIT:-}"
    GOOD_BRANCH="${LAST_GOOD_BRANCH:-}"
    GOOD_CONFIG="${LAST_GOOD_CONFIG:-}"
fi

# ── Resolve the ref we're rolling back to ────────────────────────────────────
REF=""
if [ -n "${TARGET}" ]; then
    REF="${TARGET}"
    log "rolling back to requested ref: ${REF}"
elif [ "${USE_MAIN}" = "1" ]; then
    retry_git fetch origin main || log "WARNING: fetch failed, using local origin/main"
    REF="origin/main"
    log "rolling back to ${REF} (last resort)"
elif [ -n "${GOOD_COMMIT}" ] && [ "${GOOD_COMMIT}" != "unknown" ]; then
    REF="${GOOD_COMMIT}"
    log "rolling back to last known good: ${REF:0:9} (was ${GOOD_BRANCH:-?})"
else
    # No marker — fall back to the previous HEAD position.
    REF="$(git rev-parse 'HEAD@{1}' 2>/dev/null || true)"
    if [ -z "${REF}" ]; then
        log "ERROR: no rollback point recorded and no previous HEAD in reflog."
        log "       Re-run with an explicit commit, or: scripts/rollback.sh --main"
        exit 1
    fi
    log "no marker found — rolling back to previous HEAD: ${REF:0:9}"
fi

# ── Apply the rollback ───────────────────────────────────────────────────────
if ! git rev-parse --verify --quiet "${REF}^{commit}" >/dev/null; then
    log "ERROR: '${REF}' is not a valid commit/branch in this repo."
    exit 1
fi

log "git reset --hard ${REF}"
git reset --hard "${REF}" || { log "ERROR: reset failed"; exit 1; }

# Bring submodules (Protoface) back in line with the rolled-back tree.
git submodule update --init --recursive 2>/dev/null || \
    log "WARNING: submodule update failed (Protoface may be stale)"

log "now at $(git rev-parse --short HEAD) — $(git log -1 --pretty=%s)"

# ── Restore the config that shipped with the known-good build ─────────────────
if [ "${DO_CONFIG}" = "1" ] && [ -n "${GOOD_CONFIG}" ] && [ -f "${GOOD_CONFIG}" ]; then
    # Stash the current (possibly-broken) config alongside the backups first.
    if [ -f "${ROOT}/config/config.json" ]; then
        cp -f "${ROOT}/config/config.json" \
              "${STATE_DIR}/backups/config-prerollback-$(date -u +%Y%m%dT%H%M%SZ).json" 2>/dev/null || true
    fi
    cp -f "${GOOD_CONFIG}" "${ROOT}/config/config.json" 2>/dev/null && \
        log "restored config from ${GOOD_CONFIG}" || \
        log "WARNING: could not restore config backup"
fi

# ── Rebuild + restart ────────────────────────────────────────────────────────
if [ "${DO_BUILD}" = "1" ]; then
    log "building"
    "${ROOT}/scripts/build.sh" || { log "ERROR: build failed — binary may be stale"; exit 1; }
fi

if [ "${DO_RESTART}" = "1" ]; then
    log "restarting ProtoHUD"
    "${ROOT}/scripts/restart.sh"
fi

log "done"
