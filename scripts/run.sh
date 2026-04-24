#!/usr/bin/env bash
# Run ProtoHUD. Optionally pass a config path as $1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/protohud"

if [ ! -f "${BIN}" ]; then
    echo "Binary not found — run scripts/build.sh first"
    exit 1
fi

# Ensure the running user is in the 'video' and 'dialout' groups
# (needed for camera and serial access)
for grp in video dialout; do
    if ! groups | grep -qw "${grp}"; then
        echo "WARNING: user not in '${grp}' group — device access may fail"
        echo "  Fix: sudo usermod -aG ${grp} \$USER && newgrp ${grp}"
    fi
done

CONFIG="${1:-${ROOT}/config/config.json}"
exec "${BIN}" "${CONFIG}"
