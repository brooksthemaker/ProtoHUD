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

BEFORE_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
record_good

retry_git fetch origin "${BRANCH}" || exit 1

# Switch to it if we're not already there.
if [ "$(git rev-parse --abbrev-ref HEAD)" != "${BRANCH}" ]; then
    git checkout "${BRANCH}" || { log "ERROR: checkout failed"; exit 1; }
fi

retry_git pull --ff-only origin "${BRANCH}" || exit 1
AFTER_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
log "now at $(git rev-parse --short HEAD) — $(git log -1 --pretty=%s)"

# Bring the Protoface submodule in line with the new tree. No --force, so any
# user-imported faces (untracked files in the submodule) are left in place.
if [ -f "${ROOT}/.gitmodules" ]; then
    log "updating submodules (Protoface)"
    git submodule update --init --recursive 2>/dev/null || \
        log "WARNING: submodule update failed (Protoface may be stale)"
fi

# Append to the update history log (state/update/history.log), newest line last.
record_history() {
    local hist="${ROOT}/state/update"
    mkdir -p "${hist}" 2>/dev/null || return 0
    local subj; subj="$(git log -1 --pretty=%s 2>/dev/null)"
    printf '%s\t%s\t%s..%s\t%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${BRANCH}" \
        "${BEFORE_COMMIT:0:9}" "${AFTER_COMMIT:0:9}" "${subj}" \
        >> "${hist}/history.log" 2>/dev/null || true
    # Keep the log bounded to the last 200 entries.
    if [ -f "${hist}/history.log" ]; then
        tail -n 200 "${hist}/history.log" > "${hist}/history.log.tmp" 2>/dev/null && \
            mv -f "${hist}/history.log.tmp" "${hist}/history.log" 2>/dev/null || true
    fi
}
record_history

# Merge any new config defaults shipped with this update into the user's
# config.json (existing values always win). See scripts/merge_config.py.
merge_config() {
    local ex="${ROOT}/config/config.example.json"
    local cfg="${ROOT}/config/config.json"
    [ -f "${ex}" ] || return 0
    if command -v python3 >/dev/null 2>&1; then
        python3 "${ROOT}/scripts/merge_config.py" "${cfg}" "${ex}" || \
            log "WARNING: config merge failed (runtime defaults still apply)"
    else
        log "python3 not found — skipping config merge (runtime defaults apply)"
    fi
}

if [ "${DO_BUILD}" = "1" ]; then
    log "building"
    "${ROOT}/scripts/build.sh" || { log "ERROR: build failed"; exit 1; }
fi

if [ "${DO_RESTART}" = "1" ]; then
    # Stop the old instance FIRST so its on-exit config write happens before we
    # merge — otherwise the dying process would clobber the merged config. Then
    # merge defaults into the now-quiescent config and start the new build.
    log "stopping current instance"
    "${ROOT}/scripts/restart.sh" --stop
    log "merging config defaults"
    merge_config
    log "starting updated ProtoHUD"
    "${ROOT}/scripts/restart.sh" --start
else
    # Not restarting: merge best-effort now. The running instance will rewrite
    # config.json on its own next exit, but ProtoHUD's loader re-applies any
    # missing defaults in-memory, so nothing is lost.
    merge_config
fi

log "done"
