#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"

mkdir -p "${BUILD}"
cd "${BUILD}"
cmake .. -DCMAKE_BUILD_TYPE=Release -GNinja
ninja -j$(nproc)

echo ""
echo "Binary: ${BUILD}/protohud"
echo "Run:    cd ${BUILD} && ./protohud"
