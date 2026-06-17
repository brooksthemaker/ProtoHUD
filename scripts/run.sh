#!/usr/bin/env bash
# Run ProtoHUD. Optionally pass a config path as $1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/build/protohud"

echo "Project root : ${ROOT}"
echo "Binary       : ${BIN}"
echo ""

if [ ! -f "${BIN}" ]; then
    echo "ERROR: binary not found — run ./scripts/install.sh first"
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
# Seed config.json from the tracked example on first run so the explicit path
# resolves (the binary only auto-falls-back to the example when launched with
# no argument). Never clobbers existing edits.
if [ ! -f "${CONFIG}" ] && [ "${CONFIG}" = "${ROOT}/config/config.json" ] \
   && [ -f "${ROOT}/config/config.example.json" ]; then
    cp "${ROOT}/config/config.example.json" "${CONFIG}"
    echo "First run: seeded ${CONFIG} from config.example.json"
fi
exec "${BIN}" "${CONFIG}"
