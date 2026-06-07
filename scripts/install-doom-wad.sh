#!/usr/bin/env bash
# install-doom-wad.sh — fetch a Doom IWAD for ProtoHUD's built-in Doom source.
#
# The Doom engine (doomgeneric) is compiled into ProtoHUD, but it needs a game
# data file (an "IWAD"). ProtoHUD ships none — the retail Doom WADs are
# copyrighted. This downloads Freedoom (a libre, freely-redistributable Doom
# game) and installs it at the path ProtoHUD looks for by default.
#
# Already have a WAD (e.g. shareware doom1.wad, or doom2.wad you own)? Skip this
# and just copy it to the destination below, or set game.doom_wad in config.json.
#
# Usage:  scripts/install-doom-wad.sh [dest_wad_path]
#   dest_wad_path defaults to ~/.local/share/protohud/doom.wad
set -euo pipefail

DEST="${1:-$HOME/.local/share/protohud/doom.wad}"

need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' is required (sudo apt install $2)"; exit 1; }; }
need curl curl
need unzip unzip

if [[ -f "$DEST" ]]; then
    echo "A WAD already exists at: $DEST"
    echo "Remove it first if you want to re-install. Nothing to do."
    exit 0
fi

echo "Finding the latest Freedoom release…"
# Pick the freedoom-x.y.z.zip asset (the phase 1 + 2 WAD bundle) from the latest release.
URL="$(curl -fsSL https://api.github.com/repos/freedoom/freedoom/releases/latest \
        | grep -oE 'https://[^"]*/freedoom-[0-9.]+\.zip' | head -1)"
if [[ -z "$URL" ]]; then
    echo "error: couldn't find a Freedoom zip in the latest release."
    echo "       Grab one manually from https://freedoom.github.io/ and copy a"
    echo "       .wad to: $DEST"
    exit 1
fi
echo "  $URL"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading…"
curl -fsSL "$URL" -o "$TMP/freedoom.zip"

echo "Extracting…"
unzip -q "$TMP/freedoom.zip" -d "$TMP"
# Prefer freedoom1.wad (Ultimate Doom-style episodes); fall back to any .wad.
WAD="$(find "$TMP" -iname 'freedoom1.wad' | head -1)"
[[ -z "$WAD" ]] && WAD="$(find "$TMP" -iname '*.wad' | head -1)"
if [[ -z "$WAD" ]]; then
    echo "error: no .wad found inside the Freedoom zip."; exit 1
fi

mkdir -p "$(dirname "$DEST")"
cp "$WAD" "$DEST"
echo
echo "Installed $(basename "$WAD") -> $DEST"
echo "In ProtoHUD: Games > Source > Doom > Play."
echo "(To use a different WAD, set game.doom_wad in config.json.)"
