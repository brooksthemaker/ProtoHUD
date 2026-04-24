# ProtoHUD

A stereo XR heads-up display running on a Raspberry Pi CM5, designed for a protogen costume helmet. It composites zero-copy libcamera frames from two OWLsight CSI cameras into a VITURE Beast XR glasses display (3840×1080 SBS), overlays a Dear ImGui HUD, and integrates spatial audio, LoRa mesh radio, a haptic SmartKnob, and GPIO hardware buttons.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Hardware](#hardware)
3. [Pinouts & Inter-Controller Wiring](#pinouts--inter-controller-wiring)
4. [Quick Start](#quick-start)
5. [Building Manually](#building-manually)
6. [Camera Resolution](#camera-resolution)
7. [Camera Focus Control](#camera-focus-control)
8. [Night Vision (Exposure & Shutter)](#night-vision-exposure--shutter)
9. [GPIO Buttons](#gpio-buttons)
10. [USB Camera Picture-in-Picture](#usb-camera-picture-in-picture)
11. [Android Mirror](#android-mirror)
12. [Overlay Position & Size](#overlay-position--size)
13. [SmartKnob Menu Navigation](#smartknob-menu-navigation)
14. [Spatial Audio](#spatial-audio)
15. [Configuration Reference](#configuration-reference)
16. [Troubleshooting](#troubleshooting)

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│  Raspberry Pi CM5 (aarch64 · Bullseye)                               │
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
│  GPIO thread:     libgpiod v1 · 3 hardware buttons                  │
│  Audio thread:    ALSA · 6-ch I2S · beamformer · wind gate          │
│  Android thread:  scrcpy subprocess → V4L2 loopback → OpenCV → GL  │
└──────────────────────────────────────────────────────────────────────┘
```

**Rendering pipeline per frame:**

1. Each OWLsight camera DMA buffer imported as `EGLImageKHR` → bound to `GL_TEXTURE_2D`
2. NV12→RGB GLSL shader draws into per-eye `gl::Fbo` (1920×1080 each)
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
| Audio | 6-channel I2S microphone array (cm5-6mic DT overlay) |
| Android device | Any ADB-capable Android phone (optional, for screen mirror) |
| OS | Raspberry Pi OS Bullseye (64-bit, aarch64) |

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
  GPIO 18-21, 25 ────┤ I2S (PCM)        USB-C ├──────────────────── VITURE Beast XR glasses
  GPIO 17, 27, 22 ───┤ GPIO                   │
                     └─────────────────────────────────────────┘
                              │ I2S (PCM)
                   ┌──────────┴──────────┐
                   │  ICS-43434 mic ×6   │
                   └─────────────────────┘
```

---

### Raspberry Pi CM5

The CM5 is the central hub. All other controllers connect to it.

#### GPIO — I2S Microphone Array (preferred path)

| CM5 GPIO | Pin # | Signal   | Connected to                        |
|----------|-------|----------|-------------------------------------|
| GPIO 18  | 12    | PCM_CLK  | BCK on all 6 ICS-43434 mics (shared)|
| GPIO 19  | 35    | PCM_FS   | LRCK on all 6 ICS-43434 mics (shared)|
| GPIO 20  | 38    | PCM_DIN0 | DATA on FRONT\_L + FRONT\_R mics     |
| GPIO 21  | 40    | PCM_DIN1 | DATA on SIDE\_L + SIDE\_R mics       |
| GPIO 25  | 22    | PCM_DIN2 | DATA on REAR\_L + REAR\_R mics       |
| 3V3      | 1/17  | VDD      | VDD on all 6 mics                   |
| GND      | any   | GND      | GND on all 6 mics                   |

> The `cm5-6mic` device tree overlay must be enabled to expose these pins as
> `hw:CARD=sndrpii2s0,DEV=0`. See [overlays/README.md](overlays/README.md).

#### GPIO — Hardware Buttons

| CM5 GPIO | Pin # | Function                            | Connected to |
|----------|-------|-------------------------------------|--------------|
| GPIO 17  | 11    | Button 1 — left PiP / AF left       | Pushbutton → GND |
| GPIO 27  | 13    | Button 2 — right PiP / AF right     | Pushbutton → GND |
| GPIO 22  | 15    | Button 3 — menu select              | Pushbutton → GND |

All buttons are wired to GND; internal pull-ups are configured by `libgpiod`.

#### USB Ports — Serial Controllers

| Port symlink   | Device     | Controller           | Baud    |
|----------------|------------|----------------------|---------|
| `/dev/teensy`  | /dev/ttyACM0 | Teensy 4.1 (face LEDs) | 115200 |
| `/dev/smartknob` | /dev/ttyACM1 | SmartKnob (ESP32-S3) | 115200 |
| `/dev/lora`    | /dev/ttyACM2 | RAK4631 LoRa radio   | 115200  |

Symlinks are created by the udev rules in `scripts/install.sh`.

#### CSI Camera Connectors

| Connector | Camera           | libcamera ID |
|-----------|------------------|--------------|
| CAM0/CSI0 | OWLsight Left    | 0            |
| CAM1/CSI1 | OWLsight Right   | 1            |

#### VITURE Beast XR Glasses

Connected to the CM5 via a single USB-C cable (DisplayPort Alt Mode + USB data
for the VITURE SDK).

---

### Teensy 4.1 — Face LEDs

The Teensy runs the [ProtoTracer](https://github.com/coelacanthus/ProtoTracer)
face-LED firmware and receives animation commands from the CM5 over USB CDC
serial at 115200 baud.

**CM5 → Teensy:** USB-A (CM5) to USB-C (Teensy 4.1)  
**Serial port on CM5:** `/dev/teensy` → `/dev/ttyACM0`

#### Teensy 4.1 Pinout — Mic Bridge (fallback only)

Use this wiring only if the CM5 I2S driver cannot support three simultaneous
DIN lines. When using the mic bridge, flash `firmware/teensy_audio/` instead
and set `capture_device` to `hw:CARD=TeensyAudio,DEV=0`.

| Teensy Pin | Signal         | Mic role             | I2S bus |
|------------|----------------|----------------------|---------|
| Pin 8      | I2S1 quad DIN  | FRONT\_L (GND) + FRONT\_R (VDD) | I2S1 |
| Pin 6      | I2S1 quad DIN  | SIDE\_L (GND) + SIDE\_R (VDD)   | I2S1 |
| Pin 21     | I2S1 SCK (BCK) | Shared clock for I2S1 bus        | I2S1 |
| Pin 20     | I2S1 WS (LRCK) | Shared frame sync for I2S1 bus   | I2S1 |
| Pin 5      | I2S2 DIN       | REAR\_L (GND) + REAR\_R (VDD)   | I2S2 |
| Pin 33     | I2S2 SCK (BCK) | Shared clock for I2S2 bus        | I2S2 |
| Pin 34     | I2S2 WS (LRCK) | Shared frame sync for I2S2 bus   | I2S2 |
| 3V3        | VDD            | All 6 mics (shared)              | —    |
| GND        | GND            | All 6 mics (shared)              | —    |

The Teensy then bridges all 6 mic channels to the CM5 as a USB Audio device.

---

### SmartKnob — ESP32-S3

The SmartKnob provides haptic detent navigation. It communicates with the CM5
over a framed binary UART protocol at 115200 baud.

**CM5 → SmartKnob:** USB-A (CM5) to USB-C (SmartKnob)  
**Serial port on CM5:** `/dev/smartknob` → `/dev/ttyACM1`

The SmartKnob has no additional external wiring to the CM5 beyond the USB cable.
All data exchange (position events, haptic commands, sleep control) flows over
the USB serial link. See [SmartKnob Menu Navigation](#smartknob-menu-navigation)
for the full protocol table.

---

### RAK4631 LoRa Radio

The RAK4631 is a WisBlock module combining an **nRF52840** MCU with an
**SX1262** LoRa transceiver and an optional **RAK3401 1W PA booster**.

**CM5 → RAK4631:** USB-A (CM5) to USB-C (RAK5005-O base board)  
**Serial port on CM5:** `/dev/lora` → `/dev/ttyACM2`

#### RAK4631 Internal Wiring (WisBlock BSP — do not modify)

The SX1262 is wired to the nRF52840 internally by the WisBlock PCB:

| nRF52840 signal | WisBlock pin | SX1262 function    |
|-----------------|--------------|---------------------|
| SS / P1.10 (42) | NSS          | SPI chip-select     |
| P1.15 (47)      | DIO1         | IRQ / receive done  |
| P1.06 (38)      | RESET        | Active-low reset    |
| P1.14 (46)      | BUSY         | Busy indicator      |
| DIO2 (internal) | —            | RF TX/RX switch (RAK3401 booster) |

#### RAK12501 GPS (WisBlock Slot A)

The optional GPS module connects to the RAK4631 via WisBlock Slot A UART
(`Serial1` in firmware, 9600 baud NMEA). No additional wiring is needed — it
plugs directly into the base board.

| Signal  | Connection              |
|---------|-------------------------|
| Serial1 | RAK12501 (Quectel L76K) |
| 3V3     | Supplied by base board  |
| GND     | Supplied by base board  |

---

### ICS-43434 Microphone Array (6× mics)

Six ICS-43434 MEMS microphones are arranged in a hexagonal pattern for
360° spatial audio capture. They connect either directly to the CM5 (preferred)
or to the Teensy 4.1 (fallback bridge).

#### Direct CM5 Wiring (preferred)

Three stereo pairs, each sharing one CM5 DIN line. The L/R pin on each mic
determines which LRCK slot it occupies.

| Mic label | L/R pin | Data line | CM5 GPIO | USB ch (bridge) | Azimuth |
|-----------|---------|-----------|----------|-----------------|---------|
| FRONT\_L  | GND     | PCM_DIN0  | GPIO 20  | 0               | 330°    |
| FRONT\_R  | VDD     | PCM_DIN0  | GPIO 20  | 1               | 30°     |
| SIDE\_L   | GND     | PCM_DIN1  | GPIO 21  | 2               | 270°    |
| SIDE\_R   | VDD     | PCM_DIN1  | GPIO 21  | 3               | 90°     |
| REAR\_L   | GND     | PCM_DIN2  | GPIO 25  | 4               | 210°    |
| REAR\_R   | VDD     | PCM_DIN2  | GPIO 25  | 5               | 150°    |

All mics share BCK (GPIO 18), LRCK (GPIO 19), VDD (3.3 V), and GND.

---

### Component Alternatives

#### Raspberry Pi CM5

The CM5 is the recommended SBC but alternatives exist depending on form factor
and budget constraints.

| Alternative | Pros | Cons |
|-------------|------|------|
| **Raspberry Pi CM5** *(default)* | Native CSI, official libcamera support, CM5 I2S multi-DIN, 8 GB RAM option, broad community support | Compute Module form factor requires a carrier board; higher cost than Pi 4 |
| **Raspberry Pi 5** | Same CPU (BCM2712), identical software stack, standard 40-pin header, cheaper, no carrier board needed | Two CSI ports but uses narrower 22-pin FFC vs CM5; no DDR5 option; slightly more power draw in a helmet |
| **Raspberry Pi 4** | Well-tested, widely available, cheaper | BCM2711 has weaker GPU; I2S multi-DIN support is less reliable; lower RAM ceiling |
| **NVIDIA Jetson Orin NX** | Far superior GPU/NPU for AI inference (object detection, etc.) | Much higher cost, power, and heat; no native libcamera; I2S/GPIO ecosystem less mature; heavier |
| **Orange Pi 5 Plus** | RK3588 chip with strong GPU and 32 GB RAM option; lower cost than Jetson | libcamera not supported — camera pipeline requires a complete rewrite; smaller community; GPIO library fragmentation |
| **Khadas VIM4** | Amlogic A311D2, decent GPU, compact | Very limited CSI camera support; audio I2S driver quality inconsistent; niche ecosystem |

> **Recommendation:** Stick with CM5 (or Pi 5 if a carrier board is impractical).
> Any alternative requires substantial camera and audio driver rework.

---

#### ICS-43434 MEMS Microphones

The ICS-43434 is a high-quality, low-noise digital I2S microphone well-suited
for beamforming arrays.

| Alternative | Pros | Cons |
|-------------|------|------|
| **ICS-43434** *(default)* | Low noise floor (−65 dBFS A-weighted), flat 20 Hz–20 kHz response, wide dynamic range (87 dB SNR), simple I2S interface | Requires PCB or breakout; QFN package difficult to hand-solder |
| **INMP441** | Very common, cheap breakout boards readily available, good SNR (61 dB), I2S | Narrower frequency response (60 Hz–15 kHz); lower SNR than ICS-43434; more crosstalk on shared bus |
| **SPH0645LM4H** | Adafruit breakout available, SparkFun ecosystem, good SNR (65 dB) | Philips I2S variant (data valid on opposite clock edge) — requires driver adjustment or adapter; slightly higher noise |
| **SPH0655LM4H** | Same as SPH0645 but with higher AOP (103 dBSPL) | Same driver edge issue as SPH0645; pricier breakout boards |
| **MSM261S4030H0** | Very cheap on AliExpress, I2S | Poor documentation, inconsistent QC, noisy at low signal levels |
| **Analog MEMS (e.g., ADMP504)** | Extremely low noise, excellent sensitivity | Requires external ADC (e.g., PCM1808) — adds I2C/SPI complexity and another power rail |

> **Recommendation:** ICS-43434 is the best choice if you can solder SMD or
> use a custom breakout. For prototyping, INMP441 breakouts work and the only
> software change is `capture_device` in `config.json`.

---

#### SmartKnob / ESP32-S3

The SmartKnob uses an ESP32-S3 with a BLDC motor driver for haptic feedback.

| Alternative | Pros | Cons |
|-------------|------|------|
| **SmartKnob (ESP32-S3)** *(default)* | Open hardware, haptic detents feel premium, USB-C, native BLE option | Requires motor + driver PCB; must source or build the custom PCB; calibration required on first boot |
| **ESP32-S3 (bare, rotary encoder)** | Drop-in firmware replacement (same UART protocol), simpler build, cheaper | No haptic feedback — purely mechanical detents; less premium user experience |
| **ESP32-C3 (rotary encoder)** | Cheapest option, smallest form factor | No I2S (not relevant here), fewer GPIOs; no haptics; same lack-of-feedback drawback |
| **RP2040 (rotary encoder)** | Extremely cheap, great USB support, MicroPython or C SDK | Would require firmware rewrite (SmartKnob firmware is Arduino/ESP-IDF); no haptic option |
| **Bluetooth rotary encoder (HID)** | Cable-free | Adds BLE latency (~10–50 ms); battery management in helmet; pairing complexity |
| **GPIO rotary encoder (direct to CM5)** | Zero additional hardware | Requires software debounce; loses haptic feedback entirely; uses GPIO pins already constrained |

> **Recommendation:** Use the original SmartKnob for the best experience. If
> the haptic PCB is unavailable, a bare ESP32-S3 with a rotary encoder and the
> same firmware (minus motor init) is the simplest fallback — no CM5 code
> changes needed.

---

#### RAK4631 LoRa Radio

The RAK4631 (nRF52840 + SX1262) is a WisBlock module with optional GPS and a
1W RF booster.

| Alternative | Pros | Cons |
|-------------|------|------|
| **RAK4631 + RAK3401 1W booster** *(default)* | ~1 W output, long range (1–5 km line-of-sight), integrated GPS slot, WisBlock modular ecosystem, well-supported by RadioLib | Overkill for short-range; 1W may require amateur radio licence in some regions; bulkier WisBlock stack |
| **RAK4631 (no booster)** | Same footprint, 22 dBm (~160 mW), still long-range, no licence concern in most regions | Reduced range vs booster |
| **Heltec WiFi LoRa 32 (ESP32 + SX1276/SX1278)** | Very cheap, self-contained, OLED display, USB-C | Older SX1276 (worse sensitivity than SX1262); firmware rewrite from RadioLib/nRF to ESP-IDF; no integrated GPS slot |
| **TTGO T-Beam (ESP32 + SX1276 + GPS)** | Integrated GPS + LoRa + battery management, cheap, popular in Meshtastic | SX1276 vs SX1262 sensitivity gap; firmware rewrite required; uses different sync word / modem settings |
| **Adafruit Feather M0 LoRa (ATSAMD21 + RFM95W)** | Popular, well-documented, Arduino-compatible | RFM95W is SX1276-based; lower MCU power; no GPS slot; 100 mW max; USB-serial adapter needed |
| **Meshtastic node (stock firmware)** | Off-the-shelf, large community, mesh routing built-in | Stock Meshtastic protocol is incompatible with the ProtoHUD binary frame protocol — would require a full USB protocol shim or firmware fork |

> **Recommendation:** RAK4631 without the booster is the most practical choice
> for short-to-medium range and avoids licensing headaches. If budget is tight,
> the Heltec LoRa 32 v3 (SX1262 variant) is the closest drop-in and needs
> only a firmware port.

---

## Quick Start

Run the one-shot installer on the CM5 (**requires sudo**, Bullseye, aarch64):

```bash
git clone <repo-url> ~/protohud
cd ~/protohud
chmod +x scripts/install.sh
sudo scripts/install.sh
```

The installer (11 steps):
1. Installs all apt dependencies (GLFW, GLES, libcamera, OpenCV, libgpiod, ALSA, scrcpy, v4l2loopback-dkms, adb, …)
2. Compiles and installs the `cm5-6mic` I2S device-tree overlay
3. Patches `/boot/config.txt` (gpu_mem=256, camera_auto_detect, overlay)
4. Writes udev rules for stable `/dev/teensy`, `/dev/smartknob`, `/dev/lora` symlinks
5. Configures v4l2loopback (`/dev/video4`) for Android mirror via `/etc/modprobe.d/`
6. Adds your user to `gpio dialout video render audio input` groups
7. Builds ProtoHUD with CMake + Ninja
8. Installs the VITURE SDK `.so` files
9. Creates a systemd service (`sudo systemctl enable --now protohud`)

After the installer finishes, **reboot once** to apply group membership and boot-config changes:

```bash
sudo reboot
```

Then run:

```bash
cd ~/protohud/build
./protohud                          # windowed desktop
./protohud ../config/config.json    # explicit config path
```

---

## Building Manually

### Prerequisites

```bash
sudo apt install \
    cmake ninja-build pkg-config git \
    libglfw3-dev libgles2-mesa-dev libegl1-mesa-dev libgl1-mesa-dev \
    libcamera-dev libopencv-dev \
    nlohmann-json3-dev libfftw3-dev libgpiod-dev libasound2-dev \
    device-tree-compiler \
    scrcpy v4l2loopback-dkms adb
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
cd ~/protohud
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -GNinja
ninja -C build -j$(nproc)
```

### Run

```bash
cd build
./protohud
```

Keyboard shortcuts while running:

| Key | Action |
|-----|--------|
| `M` | Open / close menu |
| `↑` / `↓` | Navigate menu items |
| `Enter` | Select menu item |
| `Backspace` | Go back one menu level |
| `Esc` | Quit |

---

## Camera Resolution

Resolution can be changed at runtime without restarting the application — libcamera stops, reconfigures the sensor mode, reallocates DMA buffers, and resumes capture. The GLSL shader and GL textures are preserved.

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

> libcamera snaps to the nearest sensor mode the physical camera supports. If an OWLsight doesn't support the requested size, libcamera returns the closest match and logs a warning.

### Config Default

The startup resolution comes from the camera config block:

```json
"cameras": {
  "owlsight_left":  { "width": 1280, "height": 800, "fps": 60 },
  "owlsight_right": { "width": 1280, "height": 800, "fps": 60 }
}
```

Changes made via the menu are applied immediately but **not persisted** — edit `config.json` to make them permanent.

### API (C++)

```cpp
// Reconfigure both cameras simultaneously
cameras.set_resolution(1920, 1080, 30);

// Reconfigure one camera independently
cameras.owl_left()->reconfigure(640, 400, 120);
```

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

### GPIO Trigger

Hold GPIO 17 (left) or GPIO 27 (right) for 1.5 s to trigger AF on that camera. In Slave mode the partner camera follows automatically.

### API (C++)

```cpp
cameras.owl_left()->start_autofocus();
cameras.owl_left()->set_focus_position(500);  // 0–1000
bool locked = cameras.owl_left()->is_af_locked();
int  pos    = cameras.owl_left()->get_focus_position();
```

---

## Night Vision (Exposure & Shutter)

### Exposure Compensation (EV)

**Menu:** `Camera → Exposure (EV)` — range −3.0 to +3.0 stops.

Each stop doubles or halves the sensor gain applied by the ISP. EV and shutter are applied every frame to both cameras.

### Shutter Speed

**Menu:** `Camera → Shutter Speed`

| Label | Exposure | Use Case |
|-------|----------|----------|
| 1/4000 | 250 µs | Bright daylight |
| 1/2000 | 500 µs | Daylight |
| 1/1000 | 1 ms | Bright indoor |
| 1/500 | 2 ms | Standard indoor |
| 1/250 | 4 ms | Dim indoor |
| 1/125 | 8 ms | Low light |
| 1/60 | 16.7 ms | Night vision |
| 1/30 | 33.3 ms | **Default** |
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
# Check GPIO lines are visible
gpioinfo gpiochip0 | grep -E "17|27|22"

# Read button state (1 = released, 0 = pressed)
gpioget gpiochip0 17

# Check group membership
groups $USER  # must include: gpio
```

---

## USB Camera Picture-in-Picture

USB cameras are captured by OpenCV in a background thread and uploaded to GL textures. When PiP is active, `HudRenderer::draw_pip()` renders a 16:9 overlay using ImGui's `DrawList::AddImage`. Position and size are controlled at runtime via the menu or set in config.

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

### Diagnosis

```bash
v4l2-ctl --list-devices        # find USB camera /dev paths
v4l2-ctl -d /dev/video2 --info # verify format support
```

---

## Android Mirror

Streams an Android device screen into a portrait overlay in the HUD using **scrcpy** as the capture backend. scrcpy writes decoded H.264 frames into a V4L2 loopback device (`/dev/video4`); ProtoHUD reads them with OpenCV on a background thread and uploads each frame to a GL texture.

### Prerequisites

1. `v4l2loopback` module loaded with `exclusive_caps=1` (the installer does this automatically).
2. Android phone connected via USB with **USB debugging enabled** (`Settings → Developer options → USB debugging`).
3. First connection: accept the ADB authorisation dialog on the phone.

```bash
# Verify ADB sees the device
adb devices
# Expected output:
# List of devices attached
# XXXXXXXX    device
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

| Key | Description |
|-----|-------------|
| `enabled` | Auto-start scrcpy on launch |
| `v4l2_sink` | V4L2 loopback device created by v4l2loopback (`video_nr=4`) |
| `adb_serial` | Target a specific device (`adb devices` to find serial). Empty = first connected. |
| `max_size` | Longest dimension scrcpy will encode (px). Lower = less CPU. |
| `fps` | Maximum frame rate passed to scrcpy (`--max-fps`). |
| `overlay_size` | Starting height of the overlay as a fraction of screen height. |
| `anchor` | Starting position (see [Overlay Position & Size](#overlay-position--size)). |

### HUD indicator

The `AN` dot in the top-bar health strip is green when scrcpy is connected and frames are flowing.

### Menu

```
Android Mirror
├── Start Mirror    — spawns scrcpy subprocess
├── Stop Mirror     — kills scrcpy and stops capture thread
├── Show Overlay    — makes the overlay visible
├── Hide Overlay    — hides the overlay (capture continues in background)
├── Position        — Top Left / Top Center / Top Right / Bottom Left / Bottom Center / Bottom Right
└── Size            — 15% / 20% / 25% / 30% / 40% / 50% / 60%
```

---

## Overlay Position & Size

Both the USB camera PiP and the Android mirror overlay use the same anchor system. Changes take effect immediately on the next rendered frame; they are not persisted to `config.json` automatically (edit the file to make them permanent).

### Anchor positions

```
┌──────────────────────────────────────────────┐  ← top bar
│  top_left      top_center      top_right      │
│                                               │
│                                               │
│  bottom_left  bottom_center  bottom_right     │
└──────────────────────────────────────────────┘  ← compass tape
```

Bottom positions automatically clear the compass tape (they use `compass_height_px` as a bottom margin).

### Size

Size is a fraction of the current eye height (1080 px at default resolution):

| Setting | PiP height | PiP width (16:9) | Android height | Android width (9:16) |
|---------|-----------|------------------|----------------|----------------------|
| 15% | 162 px | 288 px | 162 px | 91 px |
| 25% | 270 px | 480 px | 270 px | 152 px |
| 40% | 432 px | 768 px | 432 px | 243 px |
| 60% | 648 px | 1152 px | 648 px | 365 px |

### Menu paths

```
Menu → USB Cameras → Position / Size
Menu → Android Mirror → Position / Size
```

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
│   ├── Mode           (Passthrough · Focus Auto · Focus Manual)
│   ├── Focus Dir      (Front · Right · Back · Left · diagonals)
│   └── Wind Gate      (Off · Light · Medium · Strong · Max)
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

## Spatial Audio

Six-channel I2S microphone array connected directly to CM5 GPIO via the `cm5-6mic` DT overlay.

### Pipeline Stages

| Stage | Description |
|-------|-------------|
| Wind gate | Inter-mic coherence detector; suppresses turbulence bursts |
| Noise suppressor | Minimum-statistics Wiener filter per bin per channel |
| GCC-PHAT DOA | Direction-of-arrival estimator (azimuth in degrees) |
| Beamformer | Delay-and-sum spatial filter towards estimated or manual DOA |
| Spatial processor | Binaural ILD/ITD rendering tied to IMU head yaw |

### Config

```json
"audio": {
  "capture_device":  "hw:CARD=sndrpii2s0,DEV=0",
  "playback_device": "hw:CARD=VITUREXRGlasses,DEV=0",
  "sample_rate": 48000,
  "period_size": 256
}
```

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
      "width":  1280, "height": 800, "fps": 60
    },
    "owlsight_right": {
      "libcamera_id": 1,
      "width":  1280, "height": 800, "fps": 60
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
    "lora":      { "port": "/dev/ttyACM2", "baud": 115200 },
    "smartknob": { "port": "/dev/ttyACM1", "baud": 115200, "sleep_timeout_s": 30 }
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
    "focus_mode":           "auto",   // "manual" | "auto" | "slave"
    "focus_position":       500
  },

  "night_vision": {
    "exposure_ev":  0.0,    // -3.0 to +3.0
    "shutter_us":   33333   // microseconds
  },

  "pip": {
    "enabled": true,
    "size":    0.25,      // fraction of screen height (0.15–0.60)
    "anchor":  "top_center"
    // anchor: top_left | top_center | top_right | bottom_left | bottom_center | bottom_right
  },

  "android": {
    "enabled":      false,        // true = auto-start scrcpy on launch
    "v4l2_sink":    "/dev/video4",// V4L2 loopback device (video_nr=4)
    "adb_serial":   "",           // empty = first connected USB device
    "max_size":     1080,         // scrcpy --max-size
    "fps":          30,           // scrcpy --max-fps
    "overlay_size": 0.40,         // starting size (fraction of screen height)
    "anchor":       "bottom_left" // starting position
  },

  "hud": {
    "compass_height_px": 60,
    "panel_width_px":    320,
    "lora_message_history": 50
  },

  "audio": {
    "enabled":         true,
    "capture_device":  "hw:CARD=sndrpii2s0,DEV=0",
    "playback_device": "hw:CARD=VITUREXRGlasses,DEV=0",
    "sample_rate":     48000,
    "period_size":     256,
    "master_gain":     1.0,
    "pipeline": {
      "wind":  { "depth": 0.85, "floor_gain": 0.08 },
      "noise": { "enabled": true, "noise_bias": 1.5, "floor_gain": 0.05 }
    }
  }
}
```

Config changes take effect on restart. Night vision EV/shutter and focus mode are applied live each frame — no restart needed.

---

## Troubleshooting

### Camera shows black / no image

```bash
# List libcamera cameras
cam --list

# Check EGL extensions
eglinfo | grep -i "dma_buf\|image_base"
```

Ensure `camera_auto_detect=1` is in `/boot/config.txt` and the overlay was applied (`dtoverlay=cm5-6mic` if using the mic array).

### Resolution change has no effect

libcamera logs the actual negotiated mode:
```
[dma] reconfigured camera 0 → 1920×1080 @30fps
```
If the sensor doesn't support the requested size, libcamera returns the nearest match. Check `cam --camera=0 --info` for supported modes.

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

```bash
aplay -l   # check VITUREXRGlasses device
arecord -l # check sndrpii2s0 device (needs reboot after overlay)
```

### Menu not responding to SmartKnob

```bash
ls -l /dev/ttyACM1
# should show crw-rw---- ... dialout
groups $USER  # must include: dialout
```

Reflash SmartKnob firmware if the `[knob] Motor calibration complete` message never appears.

### Build fails: imgui not found

```bash
# Dear ImGui is fetched via CMake FetchContent on first configure.
# Ensure internet access during cmake, or pre-download:
cmake -S . -B build    # fetches imgui v1.91.0 automatically
```

### Android mirror: "V4L2 sink /dev/video4 not ready"

The v4l2loopback module is not loaded or not configured with `exclusive_caps=1`.

```bash
lsmod | grep v4l2loopback          # check if module is loaded
ls /dev/video*                      # check device exists
sudo modprobe v4l2loopback video_nr=4 card_label="AndroidMirror" exclusive_caps=1
cat /sys/devices/virtual/video4linux/video4/name  # should print "AndroidMirror"
```

If the module fails to load after a kernel update, rebuild the DKMS module:

```bash
sudo dkms autoinstall
```

### Android mirror: scrcpy fails / "no devices"

```bash
adb devices          # phone must show "device" not "unauthorized"
adb kill-server      # reset ADB if the server is stale
adb start-server
```

Ensure USB debugging is enabled on the phone (`Settings → Developer options → USB debugging`) and the authorisation dialog was accepted.

### Android mirror: overlay shows "No signal"

scrcpy is running but frames are not arriving. Common causes:

```bash
# Check scrcpy is actually running
pgrep -a scrcpy

# Check the V4L2 sink is receiving data
v4l2-ctl -d /dev/video4 --stream-mmap --stream-count=1 2>&1 | head -5

# Try running scrcpy manually to see error output
scrcpy --no-audio --no-control --video-codec=h264 --max-size=1080 \
       --v4l2-sink=/dev/video4 --max-fps=30
```

Some Android versions require `--video-encoder` or `--video-source=display` depending on the scrcpy version. Check `scrcpy --version`; version ≥ 2.0 is recommended.

### Android mirror: overlay position/size not saved after restart

Menu changes are runtime-only. Edit `config/config.json` to persist them:

```json
"pip":     { "anchor": "bottom_right", "size": 0.30 },
"android": { "anchor": "top_left",     "overlay_size": 0.35 }
```

---

## Project Structure

```
.
├── src/
│   ├── main.cpp                    — init, render loop, menu definition
│   ├── app_state.h                 — shared state structs (mutex-protected)
│   ├── gl_utils.h                  — GLES2 shader/VBO/FBO helpers
│   ├── android/
│   │   └── android_mirror.h/.cpp  — scrcpy subprocess + V4L2 capture + GL upload
│   ├── camera/
│   │   ├── dma_camera.h/.cpp       — zero-copy NV12 libcamera path
│   │   ├── camera_manager.h/.cpp   — owns both OWLsight + USB cameras
│   │   └── viture_camera.h/.cpp    — Beast built-in passthrough camera
│   ├── hud/
│   │   └── hud_renderer.h/.cpp     — Dear ImGui DrawList HUD
│   ├── menu/
│   │   └── menu_system.h/.cpp      — stack-based ImGui menu
│   ├── vitrue/
│   │   ├── xr_display.h/.cpp       — GLFW window, eye FBOs, SBS composite
│   │   └── timewarp.h/.cpp         — rotational homography warp shader
│   ├── input/
│   │   └── gpio_buttons.h/.cpp     — libgpiod v1 button handler
│   ├── serial/
│   │   ├── teensy_controller.h/.cpp
│   │   ├── lora_radio.h/.cpp
│   │   └── smartknob.h/.cpp
│   └── audio/
│       ├── audio_engine.h/.cpp
│       ├── enhanced_pipeline.h/.cpp
│       └── …
├── assets/shaders/
│   ├── nv12.vs / nv12.fs           — NV12→RGB camera shader (GLSL ES)
│   └── timewarp.vs / timewarp.fs   — homography warp shader
├── config/config.json
├── overlays/cm5-6mic.dts           — 6-ch I2S microphone DT overlay
├── vendor/viture/                  — VITURE XR SDK (pre-built .so)
└── scripts/install.sh              — one-shot CM5 installer
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
