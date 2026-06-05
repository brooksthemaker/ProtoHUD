#!/usr/bin/env bash
# ProtoHUD update helper — pull the latest code and rebuild.
#
# Fetches and fast-forwards a branch, rebuilds the binary, and (optionally)
# restarts the running instance. Repeatable "deploy latest" flow that ties
# together build.sh and restart.sh.
#
# Usage:
#   scripts/update.sh                      # update the CURRENT branch, build
#   scripts/update.sh <branch>             # checkout + update a specific branch
#   scripts/update.sh --restart            # ... then restart ProtoHUD
#   scripts/update.sh <branch> --restart   # both
#   scripts/update.sh --no-build           # pull only, skip the build
#
# Network steps retry with exponential backoff (2s, 4s, 8s, 16s).

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

DO_BUILD=1
DO_RESTART=0
BRANCH=""
for arg in "$@"; do
    case "${arg}" in
        --restart)   DO_RESTART=1 ;;
        --no-build)  DO_BUILD=0 ;;
        -h|--help)   sed -n '2,15p' "$0"; exit 0 ;;
        -*)          echo "[update] unknown option: ${arg}"; exit 2 ;;
        *)           BRANCH="${arg}" ;;
    esac
done

log() { echo "[protohud-update] $*"; }

# Record the current (pre-update) commit + config as the "last known good"
# state, so scripts/rollback.sh (and the in-HUD Updates menu) can return here
# if the new code misbehaves. Runs BEFORE we fetch/checkout/pull so the marker
# always points at the code that was actually running. Best-effort: never abort
# the update if this bookkeeping fails.
record_good() {
    local state_dir="${ROOT}/state/update"
    local backups="${state_dir}/backups"
    mkdir -p "${backups}" 2>/dev/null || return 0

    local commit branch ts
    commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    ts="$(date -u +%Y%m%dT%H%M%SZ)"

    local cfg_backup=""
    if [ -f "${ROOT}/config/config.json" ]; then
        cfg_backup="${backups}/config-${ts}.json"
        cp -f "${ROOT}/config/config.json" "${cfg_backup}" 2>/dev/null || cfg_backup=""
        # Prune to the 10 most recent config backups.
        ls -1t "${backups}"/config-*.json 2>/dev/null | tail -n +11 | \
            while read -r old; do rm -f "${old}"; done
    fi

    {
        echo "LAST_GOOD_COMMIT=${commit}"
        echo "LAST_GOOD_BRANCH=${branch}"
        echo "LAST_GOOD_DATE=${ts}"
        echo "LAST_GOOD_CONFIG=${cfg_backup}"
    } > "${state_dir}/last_good.env" 2>/dev/null || true
    log "recorded rollback point: ${commit:0:9} on ${branch}"
}

# Retry a git network command up to 4 times with backoff.
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

# Resolve the branch to update (current HEAD if none given).
if [ -z "${BRANCH}" ]; then
    BRANCH="$(git rev-parse --abbrev-ref HEAD)"
fi
log "updating branch '${BRANCH}'"

record_good

retry_git fetch origin "${BRANCH}" || exit 1

# Switch to it if we're not already there.
if [ "$(git rev-parse --abbrev-ref HEAD)" != "${BRANCH}" ]; then
    git checkout "${BRANCH}" || { log "ERROR: checkout failed"; exit 1; }
fi

retry_git pull --ff-only origin "${BRANCH}" || exit 1
log "now at $(git rev-parse --short HEAD) — $(git log -1 --pretty=%s)"

if [ "${DO_BUILD}" = "1" ]; then
    log "building"
    "${ROOT}/scripts/build.sh" || { log "ERROR: build failed"; exit 1; }
fi

if [ "${DO_RESTART}" = "1" ]; then
    log "restarting ProtoHUD"
    "${ROOT}/scripts/restart.sh"
fi

log "done"
