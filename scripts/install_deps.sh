#!/usr/bin/env bash
# Install all build dependencies for ProtoHUD on CM5 (aarch64).
# Tested on: Raspberry Pi OS Bookworm (Debian 12) / Debian Trixie aarch64.
#
# This script is a lightweight alternative to install.sh — it installs only the
# build-time libraries without configuring overlays, services, or boot config.
# For a full system setup (udev, groups, service, boot config) run
# scripts/install.sh instead, and scripts/uninstall.sh to reverse it.
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

# libgpiod v2 (GPIO button input — code uses the v2 API; Bookworm/Trixie ship 2.x).
# On older Bullseye (libgpiod 1.6) the GPIO input TUs won't compile — upgrade the
# OS or backport libgpiod 2.x.
sudo apt-get install -y libgpiod-dev

# ALSA (audio routing: capture from RP2350 USB Audio, playback to output device)
sudo apt-get install -y libasound2-dev

# DBus (KDE Connect phone-notification bridge — optional but the build will
# enable phone integration automatically when it's available)
sudo apt-get install -y libdbus-1-dev

# i2c-tools (needed for any of the I2C peripherals: MPU-9250, BNO055, MPR121 boop sensor)
sudo apt-get install -y i2c-tools

# Dear ImGui is fetched automatically by CMake (FetchContent v1.91.0) —
# no apt package needed.
# Audio DSP (beamforming, noise suppression, DOA) is handled by the RP2350 —
# libfftw3-dev is no longer required on the CM5.

echo ""
echo "All build dependencies installed."
echo "Build with:  mkdir -p build && cmake -S . -B build -GNinja && ninja -C build"
