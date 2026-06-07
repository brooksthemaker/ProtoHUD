#!/usr/bin/env bash
# install-emulator.sh — opt into the libretro emulator add-on for ProtoHUD.
#
# The emulator (libretro) game source is NOT part of a stock build. This script
# turns it on and rebuilds: it reconfigures CMake with -DENABLE_LIBRETRO=ON and
# compiles. It does not download any emulator cores or ROMs — you supply those
# yourself and point config.json at them (see docs/games.md).
#
# Usage:  scripts/install-emulator.sh [build_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"

echo "ProtoHUD emulator add-on installer"
echo "  repo:  $ROOT"
echo "  build: $BUILD_DIR"

# The frontend needs the vendored libretro API header. It ships with the repo;
# fetch it if it's somehow missing (e.g. a partial checkout).
HDR="$ROOT/third_party/libretro/libretro.h"
if [[ ! -f "$HDR" ]]; then
    echo "libretro.h missing — fetching it…"
    mkdir -p "$(dirname "$HDR")"
    curl -fsSL \
        https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h \
        -o "$HDR"
fi

# ROM drop folder for the in-menu ROM picker (game.libretro_rom_dir default).
ROMS_DIR="$ROOT/third_party/roms"
mkdir -p "$ROMS_DIR"
echo "ROM folder ready: $ROMS_DIR"

echo "Configuring with -DENABLE_LIBRETRO=ON…"
cmake -S "$ROOT" -B "$BUILD_DIR" -DENABLE_LIBRETRO=ON

echo "Building…"
cmake --build "$BUILD_DIR" -j "$(nproc)"

cat <<'EOF'

Emulator add-on installed.

Next steps:
  1. Get a libretro core (.so), e.g.:
        sudo apt install libretro-snes9x        # or any libretro-* package
     or download from https://buildbot.libretro.com/
     Point config.json at it:
        "game": { "libretro_core": "/usr/lib/libretro/snes9x_libretro.so" }
  2. Drop ROMs into the ROM folder:
        third_party/roms/
  3. In ProtoHUD: Games > Source > Emulator (libretro) > ROM > pick one > Play.
     (Or set game.libretro_rom directly. Use a different folder via
      game.libretro_rom_dir.)

See docs/games.md for details (supported cores, N64/hardware-render notes).
EOF
