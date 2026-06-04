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
