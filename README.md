# ProtoHUD

A stereo XR heads-up display running on a Raspberry Pi CM5, designed for a protogen costume helmet. It composites zero-copy libcamera frames from two OWLsight CSI cameras into a VITURE Beast XR glasses display (3840×1080 SBS), overlays a Dear ImGui HUD, and integrates audio routing, LoRa mesh radio, a haptic SmartKnob, and GPIO hardware buttons.

Audio capture and all DSP (beamforming, noise suppression, direction-of-arrival) is handled by a companion **RP2350 helmet audio processor** over USB Audio. The CM5 receives pre-processed stereo and routes it to VITURE glasses, headphones, or HDMI.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Hardware](#hardware)
3. [Pinouts & Inter-Controller Wiring](#pinouts--inter-controller-wiring)
4. [Quick Start](#quick-start)
5. [Post-Install Health Check](#post-install-health-check)
6. [Building Manually](#building-manually)
7. [Camera Resolution](#camera-resolution)
8. [Camera Focus Control](#camera-focus-control)
9. [Night Vision (Exposure & Shutter)](#night-vision-exposure--shutter)
10. [GPIO Buttons](#gpio-buttons)
11. [USB Camera Picture-in-Picture](#usb-camera-picture-in-picture)
12. [Android Mirror](#android-mirror)
13. [Overlay Position & Size](#overlay-position--size)
14. [SmartKnob Menu Navigation](#smartknob-menu-navigation)
15. [Audio Routing](#audio-routing)
16. [HUD Layout](#hud-layout)
17. [Onboard Compass (MPU-9250 / GY-9250)](#onboard-compass-mpu-9250--gy-9250)
18. [Configuration Reference](#configuration-reference)
19. [Troubleshooting](#troubleshooting)

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│  Raspberry Pi CM5 (aarch64 · Bookworm / Trixie + RPT)                │
│                                                                      │
│  libcamera (NV12, zero-copy DMA) ──► DmaCamera ──► GLES2 shader     │
│                          left FBO ◄────────────────────────────────  │
│                         right FBO ◄────────────────────────────────  │
│                                                                      │
│  GLFW / EGL context  ──► XRDisplay (3840×1080 SBS)                  │
│                           composite() → ImGui overlay → present()   │
│                                                                      │
│  Dear ImGui  ──► HudRenderer (DrawList)  ──► compass, LoRa, audio   │
│               ──► MenuSystem (ImGui windows) ──► camera / audio ctrl │
│                                                                      │
│  Serial threads:  Teensy (face LEDs) · SmartKnob · LoRa radio       │
│  GPIO thread:     libgpiod v2 · 3 hardware buttons                  │
│  Audio thread:    ALSA · USB capture (RP2350) → gain → playback     │
│  Android thread:  scrcpy subprocess → V4L2 loopback → OpenCV → GL  │
└──────────────────────────────────────────────────────────────────────┘
         ▲ USB Audio (UAC2, stereo 48 kHz)
┌────────┴───────────────────────────────┐
│  RP2350 Helmet Audio Processor         │
│  6× ICS-43434 MEMS mics               │
│  Beamforming · Noise suppression · DOA │
│  → stereo out via USB Audio Class 2   │
└────────────────────────────────────────┘
```

**Rendering pipeline per frame:**

1. Each OWLsight camera DMA buffer imported as a single multi-plane `DRM_FORMAT_NV12` `EGLImageKHR` → bound to `GL_TEXTURE_EXTERNAL_OES`; Mesa V3D handles YCbCr→RGB in the TMU automatically
2. `samplerExternalOES` fragment shader draws into per-eye `gl::Fbo` (1920×1080 each)
3. `xr.composite()` — blits both FBOs side-by-side into default framebuffer
4. `hud.begin_frame()` → `draw_frame()` → `menu.draw()` → `render_overlay()` — ImGui on top
5. `xr.present()` — `glfwSwapBuffers` + `glfwPollEvents`

Optional async timewarp warps each eye FBO using the latest IMU pose before compositing, reducing rotational latency to ~4 ms.

---

## Hardware

| Component | Details |
|-----------|---------|
| SBC | Raspberry Pi CM5 (8 GB RAM recommended) |
| Display | VITURE Beast XR glasses (3840×1080 SBS, 60/90/120 Hz) |
| Cameras | 2× OWLsight CSI cameras (libcamera NV12 zero-copy path) |
| USB cameras | 2× USB webcams for PiP overlays (optional) |
| Face LEDs | Teensy 4.1 running Prototracer firmware |
| Input | SmartKnob (ESP32-S3 haptic knob) + 3 GPIO buttons |
| Radio | RAK4631 LoRa mesh radio (868/915 MHz) |
| Audio processor | RP2350 helmet audio board — 6-mic beamforming/NR, USB Audio UAC2 output |
| Backup compass | HiLetgo GY-9250 / MPU-9250 9-axis IMU — heading fallback when XR glasses are offline (optional) |
| Android device | Any ADB-capable Android phone (optional, for screen mirror) |
| OS | Raspberry Pi OS Bookworm · Debian Trixie + RPT packages (64-bit, aarch64) |

### GPIO Button Wiring

| Function | GPIO | Behaviour |
|----------|------|-----------|
| Left PiP / AF Left | GPIO 17 | Short press = PiP toggle · 1.5 s = AF left |
| Right PiP / AF Right | GPIO 27 | Short press = PiP toggle · 1.5 s = AF right |
| Menu Select | GPIO 22 | Press while menu open = select item |

All buttons wire to GND; the libgpiod driver configures internal pull-ups.

---

## Pinouts & Inter-Controller Wiring

This section lists every physical connection for each controller in the system and documents which controllers talk to each other and over which interface.

---

### System-Level Connection Map

```
                     ┌─────────────────────────────────────────┐
                     │         Raspberry Pi CM5                │
                     │                                         │
  OWLsight Left ─────┤ CSI0             USB-A ├──────────────────── Teensy 4.1
  OWLsight Right ────┤ CSI1             USB-A ├──────────────────── SmartKnob (ESP32-S3)
                     │                  USB-A ├──────────────────── RAK4631 LoRa radio
  GPIO 17, 27, 22 ───┤ GPIO             USB-A ├──────────────────── RP2350 Helmet Audio
  MPU-9250 (GY-9250)─┤ I2C-1 (GPIO2/3) USB-C ├──────────────────── VITURE Beast XR glasses
                     └─────────────────────────────────────────┘
```

---

### Raspberry Pi CM5

The CM5 is the central hub. All other controllers connect to it.

#### GPIO — Hardware Buttons

| CM5 GPIO | Pin # | Function                            | Connected to |
|----------|-------|-------------------------------------|--------------|
| GPIO 17  | 11    | Button 1 — left PiP / AF left       | Pushbutton → GND |
| GPIO 27  | 13    | Button 2 — right PiP / AF right     | Pushbutton → GND |
| GPIO 22  | 15    | Button 3 — menu select              | Pushbutton → GND |

All buttons are wired to GND; internal pull-ups are configured by `libgpiod`.

#### GPIO — I2C Backup Compass (MPU-9250)

| CM5 Pin # | Signal   | MPU-9250 pin | Notes |
|-----------|----------|--------------|-------|
| 1         | 3.3 V    | VCC          | Do **not** use 5 V — MPU-9250 is 3.3 V only |
| 3         | GPIO 2 (SDA1) | SDA     | I2C-1 data; 4.7 kΩ pull-up to 3.3 V (on GY-9250 board) |
| 5         | GPIO 3 (SCL1) | SCL     | I2C-1 clock; 4.7 kΩ pull-up to 3.3 V (on GY-9250 board) |
| 6         | GND      | GND          | Signal ground |
| —         | —        | AD0          | Tie to GND → I2C address 0x68; tie to 3.3 V → 0x69 |

The GY-9250 breakout board includes onboard 4.7 kΩ pull-up resistors on SDA and SCL, so no external pull-ups are needed. See [Onboard Compass (MPU-9250 / GY-9250)](#onboard-compass-mpu-9250--gy-9250) for software setup and calibration.

#### USB Ports — Serial & Audio Controllers

| Port symlink      | Device       | Controller                      | Protocol |
|-------------------|--------------|---------------------------------|----------|
| `/dev/teensy`     | /dev/ttyACM0 | Teensy 4.1 (face LEDs)          | CDC serial 115200 |
| `/dev/smartknob`  | /dev/ttyACM1 | SmartKnob (ESP32-S3)            | CDC serial 115200 |
| `/dev/lora`       | /dev/ttyACM2 | RAK4631 LoRa radio              | CDC serial 115200 |
| `hw:CARD=HelmetAudio6Mic,DEV=0` | USB Audio | RP2350 Helmet Audio | UAC2 stereo 48 kHz |

The serial symlinks are created by the udev rules in `scripts/install.sh`.

#### CSI Camera Connectors

| Connector | Camera           | libcamera ID |
|-----------|------------------|--------------|
| CAM0/CSI0 | OWLsight Left    | 0            |
| CAM1/CSI1 | OWLsight Right   | 1            |

#### VITURE Beast XR Glasses

Connected to the CM5 via a single USB-C cable (DisplayPort Alt Mode + USB data for the VITURE SDK).

---

### RP2350 Helmet Audio Processor

The RP2350 owns all microphone capture and DSP. It presents itself to the CM5 as a standard USB Audio Class 2 device and streams pre-processed stereo audio.

**VID:PID:** `1209:B350`  
**Product string:** `Helmet Audio 6-Mic`  
**Connection:** USB-A (CM5) → USB-C (RP2350 board)  
**ALSA card name:** `hw:CARD=HelmetAudio6Mic,DEV=0`

The CM5 does **not** run any microphone DSP — it simply captures the stereo stream and routes it to the selected output device.

#### RP2350 → ICS-43434 Microphone Wiring

Six ICS-43434 MEMS microphones are arranged in a hexagonal pattern on the RP2350 board for 360° capture. The RP2350 handles all I2S capture internally.

| Mic label | L/R pin | Azimuth |
|-----------|---------|---------|
| FRONT\_L  | GND     | 330°    |
| FRONT\_R  | VDD     | 30°     |
| SIDE\_L   | GND     | 270°    |
| SIDE\_R   | VDD     | 90°     |
| REAR\_L   | GND     | 210°    |
| REAR\_R   | VDD     | 150°    |

---

### Teensy 4.1 — Face LEDs

The Teensy runs the [ProtoTracer](https://github.com/coelacanthus/ProtoTracer) face-LED firmware and receives animation commands from the CM5 over USB CDC serial at 115200 baud.

**CM5 → Teensy:** USB-A (CM5) to USB-C (Teensy 4.1)  
**Serial port on CM5:** `/dev/teensy` → `/dev/ttyACM0`

---

### SmartKnob — ESP32-S3

The SmartKnob provides haptic detent navigation. It communicates with the CM5 over a framed binary UART protocol at 115200 baud.

**CM5 → SmartKnob:** USB-A (CM5) to USB-C (SmartKnob)  
**Serial port on CM5:** `/dev/smartknob` → `/dev/ttyACM1`

The SmartKnob has no additional external wiring to the CM5 beyond the USB cable. All data exchange (position events, haptic commands, sleep control) flows over the USB serial link. See [SmartKnob Menu Navigation](#smartknob-menu-navigation) for the full protocol table.

---

### RAK4631 LoRa Radio

The RAK4631 is a WisBlock module combining an **nRF52840** MCU with an **SX1262** LoRa transceiver and an optional **RAK3401 1W PA booster**.

**CM5 → RAK4631:** USB-A (CM5) to USB-C (RAK5005-O base board)  
**Serial port on CM5:** `/dev/lora` → `/dev/ttyACM2`

#### RAK4631 Internal Wiring (WisBlock BSP — do not modify)

| nRF52840 signal | WisBlock pin | SX1262 function    |
|-----------------|--------------|---------------------|
| SS / P1.10 (42) | NSS          | SPI chip-select     |
| P1.15 (47)      | DIO1         | IRQ / receive done  |
| P1.06 (38)      | RESET        | Active-low reset    |
| P1.14 (46)      | BUSY         | Busy indicator      |
| DIO2 (internal) | —            | RF TX/RX switch (RAK3401 booster) |

#### RAK12501 GPS (WisBlock Slot A)

The optional GPS module connects to the RAK4631 via WisBlock Slot A UART (`Serial1` in firmware, 9600 baud NMEA). No additional wiring is needed — it plugs directly into the base board.

---

### Component Alternatives

#### Raspberry Pi CM5

| Alternative | Pros | Cons |
|-------------|------|------|
| **Raspberry Pi CM5** *(default)* | Native CSI, official libcamera support, 8 GB RAM option, broad community support | Compute Module form factor requires a carrier board; higher cost than Pi 4 |
| **Raspberry Pi 5** | Same CPU (BCM2712), identical software stack, standard 40-pin header, cheaper | Two CSI ports but uses narrower 22-pin FFC vs CM5; no DDR5 option |
| **Raspberry Pi 4** | Well-tested, widely available, cheaper | Weaker GPU; lower RAM ceiling |
| **NVIDIA Jetson Orin NX** | Far superior GPU/NPU for AI inference | Much higher cost, power, and heat; no native libcamera |
| **Orange Pi 5 Plus** | RK3588 chip, lower cost | libcamera not supported — camera pipeline requires a complete rewrite |

> **Recommendation:** Stick with CM5 (or Pi 5 if a carrier board is impractical). Any alternative requires substantial camera driver rework.

---

#### SmartKnob / ESP32-S3

| Alternative | Pros | Cons |
|-------------|------|------|
| **SmartKnob (ESP32-S3)** *(default)* | Open hardware, haptic detents feel premium, USB-C | Requires motor + driver PCB; must source or build the custom PCB |
| **ESP32-S3 (bare, rotary encoder)** | Drop-in firmware replacement (same UART protocol) | No haptic feedback |
| **GPIO rotary encoder (direct to CM5)** | Zero additional hardware | No haptic feedback; uses GPIO pins |

---

#### RAK4631 LoRa Radio

| Alternative | Pros | Cons |
|-------------|------|------|
| **RAK4631 + RAK3401 1W booster** *(default)* | ~1 W output, long range, integrated GPS slot | May require amateur radio licence at 1W |
| **RAK4631 (no booster)** | Same footprint, 22 dBm, no licence concern in most regions | Reduced range vs booster |
| **Heltec WiFi LoRa 32 v3 (ESP32 + SX1262)** | Cheap, self-contained | Firmware rewrite required; no integrated GPS |
| **Meshtastic node (stock firmware)** | Off-the-shelf, large community | Stock protocol incompatible with ProtoHUD binary frame protocol |

> **Recommendation:** RAK4631 without the booster is the most practical choice. If budget is tight, the Heltec LoRa 32 v3 (SX1262 variant) needs only a firmware port.

---

## Quick Start

Run the one-shot installer on the CM5 (Bookworm or Debian Trixie with RPT packages, aarch64). The script uses per-command `sudo` internally — **do not run it as root**:

```bash
git clone https://github.com/brooksthemaker/ProtoHUD ~/ProtoHUD
cd ~/ProtoHUD
chmod +x scripts/install.sh
./scripts/install.sh
```

The installer runs 11 steps:
1. **Preflight** — checks OS, arch, and sudo availability; caches credentials once
2. **Packages** — apt dependencies (GLFW, GLES, libcamera, OpenCV, libgpiod, ALSA, v4l2loopback-dkms, adb)
3. **RP2350 audio** — checks for RP2350 USB audio device on `hw:CARD=HelmetAudio6Mic`
4. **Boot config** — sets `gpu_mem=256`, `camera_auto_detect=1` in `/boot/config.txt`
5. **udev** — stable `/dev/teensy`, `/dev/smartknob`, `/dev/lora` symlinks
6. **Android** — v4l2loopback module config for Android mirror (`/dev/video4`)
7. **Groups** — adds current user to `gpio dialout video render audio input`
8. **Build** — CMake + Ninja (ImGui fetched automatically on first run)
9. **Libraries** — installs VITURE SDK `.so` files to `/usr/local/lib`
10. **Service** — optional systemd unit for auto-start
11. **Summary** — lists what changed and what still needs a reboot

> **scrcpy** is **not** installed by the script — install it separately afterward if you need Android mirror:
> ```bash
> sudo apt-get install scrcpy   # if available in your repo
> # or build from source: https://github.com/Genymobile/scrcpy
> ```

After the installer finishes, **reboot once** to apply group membership and boot-config changes:

```bash
sudo reboot
```

Then run the health check to verify everything is ready:

```bash
./scripts/check.sh
```

And launch ProtoHUD:

```bash
./protohud                        # uses config/config.json by default
./scripts/run.sh                  # same, with startup diagnostics printed
./scripts/run.sh /path/to/alt.json  # explicit config path
```

---

## Post-Install Health Check

`scripts/check.sh` verifies the full system after a reboot. Run it any time to diagnose problems:

```bash
chmod +x scripts/check.sh && ./scripts/check.sh
```

It checks (and prints PASS / WARN / FAIL for each):

| Check | What it looks for |
|-------|-------------------|
| User groups | `gpio dialout video render audio input` |
| Binary | `build/protohud` exists; root symlink `./protohud` present |
| VITURE SDK | `libglasses.so` + `libcarina_vio.so` in `/usr/local/lib` |
| RP2350 audio | `arecord -l` finds `HelmetAudio` / VID:PID `1209:b350` |
| Audio outputs | VITURE XR glasses, headphones jack, HDMI via `aplay -l` |
| CSI cameras | `libcamera-hello --list-cameras` finds ≥ 2 cameras |
| v4l2loopback | Module loaded; `/dev/video4` ready |
| Boot config | `gpu_mem=256` and `camera_auto_detect=1` set |
| scrcpy | Binary present (optional — install separately for Android mirror) |

All required checks must pass before launching. Fix any `[FAIL]` items, then re-run.

---

## Building Manually

### Prerequisites

```bash
sudo apt install \
    cmake ninja-build pkg-config git \
    libglfw3-dev libgles2-mesa-dev libegl1-mesa-dev \
    libcamera-dev libopencv-dev \
    nlohmann-json3-dev libgpiod-dev libasound2-dev \
    device-tree-compiler
```

Dear ImGui is fetched automatically by CMake (`FetchContent`); no manual install needed.

For Android mirror, also configure v4l2loopback:

```bash
# Persistent config
echo 'options v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1' \
    | sudo tee /etc/modprobe.d/v4l2loopback.conf
echo v4l2loopback | sudo tee -a /etc/modules

# Load now (no reboot needed for first use)
sudo modprobe v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1
```

### Build

```bash
cd ~/ProtoHUD
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -GNinja
ninja -C build -j$(nproc)
```

### Run

```bash
./protohud          # from project root via symlink
# or
./build/protohud    # direct binary path
```

Keyboard shortcuts while running:

| Key | Action |
|-----|--------|
| `M` | Open / close menu |
| `↑` / `↓` | Navigate menu items |
| `Enter` | Select menu item |
| `Backspace` | Go back one menu level |
| `Esc` | Quit |
| `Ctrl+Q` / `Ctrl+K` | Force-kill the process immediately |

---

## Camera Resolution

Resolution can be changed at runtime without restarting the application — libcamera stops, reconfigures the sensor mode, reallocates DMA buffers, and resumes capture.

### Menu Navigation

```
Menu → Camera → Resolution → <preset>
```

### Available Presets

| Preset | Resolution | FPS | Use Case |
|--------|-----------|-----|----------|
| 640×400 @120fps | 640×400 | 120 | Maximum frame rate, lowest detail |
| **1280×800 @60fps** | 1280×800 | 60 | **Default — balanced quality/fps** |
| 1920×1080 @30fps | 1920×1080 | 30 | Full HD, lower frame rate |
| 2560×1440 @15fps | 2560×1440 | 15 | Maximum detail, slideshow fps |

### Config Default

```json
"cameras": {
  "owlsight_left":  { "width": 1280, "height": 800, "fps": 60 },
  "owlsight_right": { "width": 1280, "height": 800, "fps": 60 }
}
```

Changes made via the menu are applied immediately but **not persisted** — edit `config/config.json` to make them permanent.

### Aspect-Ratio Crop Note

At the default **1280×800** preset the OWLsight sensor (native 1920×1080, 16:9) is read out at 8:5. This creates roughly a **10% horizontal crop** compared to the full sensor width — you see slightly less of the scene left-to-right than you would at 1920×1080. Switch to the `1920×1080 @30fps` preset to eliminate the crop if field of view matters more than frame rate.

### Desktop Preview (No Glasses)

When VITURE glasses are not connected ProtoHUD falls back to a 1920×1080 desktop window and shows both cameras **side-by-side** (squeezed to 960×1080 each). This is intentional — it lets you verify both feeds and the HUD layout on a standard monitor during development.

---

## Camera Focus Control

### Focus Modes

| Mode | Behaviour |
|------|-----------|
| **Manual** | Fixed focus position (0–1000) set via menu or GPIO |
| **Auto** | Each camera runs AF independently |
| **Slave** | AF runs on master camera; other syncs its position ~500 ms later |

### Menu Navigation

```
Menu → Camera → Focus Mode    → Manual / Auto / Slave
Menu → Camera → Focus Position → 0 / 100 / … / 1000  (Manual only)
Menu → Camera → Autofocus     → Left / Right / Both
```

### Startup Autofocus

When `"autofocus_on_startup": true` is set in `config.json`, both cameras are placed into **continuous AF mode** (`AfModeContinuous`) immediately on launch — no trigger required. The lens scans within the first few frames and then tracks focus continuously. This is the recommended setting for helmet use where you can't press a button to trigger a one-shot AF scan.

### GPIO Trigger

Hold GPIO 17 (left) or GPIO 27 (right) for 1.5 s to trigger AF on that camera. In Slave mode the partner camera follows automatically.

---

## Night Vision (Exposure & Shutter)

### Exposure Compensation (EV)

**Menu:** `Camera → Exposure (EV)` — range −3.0 to +3.0 stops.

### Shutter Speed

**Menu:** `Camera → Shutter Speed`

| Label | Exposure | Use Case |
|-------|----------|----------|
| 1/4000 | 250 µs | Bright daylight |
| 1/1000 | 1 ms | Bright indoor |
| 1/250 | 4 ms | Dim indoor |
| 1/60 | 16.7 ms | Low light |
| **1/30** | **33.3 ms** | **Default** |
| 1/25 | 40 ms | Very low light |

### Config Defaults

```json
"night_vision": {
  "exposure_ev": 0.0,
  "shutter_us":  33333
}
```

---

## GPIO Buttons

### Config

```json
"gpio": {
  "enabled":           true,
  "button_1_gpio":     17,
  "button_2_gpio":     27,
  "button_3_gpio":     22,
  "af_trigger_time_ms":  1500,
  "pip_trigger_time_ms": 2000
}
```

### Behaviour Summary

| Button | GPIO | Short press | Long press (1.5 s+) |
|--------|------|-------------|---------------------|
| 1 | 17 | Toggle left USB PiP | AF left camera |
| 2 | 27 | Toggle right USB PiP | AF right camera |
| 3 | 22 | Menu select | — (reserved) |

### Diagnosis

```bash
gpioinfo gpiochip0 | grep -E "17|27|22"
gpioget gpiochip0 17   # 1 = released, 0 = pressed
groups $USER           # must include: gpio
```

---

## USB Camera Picture-in-Picture

USB cameras are captured by OpenCV in a background thread and uploaded to GL textures. When PiP is active, `HudRenderer::draw_pip()` renders a 16:9 overlay using ImGui's `DrawList::AddImage`.

### Activation

- **GPIO 17 short press** — toggle left USB camera overlay
- **GPIO 27 short press** — toggle right USB camera overlay
- **Menu → USB Cameras → Position** — choose anchor corner / edge
- **Menu → USB Cameras → Size** — choose overlay height (15%–60% of screen)

### Config

```json
"pip": {
  "enabled": true,
  "size":    0.25,
  "anchor":  "top_center"
}
```

`anchor` values: `top_left` · `top_center` · `top_right` · `bottom_left` · `bottom_center` · `bottom_right`

---

## Android Mirror

Streams an Android device screen into a portrait overlay in the HUD using **scrcpy** as the capture backend. scrcpy writes decoded H.264 frames into a V4L2 loopback device (`/dev/video4`); ProtoHUD reads them with OpenCV on a background thread and uploads each frame to a GL texture.

### Prerequisites

1. **scrcpy installed** — not included in the main installer; install it separately:
   ```bash
   sudo apt-get install scrcpy   # or build from source: https://github.com/Genymobile/scrcpy
   ```
2. `v4l2loopback` module loaded with `exclusive_caps=1` (the installer does this automatically).
3. Android phone connected via USB with **USB debugging enabled** (`Settings → Developer options → USB debugging`).
4. First connection: accept the ADB authorisation dialog on the phone.

```bash
adb devices
# Expected: XXXXXXXX    device
```

### Activation

```
Menu → Android Mirror → Start Mirror   # spawns scrcpy, opens /dev/video4
Menu → Android Mirror → Show Overlay   # makes the overlay visible
```

Or set `"android"."enabled": true` in `config.json` to auto-start on launch.

### Config

```json
"android": {
  "enabled":      false,
  "v4l2_sink":    "/dev/video4",
  "adb_serial":   "",
  "max_size":     1080,
  "fps":          30,
  "overlay_size": 0.40,
  "anchor":       "bottom_left"
}
```

---

## Overlay Position & Size

Both the USB camera PiP and the Android mirror overlay use the same anchor system.

```
┌──────────────────────────────────────────────┐  ← top bar
│  top_left      top_center      top_right      │
│                                               │
│  bottom_left  bottom_center  bottom_right     │
└──────────────────────────────────────────────┘  ← compass tape
```

Size is a fraction of the current eye height (1080 px at default resolution):

| Setting | PiP height | PiP width (16:9) |
|---------|-----------|------------------|
| 15% | 162 px | 288 px |
| 25% | 270 px | 480 px |
| 40% | 432 px | 768 px |
| 60% | 648 px | 1152 px |

---

## SmartKnob Menu Navigation

The SmartKnob (ESP32-S3) provides haptic-feedback detent navigation over UART.

### Menu Structure

```
Root
├── Face Effects   (Idle · Blink · Angry · Happy · Sad · Shocked · Rainbow · Pulse · Wave · Custom)
├── Face Color     (Teal · Cyan · Red · Green · Purple · White)
├── Play GIF       (GIF #0 – #7)
├── Face Brightness(25% · 50% · 75% · 100%)
├── Lens Brightness(Low · Medium · High · Max)
├── Camera
│   ├── Resolution     (640×400@120 · 1280×800@60 · 1920×1080@30 · 2560×1440@15)
│   ├── Focus Mode     (Manual · Auto · Slave)
│   ├── Focus Position (0 · 100 · … · 1000)
│   ├── Autofocus      (Left · Right · Both)
│   ├── Exposure (EV)  (-3.0 · -2.0 · … · +3.0)
│   └── Shutter Speed  (1/4000 · … · 1/25)
├── USB Cameras
│   ├── Position       (Top Left · Top Center · Top Right · Bottom Left · Bottom Center · Bottom Right)
│   └── Size           (15% · 20% · 25% · 30% · 40% · 50% · 60%)
├── Audio
│   ├── Enable / Disable
│   ├── Volume         (25% · 50% · 75% · 100% · 125% · 150%)
│   └── Output         (VITURE · Headphones · HDMI)
├── Headset
│   ├── Dimming        (Level 0–9)
│   ├── HUD Bright     (Level 1–9)
│   ├── Recenter
│   ├── Gaze Lock
│   └── 3D SBS
├── Android Mirror
│   ├── Start Mirror
│   ├── Stop Mirror
│   ├── Show Overlay
│   ├── Hide Overlay
│   ├── Position       (Top Left · Top Center · Top Right · Bottom Left · Bottom Center · Bottom Right)
│   └── Size           (15% · 20% · 25% · 30% · 40% · 50% · 60%)
└── Request Status
```

### Haptic Profiles

| Menu depth | Amplitude | Frequency | Strength |
|-----------|-----------|-----------|----------|
| Root (1) | 255 | 100 | 200 |
| Submenu (2) | 200 | 80 | 150 |
| Deep (3+) | 150 | 60 | 100 |

### Serial Protocol (UART 115200)

| Direction | Code | Payload | Meaning |
|-----------|------|---------|---------|
| ESP32 → CM5 | 0x01 | — | Motor calibration ready |
| ESP32 → CM5 | 0x02 | — | Entering sleep |
| ESP32 → CM5 | 0x03 | reason(1) | Woke up |
| CM5 → ESP32 | 0x81 | count + positions | Set detents |
| CM5 → ESP32 | 0x82 | — | Wake device |
| CM5 → ESP32 | 0x83 | timeout_s(2) | Set sleep timeout |
| CM5 → ESP32 | 0x84 | spacing + min + max + start | Set range |
| CM5 → ESP32 | 0x85 | amp + freq + strength | Set haptics |

---

## Audio Routing

All microphone capture and DSP (beamforming, noise suppression, direction-of-arrival) runs on the **RP2350 helmet audio processor**. The CM5 plays no role in DSP — it simply receives the pre-processed stereo stream over USB Audio and routes it to the selected output.

### Signal Path

```
6× ICS-43434 mics
    │ I2S (internal to RP2350 board)
    ▼
RP2350 — beamforming · noise suppression · DOA
    │ USB Audio UAC2 (stereo 48 kHz 16-bit)
    ▼
CM5 ALSA capture  →  gain  →  ALSA playback
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
             VITURE XR        3.5 mm jack        HDMI
          hw:VITUREXRGlasses  hw:Headphones    hw:vc4hdmi0
```

### Output Selection

Switch the output at runtime via the HUD menu — no restart required:

```
Menu → Audio → Output → VITURE / Headphones / HDMI
```

The switch takes effect at the start of the next ALSA period (~5 ms) with no dropout or gap in audio.

The audio strip in the HUD top bar shows the current output and xrun count:

```
AU → VITURE  X:0
```

### Config

```json
"audio": {
  "enabled":           true,
  "capture_device":    "hw:CARD=HelmetAudio6Mic,DEV=0",
  "active_output":     "viture",
  "output_viture":     "hw:CARD=VITUREXRGlasses,DEV=0",
  "output_headphones": "hw:CARD=Headphones,DEV=0",
  "output_hdmi":       "hw:CARD=vc4hdmi0,DEV=0",
  "sample_rate":       48000,
  "period_size":       256,
  "n_periods":         4,
  "master_gain":       1.0
}
```

`active_output` values: `viture` · `headphones` · `hdmi`

> **Note:** Run `arecord -l` to verify the RP2350 card name after connecting via USB. If the card appears under a different name, update `capture_device` accordingly.

---

## HUD Layout

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  ░░░░░░░░░  Top bar (background fill, no border)           Audio strip       │
│             [clock · unread message badge]           AU → VITURE  X:0  ████ │
├─────────────────────────────────────────────────────────────────────────────┤
│ ● Proot      │                                        │ FACE                │
│ ● LoRa       │                                        │ Connected           │
│ ● Interface  │      Left eye / Right eye camera       │ Effect: Idle        │
│ ● Left Cam   │      (stereo SBS 3840×1080)            │ Palette: #0         │
│ ● Right Cam  │                                        ├─────────────────────┤
│ ● Cam 1      │  [LoRa messages panel — left side,     │ LORA NODES          │
│ ● Cam 2      │   shown only when messages present]    │ NodeName  045° 1.2km│
│ ● Audio      │                                        │   RSSI:-87 SNR:6 8s │
│ ● Android    │                                        │                     │
├─────────────────────────────────────────────────────────────────────────────┤
│░░░░░░░░░░░░░░░░░░░░░  Compass tape  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│
└─────────────────────────────────────────────────────────────────────────────┘
```

**Health indicator column** (left edge, below top bar):
Each row shows a filled dot — teal = OK, red = fault — followed by its label.

| Label | Subsystem |
|-------|-----------|
| Proot | Prototracer face LED controller (Teensy) |
| LoRa | LoRa mesh radio |
| Interface | SmartKnob haptic controller |
| Left Cam | OWLsight left CSI camera |
| Right Cam | OWLsight right CSI camera |
| Cam 1 | USB camera 1 (PiP source) |
| Cam 2 | USB camera 2 (PiP source) |
| Audio | Spatial audio engine |
| Android | Android screen mirror (scrcpy) |

Background opacity of the health panel is configurable via `hud.health_panel_opacity` (0.0–1.0).

---

## Onboard Compass (MPU-9250 / GY-9250)

The **HiLetgo GY-9250** (MPU-9250) is an optional backup compass. It reads heading from the onboard magnetometer and takes over the HUD compass tape whenever the VITURE XR glasses haven't sent an IMU frame for more than 2 seconds — glasses off-head, cable disconnected, or XR disabled. When the glasses reconnect the primary IMU resumes automatically.

### Wiring

Wire the GY-9250 to the CM5's 40-pin header using the I2C-1 bus (GPIO 2 / GPIO 3):

```
CM5 40-pin header
                         ┌─────────────┐
  Pin 1  ── 3.3 V ──────►│ VCC         │
  Pin 3  ── GPIO 2 (SDA)─►│ SDA         │  GY-9250 / MPU-9250
  Pin 5  ── GPIO 3 (SCL)─►│ SCL         │
  Pin 6  ── GND ─────────►│ GND         │
                          │ AD0 ── GND  │  (address 0x68)
                          └─────────────┘
```

| CM5 pin | Signal        | GY-9250 pin | Notes |
|---------|---------------|-------------|-------|
| 1       | 3.3 V         | VCC         | 3.3 V **only** — do not use 5 V |
| 3       | GPIO 2 (SDA1) | SDA         | I2C data; pull-up resistors are on the GY-9250 board |
| 5       | GPIO 3 (SCL1) | SCL         | I2C clock |
| 6       | GND           | GND         | |
| —       | —             | AD0 → GND   | Sets I2C address to 0x68 (default) |

> **Address conflict:** If another I2C device on the bus already uses 0x68, tie AD0 to 3.3 V to use 0x69 and update `mpu_addr` to `105` in `config.json`.

### Enable I2C-1 on the CM5

The I2C-1 bus is disabled by default. Enable it once with `raspi-config` or by editing the boot config directly:

```bash
# Option A — raspi-config (recommended)
sudo raspi-config
# Interface Options → I2C → Enable

# Option B — manual edit
echo 'dtparam=i2c_arm=on' | sudo tee -a /boot/config.txt
sudo reboot
```

Verify the bus is up after reboot:

```bash
ls /dev/i2c-*     # should list /dev/i2c-1
sudo apt install i2c-tools
i2cdetect -y 1    # 0x68 (or 0x69) should appear in the grid
```

Expected output with GY-9250 at default address:

```
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:          -- -- -- -- -- -- -- -- -- -- -- -- --
...
60: -- -- -- -- -- -- -- -- 68 -- -- -- -- -- -- --
```

If `0x68` does not appear: check wiring, confirm 3.3 V on VCC, and verify `dtparam=i2c_arm=on` is present in `/boot/config.txt`.

### Config

```jsonc
"mpu9250": {
  "enabled":         false,          // set to true to activate
  "i2c_bus":         "/dev/i2c-1",
  "mpu_addr":        104,            // 104 = 0x68 (AD0 low) · 105 = 0x69 (AD0 high)
  "declination_deg": 0.0,            // local magnetic declination — see note below
  "heading_offset":  0.0,            // mechanical mounting offset in degrees
  "mag_bias":        [0.0, 0.0, 0.0] // hard-iron calibration; auto-updated by Calibrate Compass
}
```

**Magnetic declination** is the angle between magnetic north and true north at your location. Find yours at [ngdc.noaa.gov/geomag](https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml). Positive = east of true north, negative = west. Example: `"declination_deg": -14.5` for the US Pacific coast.

**Heading offset** compensates for the physical mounting angle of the sensor in the helmet. Adjust until the compass reads 0° when the sensor physically points north.

### Activating at Runtime

```
Menu → Settings → HUD → Compass → Onboard Compass → Active  (toggle ON)
```

The backup heading takes over the compass tape immediately. To stop it:

```
Menu → Settings → HUD → Compass → Onboard Compass → Active  (toggle OFF)
```

### Hard-Iron Calibration

The onboard magnetometer is affected by the helmet's permanent magnets and ferrous metal. The **Calibrate Compass** routine cancels this **hard-iron distortion**:

1. Open the menu:
   ```
   Menu → Settings → HUD → Compass → Onboard Compass → Calibrate  (toggle ON)
   ```
2. **Slowly rotate the helmet through a full 360° in each axis** — roll, pitch, and yaw. Spend ~30 seconds on each rotation axis. The sensor samples the field extremes during this motion.
3. Stop the calibration:
   ```
   Menu → Settings → HUD → Compass → Onboard Compass → Calibrate  (toggle OFF)
   ```
4. The computed `mag_bias` values are automatically written to `config.json` when the program exits so they persist across restarts.

Recalibrate any time the helmet's magnetic environment changes significantly (new hardware installed near the sensor, different venue).

### Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| Compass heading stuck at 0° | MPU-9250 not enabled or I2C fail | Enable in config; check `i2cdetect -y 1` |
| `[mpu] I2C open failed` in log | Wrong bus or address | Verify `i2c_bus` and `mpu_addr` in config |
| Heading drifts ~30° | Magnetic declination not set | Add local `declination_deg` value |
| Heading offset (reads N but not pointing N) | Mounting angle | Adjust `heading_offset` in degrees |
| Heading noisy / jumpy | Hard-iron interference | Run Calibrate Compass routine |
| Backup heading ignored (XR heading used instead) | XR glasses still active | XR IMU suppresses MPU-9250 when fresh (<2 s old) — disconnect glasses to test MPU-9250 alone |

---

## Clock & RTC Battery

The HUD clock arm reads the CM5 system clock via `std::time()` — no extra hardware is required. However, **the CM5 has no RTC battery by default**, which means:

- On every cold boot without a network / NTP server, the clock resets to **1970-01-01 00:00:00**.
- Once the system reaches NTP it syncs automatically, but in the field you may have no internet.

**Fix: install a CR2032 coin cell** into the RTC battery connector on your carrier board:

| Carrier board | Connector | Notes |
|---------------|-----------|-------|
| Official CM5 IO Board | J9 (2-pin, near USB-C) | Polarity marked on silkscreen |
| Custom / third-party boards | Varies — check schematic | Typically a BAT+ / GND footprint |

Verify the RTC is holding time after power-off:
```bash
timedatectl status          # look for "System clock synchronized: yes"
hwclock --show              # reads directly from the hardware RTC
```

### In-HUD Clock Options (Menu → Settings → HUD → Clock)

| Option | Default | Description |
|--------|---------|-------------|
| 24-Hour | on | Toggle 12 / 24-hour format |
| Seconds | on | Show or hide the seconds field |
| Show Date | off | Add a date row (e.g., `Wed 30 Apr`) below the time |
| Font Size | 1.5× | Scale the clock text relative to the standard HUD mono font |
| Time Offset → +1h / -1h / +1m / -1m / Reset | 0 | Shift displayed time without touching the system clock — useful for timezone differences in the field |

---

## Configuration Reference

Full `config/config.json` layout:

```jsonc
{
  "display": {
    "target_fps":   90,       // 60 / 90 / 120
    "hud_opacity":  0.85,
    "hud_scale":    1.0
  },

  "cameras": {
    "owlsight_left": {
      "libcamera_id": 0,
      "width":  1280, "height": 800, "fps": 60,
      "flip_h": false, "flip_v": false
    },
    "owlsight_right": {
      "libcamera_id": 1,
      "width":  1280, "height": 800, "fps": 60,
      "flip_h": false, "flip_v": false
    },
    "usb_cam_1": { "device": "/dev/video2", "width": 1280, "height": 720, "fps": 30 },
    "usb_cam_2": { "device": "/dev/video3", "width": 1280, "height": 720, "fps": 30 }
  },

  "vitrue": {
    "product_id":       0,    // 0 = auto-detect
    "monitor_index":   -1,    // -1 = auto (prefer ≥3840px wide monitor)
    "use_beast_camera": true,
    "enable_imu":       true
  },

  "serial": {
    "teensy":    { "port": "/dev/ttyACM0", "baud": 115200 },
    "lora":      { "port": "/dev/ttyACM2", "baud": 115200, "ping_on_start": true, "node_info_refresh_s": 60 },
    "smartknob": { "port": "/dev/ttyACM1", "baud": 115200, "sleep_timeout_s": 30, "wake_degrees": 5.0 }
  },

  "gpio": {
    "enabled":             true,
    "button_1_gpio":       17,
    "button_2_gpio":       27,
    "button_3_gpio":       22,
    "af_trigger_time_ms":  1500,
    "pip_trigger_time_ms": 2000
  },

  "camera": {
    "autofocus_on_startup": true,
    "autofocus_sync":       true,
    "focus_mode":           "auto",   // "manual" | "auto" | "slave"
    "focus_position":       500
  },

  "night_vision": {
    "exposure_ev":  0.0,    // -3.0 to +3.0
    "shutter_us":   33333   // microseconds
  },

  "pip": {
    "enabled": true,
    "size":    0.25,
    "anchor":  "top_center"
  },

  "android": {
    "enabled":      false,
    "v4l2_sink":    "/dev/video4",
    "adb_serial":   "",
    "max_size":     1080,
    "fps":          30,
    "overlay_size": 0.40,
    "anchor":       "bottom_left"
  },

  "hud": {
    "compass_height_px":    60,
    "panel_width_px":       200,
    "lora_message_history": 50,
    "show_secondary_cams":  true,
    "secondary_cam_size":   0.22,
    "health_panel_opacity": 0.71   // 0.0 = fully transparent · 1.0 = fully opaque
  },

  "audio": {
    "enabled":           true,
    "capture_device":    "hw:CARD=HelmetAudio6Mic,DEV=0",
    "active_output":     "viture",     // viture | headphones | hdmi
    "output_viture":     "hw:CARD=VITUREXRGlasses,DEV=0",
    "output_headphones": "hw:CARD=Headphones,DEV=0",
    "output_hdmi":       "hw:CARD=vc4hdmi0,DEV=0",
    "sample_rate":       48000,
    "period_size":       256,
    "n_periods":         4,
    "master_gain":       1.0
  },

  "colors": {
    "hud_primary":    [0, 220, 180, 220],
    "hud_accent":     [0, 180, 255, 255],
    "hud_warn":       [255, 180, 0,  255],
    "hud_danger":     [255, 60,  60, 255],
    "hud_background": [10,  15,  20, 180]
  }
}
```

**Config hot-reload:** Edit `config/config.json` directly — no rebuild or reconfigure step needed. The binary always reads from the source file on startup, so changes take effect the next time you launch.

Night vision EV/shutter and focus mode are applied live each frame — no restart needed. Audio output can be switched live from the menu.

### HUD Side Panels

The left and right HUD side panels have **transparent backgrounds** — only the border line and text are drawn, keeping the camera feed visible underneath. Brightness is shown as a percentage number (`Brt: 72%`) rather than a graphical bar. Panel width defaults to **200 px**; adjust with `panel_width_px`.

---

## Troubleshooting

### Camera shows black / no image

```bash
cam --list
eglinfo | grep -i "dma_buf\|image_base"
```

Ensure `camera_auto_detect=1` is in `/boot/config.txt`. Reboot after any boot config change.

### GPIO buttons not responding

```bash
gpioget gpiochip0 17    # 1 = released, 0 = pressed
groups $USER            # must include: gpio
sudo usermod -aG gpio $USER && newgrp gpio
```

### Autofocus not working

```bash
cam --camera=0 --info | grep -i "AfMode\|focus"
```

Not all CSI camera modules include a VCM motor. OWLsight supports it; generic IMX219 modules do not.

### Audio not starting

**Check RP2350 is connected and detected:**

```bash
arecord -l   # must list HelmetAudio or similar (VID:PID 1209:b350)
```

If the card is not listed, the RP2350 is not connected or not enumerated. Connect via USB and re-check.

**Check output devices:**

```bash
aplay -l     # look for VITUREXRGlasses, Headphones, vc4hdmi0
```

**Check the audio strip** in the HUD top bar — `AU → VITURE  X:0` is normal. A rising xrun count (X:N) means the system is too busy; try reducing camera resolution. Also check that the **Audio** indicator in the left-side health column is teal (not red).

**Check group membership:**

```bash
groups $USER   # must include: audio
```

### Menu not responding to SmartKnob

```bash
ls -l /dev/ttyACM1
groups $USER  # must include: dialout
```

Reflash SmartKnob firmware if `[knob] Motor calibration complete` never appears in the log.

### Build fails: imgui not found

```bash
# Dear ImGui is fetched via CMake FetchContent on first configure.
# Ensure internet access during cmake:
cmake -S . -B build    # fetches imgui v1.91.0 automatically
```

### Build fails: glfw3 not found

The installer handles GLFW3 automatically (pkg-config path fix + source build fallback). If building manually:

```bash
sudo apt-get install libglfw3-dev
# If pkg-config still can't find it, add the multiarch path:
export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH}"
```

### Android mirror: "V4L2 sink /dev/video4 not ready"

```bash
lsmod | grep v4l2loopback
sudo modprobe v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1
```

If the module fails to load after a kernel update:

```bash
sudo dkms autoinstall
```

### Android mirror: scrcpy fails / "no devices"

```bash
adb devices          # phone must show "device" not "unauthorized"
adb kill-server && adb start-server
```

Ensure USB debugging is enabled and the authorisation dialog was accepted on the phone.

### Can't find the binary after install

The installer creates a symlink at the project root:

```bash
ls -la ~/ProtoHUD/protohud    # should point to build/protohud
./protohud                    # run from project root
```

If the symlink is missing, recreate it:

```bash
ln -sf build/protohud ~/ProtoHUD/protohud
```

---

## Project Structure

```
.
├── src/
│   ├── main.cpp                    — init, render loop, menu definition
│   ├── app_state.h                 — shared state structs (mutex-protected)
│   ├── gl_utils.h                  — GLES2 shader/VBO/FBO helpers
│   ├── protocols.h                 — serial frame protocol definitions
│   ├── android/
│   │   └── android_mirror.h/.cpp  — scrcpy subprocess + V4L2 capture + GL upload
│   ├── camera/
│   │   ├── dma_camera.h/.cpp       — zero-copy NV12 libcamera path
│   │   ├── camera_manager.h/.cpp   — owns both OWLsight + USB cameras
│   │   ├── v4l2_camera.h/.cpp      — V4L2 USB camera capture
│   │   └── viture_camera.h/.cpp    — Beast built-in passthrough camera
│   ├── hud/
│   │   └── hud_renderer.h/.cpp     — Dear ImGui DrawList HUD
│   ├── menu/
│   │   └── menu_system.h/.cpp      — stack-based ImGui menu
│   ├── vitrue/
│   │   ├── xr_display.h/.cpp       — GLFW window, eye FBOs, SBS composite
│   │   └── timewarp.h/.cpp         — rotational homography warp shader
│   ├── input/
│   │   └── gpio_buttons.h/.cpp     — libgpiod v2 button handler
│   ├── serial/
│   │   ├── serial_port.h/.cpp      — raw UART helpers
│   │   ├── teensy_controller.h/.cpp
│   │   ├── lora_radio.h/.cpp
│   │   └── smartknob.h/.cpp
│   └── audio/
│       └── audio_engine.h/.cpp     — ALSA USB capture → gain → playback routing
├── assets/shaders/
│   ├── nv12.vs / nv12.fs           — NV12→RGB camera shader (GLSL ES)
│   └── timewarp.vs / timewarp.fs   — homography warp shader
├── config/config.json
├── overlays/cm5-6mic.dts           — 6-ch I2S DT overlay (boot config; audio handled by RP2350)
├── vendor/viture/                  — VITURE XR SDK (pre-built aarch64 .so)
└── scripts/
    ├── install.sh                  — one-shot CM5 installer (11 steps)
    ├── check.sh                    — post-reboot health check
    └── run.sh                      — launch wrapper with startup diagnostics
```

---

## License & Attribution

ProtoHUD integrates:

- **Dear ImGui** (Omar Cornut) — MIT — immediate-mode UI
- **GLFW** — zlib/libpng — window/context management
- **libcamera** — LGPL — camera abstraction layer
- **SmartKnob** (Scott Bezek) — haptic input knob firmware
- **VITURE XR SDK** — proprietary — XR glasses display & IMU
- **OpenCV** — Apache 2.0 — USB camera decode
- **nlohmann/json** — MIT — config parsing

See individual projects for full license details.
