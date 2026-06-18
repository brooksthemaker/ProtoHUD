#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build"

# Let ccache cache compilations that use a precompiled header (harmless when
# ccache isn't installed). Without this ccache conservatively skips PCH builds.
export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros}"

mkdir -p "${BUILD}"
cd "${BUILD}"

# Only run the (slow) CMake configure when there isn't a usable build tree yet.
# Ninja re-invokes CMake on its own whenever CMakeLists.txt changes, so an
# ordinary rebuild can skip straight to the compile step.
if [ ! -f build.ninja ] || [ ! -f CMakeCache.txt ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -GNinja
fi

ninja -j"$(nproc)"

echo ""
echo "Binary: ${BUILD}/protohud"
echo "Run:    cd ${BUILD} && ./protohud"
