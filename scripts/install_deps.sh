#!/usr/bin/env bash
# Install all build dependencies for ProtoHUD on CM5 (Raspberry Pi OS Bullseye 64-bit).
# Tested on: Raspberry Pi OS Bullseye (Debian 11) aarch64
#
# This script is a lightweight alternative to install.sh — it installs only the
# build-time libraries without configuring overlays, services, or boot config.
# For a full system setup run scripts/install.sh instead.
set -euo pipefail

sudo apt-get update

# Build tools
sudo apt-get install -y cmake ninja-build pkg-config git curl

# GLFW3 + OpenGL ES / EGL (window management and GPU context)
sudo apt-get install -y \
    libglfw3-dev \
    libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev

# libcamera (OWLsight CSI cameras)
# On Bullseye, libcamera is available in the Raspberry Pi OS repos
sudo apt-get install -y libcamera-dev

# OpenCV (USB cameras + MJPEG decode)
sudo apt-get install -y libopencv-dev

# nlohmann-json (header-only, CMake target: nlohmann_json::nlohmann_json)
sudo apt-get install -y nlohmann-json3-dev

# libgpiod v1.6 (GPIO button input — Bullseye ships 1.6.x, v1 API)
sudo apt-get install -y libgpiod-dev

# ALSA (audio routing: capture from RP2350 USB Audio, playback to output device)
sudo apt-get install -y libasound2-dev

# Dear ImGui is fetched automatically by CMake (FetchContent v1.91.0) —
# no apt package needed.
# Audio DSP (beamforming, noise suppression, DOA) is handled by the RP2350 —
# libfftw3-dev is no longer required on the CM5.

echo ""
echo "All build dependencies installed."
echo "Build with:  mkdir -p build && cmake -S . -B build -GNinja && ninja -C build"
