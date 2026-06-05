# ProtoHUD — Options & Capabilities Reference

---

## Overview

ProtoHUD is a heads-up display system for VITURE XR glasses running on a Raspberry Pi CM5. It composites camera feeds, a HUD overlay, post-processing vision-assist effects, and hardware integrations (LoRa radio, haptic knob, LED face panels, IMU) into a real-time stereo side-by-side display at up to 90 fps.

---

## 1. Camera System

### Supported Cameras
- **Two OWLsight CSI cameras** (left + right eye) — libcamera, EGL dmabuf zero-copy NV12 path
- **Three USB cameras** — OpenCV capture, used as picture-in-picture overlays
- **VITURE Beast passthrough camera** — enabled via config when glasses are detected
- **Android mirror** — scrcpy → V4L2 loopback → overlay

### OWLsight Resolutions
| Label | Resolution | Max FPS |
|---|---|---|
| 640×400 | 640×400 | 120 fps |
| 1280×800 | 1280×800 | 60 fps (default) |
| 1920×1080 | 1920×1080 | 30 fps |
| 2560×1440 | 2560×1440 | 15 fps |

### Digital Zoom (per-eye)
- Zoom levels: 1.0×, 1.25×, 1.5×, 2.0×, 3.0×
- Crop center: Center, Top, Bottom, Left, Right
- Reset Zoom action

### Focus
- **Modes**: Manual, Auto, Slave (right eye mirrors left eye 500 ms after AF completes)
- **Focus Position slider**: 0–1000, step 50
- **One-shot AF**: Left only, Right only, Both

### Night Vision
- Manual toggle (sets exposure EV +3.0, shutter 1/25 s)
- **Auto Night Vision** — enables automatically when scene gain crosses a threshold
- Auto NV Gain Threshold: 1.5–16×, step 0.5
- Exposure (EV) slider: −3.0 to +3.0, step 0.5
- Shutter Speed presets: 1/4000 through 1/25 s (nine steps)

### White Balance
- Auto WB toggle
- Colour temp presets: Tungsten (2800K), Warm (3500K), Neutral (4500K), Daylight (5600K), Cool Blue (7000K)

### Theater Mode
Renders OWLsight feeds at their native aspect ratio (letterbox or pillarbox) with black bars, preserving correct proportions. Black bar region is reserved for future USB camera placement.

### Photo Capture
Captures the raw camera FBO (no HUD overlay) as a PNG. Write is asynchronous; a toast shows the saved path.

| Mode | Output |
|---|---|
| Capture Left | Single eye PNG at camera resolution |
| Capture Right | Single eye PNG at camera resolution |
| Capture Stereo | Side-by-side both eyes in one wide PNG |

- Default save path: `/home/user/Pictures/protohud/`
- Override with `system.photo_dir` in config.json (e.g. an NVMe mount)
- Filename: `protohud_YYYYMMDD_HHMMSS_{left|right|stereo}.png`

### USB Camera Overlays (PiP)
Each of the three USB cameras has:
- Open/close stream toggle
- Show overlay toggle
- Position: Top-Left, Top-Center, Top-Right, Bottom-Left, Bottom-Center, Bottom-Right
- Size: 15%–60%, step 5%
- Flip Upside Down toggle
- Auto Brightness toggle + Brightness Target slider (40–220)
- Software brightness multiplier: 50%, 100%, 150%, 200%, 300%
- Manual exposure controls: Exposure Time, Auto WB, WB Temperature (2800–6500K), Dynamic Framerate toggle
- Scan for Camera action (probes /dev/video0..N)

---

## 2. HUD Display

### Always-On Elements
- **Compass tape** (bottom-center) — shows ~120° arc with cardinal/intercardinal labels, major/mid/minor ticks, LoRa bearing markers
- **Health indicators** (left side) — Proot (Teensy), LoRa, Interface (knob), WiFi
- **Camera indicators** (right side) — L.Cam, R.Cam with AF status (MAN / SLV / LOCK / SCAN / AUTO), USB Cam 1/2
- **Indicator arms** — diagonal lines with label nodes for: Face, Clock, Timer/Alarm
- **Toast notifications** — slide-in from right, up to 4 stacked, auto-dismiss configurable per message

### System Status Panel (toggleable)
Full system information panel in the top-left corner:
- CPU % with 60-sample sparkline
- RAM used/total with 60-sample sparkline
- Uptime
- WiFi: SSID, IP, signal dBm, 4-bar indicator
- Ping latency with 30-sample sparkline and host
- Bluetooth: up to 4 paired devices with connection state
- Performance: FPS + frame-time sparkline
- Serial Latency: Teensy RTT, SmartKnob event age, LoRa RSSI/SNR
- SSH: active state and port

### FPS Overlay (toggleable)
Small per-eye "NN FPS  N.N ms" readout in the top-right of each eye.

### LED Panel Preview (toggleable)
Live 128×64 HUB75 panel canvas from Protoface shared memory, displayed as a floating HUD window (scaled 3×).

### Clock
- 12/24-hour toggle
- Show seconds toggle
- Show date toggle
- Font scale
- Manual time offset (seconds)

### Compass Options
- Background enabled/disabled
- Background opacity (0–1)
- Background side fade (px)
- Tick length (8–48 px, step 2)
- Tick glow toggle
- Height (px)
- Bottom margin (px)
- LoRa node bearing markers (colored per-node)

### HUD Styling
- **Text Scale**: 0.7×–2.0×, step 0.1
- **Glow Intensity**: 0–2, step 0.05
- **Text Glow toggle**
- **Indicator Background toggle**
- **Flip to Top** — mirrors HUD chrome to top edge instead of bottom
- **Health Panel Opacity**
- **PiP Corner Clip** — chamfer radius for USB camera overlay borders

---

## 3. Themes & Color Customization

### Themes (coordinated presets)
Each theme sets glow base, text color, compass tick colors, menu selection style, border, and bold text simultaneously:

| Theme | Accent | Selection Style |
|---|---|---|
| Halo | White | Filled Row |
| Solar | Orange | Accent Bar |
| Fallout | Green | Accent Bar |
| Space | Blue | Accent Bar |

### Individual Color Controls
- **Borders & Lines**: Orange, Teal, Cyan, Green, Yellow, Purple, White, Red
- **Text Color**: White, Cyan, Orange, Amber, Green, Yellow, Red, Purple, Blue, Pink
- **Compass Tick Color**: Orange, Teal, Cyan, Green, Purple, White
- **Compass Glow Color**: Orange, Teal, Cyan, Green, Purple, White
- **Menu Accent**: Orange, Teal, Cyan, Green, Purple
- **Menu Border**: toggle, thickness 0.5–8 px, 8 color options
- **Backgrounds**: HUD background (Dark/Navy/Black/Teal/Green/Space), menu background, compass background (with tint options)
- **Indicator Colors**: active, inactive, fail states — each independently configurable
- **LoRa Node Colors**: per-node (1–8) from: Orange, Teal, Cyan, Green, Yellow, Purple, Red, White

---

## 4. Vision Assist (Post-Processing)

All three effects are independent and stackable. Applied as a GLSL shader pass over the camera FBOs.

### Edge Highlight
Sobel edge detection; draws colored outlines on object boundaries.

| Parameter | Options |
|---|---|
| Enabled | Toggle (keyboard: E) |
| Strength | 10%, 30%, 50%, 70%, 100% |
| Color | Orange, Teal, Cyan, Green, White, Black |
| Detail (kernel step) | Ultra Fine, Fine, Standard, Coarse, Silhouette |
| Threshold | None, Low, Medium, High |
| Size Filter | Off, Tiny, Small, Medium, Large (removes noise/small objects) |

### Background Desaturate
Desaturates low-contrast background regions, leaving foreground objects in color.

| Parameter | Options |
|---|---|
| Enabled | Toggle (keyboard: D) |
| Strength | 25%, 50%, 75%, 100% |
| BG Threshold | Subtle, Medium, Aggressive |
| Focus Blend | Off, Low, Medium, Full (uses AF lens position) |
| Color Protect | 0–100%, step 10% |
| Edge Expand | 0–3, step 0.5 |

### Motion Highlight
Temporal frame-difference; highlights moving objects against a blended reference frame.

| Parameter | Options |
|---|---|
| Enabled | Toggle (keyboard: W) |
| Mode | Fine Line, Fill |
| Sensitivity | Low, Medium, High, Very High |
| Spread | Tight (2px), Close (4px), Medium (8px), Wide (16px) |
| Trail Length | Instant (1 frame) → Extended (~30 frames) |
| Color | Green, Cyan, Yellow, White, Orange |

---

## 5. Effects & Border System

### Effect Types
| Effect | Description | Draw Cost |
|---|---|---|
| None | No border effect | 0 |
| Arm Glints | Sparks along the five indicator arm angles | ~3/s per arm |
| Corner Drift | Slow drifting dots from compass tape corners | ~2.5/s per corner |
| Popup Burst | One-shot 24-particle ring from screen center on alarm/timer popup | One-shot |
| Compass Turbulence | Random sparks inside the compass tape area | ~12/s |
| Nebula Edge | Layered dark-blue/violet gradient cloud on all edges + dot and comet particles; covers corners cleanly via box gradient | 3 fills + ~1–4 particles/frame |
| Dark Vignette | Two-pass dark border (soft 200px outer band + hard 55px edge ring); no particles | 2 fills, static |

### Effect Palettes (applies to particle color)
Theme (match HUD), Halo (white), Solar (amber), Fallout (radioactive green), Space (electric blue)

---

## 6. Audio

- ALSA passthrough from RP2350 USB Audio (beamforming, noise suppression handled on-device)
- **Enable/disable toggle**
- **Volume**: 25%–150%, step 5%
- **Output selection**: VITURE glasses / Headphones / HDMI

---

## 7. LoRa Radio

Communicates with RAK4631 nodes running custom firmware.

### Per-Node Data Tracked
- Name (up to 12 chars)
- Bearing (degrees)
- Distance (meters)
- RSSI (dBm) and SNR (dB)
- Last-seen timestamp

### HUD Integration
- Bearing markers on compass tape (colored per node, 8-color palette)
- Right-side indicator arm: up to 4 nodes showing `NAME ___° X.Xk`
- Message panel (left side): scrolling message history
- Toast notification on incoming message

### Menu Actions
- Clear Messages
- Clear Notifications
- Lookup Node (1–8): fires a 6-second toast with distance, heading, RSSI, SNR, time-since

### Message Queue
Ring buffer capped at 50 messages (configurable via `hud.lora_message_history`).

---

## 8. SmartKnob

ESP32-S3 haptic detent rotary encoder over UART.

- Rotary input navigates menus and toast action buttons
- Detent count and haptic strength adapt to menu depth:
  - Root level: strong haptics
  - Submenus: medium
  - Deep menus: lighter
- Sleep timeout configurable (default 30 s)
- Wake threshold configurable (default 5°)
- Last event age shown in System Panel

---

## 9. Face / LED Panels

Two interchangeable backends — Teensy (ProtoTracer) and Protoface Python daemon. Runtime-selectable.

### Effects (10 face animations)
Idle, Blink, Angry, Happy, Sad, Shocked, Rainbow, Pulse, Wave, Custom

### Layered Particle Effects (native Protoface)
Composable, multi-layer particle compositor with per-layer color, density,
speed, direction, and blend (add/normal). Primitives:
`sparkle, embers, rain, snow, confetti, rings, fireflies, clouds, lightning,
meteor, bubbles, fireworks, vortex, water`. Built-in presets include `fire,
aurora, nebula, plasma, sonar, thunderstorm, arc, meteor_shower, fireworks,
bubbles, vortex`, plus liquid palettes `water, lava, toxic, ocean, plasma_fluid,
mercury`.

- **Lightning** forks **random branches** (Branches density control); with
  **Arc Mode** it becomes continuous crackling electrical arcs between drifting
  points (the `arc` preset). Both are per-layer options in the builder.

- **Water / liquid fill** — the face looks partially filled with a tinted liquid
  (single colour or a deep→surface gradient, with Fill Level + Viscosity
  controls). Modelled as a real fluid: a **damped-spring slosh** (it leans and
  settles, overshooting like liquid in a container), **multi-frequency surface
  waves**, a **sub-pixel anti-aliased surface** with a **specular sheen** and an
  **edge meniscus**. The surface stays level in world space, so it **tilts as you
  roll your head** and **sloshes** when the gyro/accel kicks — driven by the BNO055.
  Rendered in **canvas space**, so a multi-panel face reads as one continuous
  tank across the whole visor (set `protoface.continuous_effects` to make a
  mirrored 2-eye face self-render both eyes so the tank is world-continuous
  rather than a flipped copy). Optional **rising bubbles** (in the liquid) or
  **drip droplets** (above the surface), and **pitch-driven fill** (look down →
  the liquid rises). Use `blend:normal` for an opaque liquid or `add` for a
  glowy lava/plasma look.
- **Motion-reactive layers** — couple a layer's direction to the IMU: `heading`
  (lock to compass), `yaw` (drift as you turn your head), or `tilt` (skew like
  gravity when you roll). Fed live from the BNO055 each frame.
- **Audio/motion-reactive density** — scale a layer's particle count from a live
  signal via `intensity_from`: `audio` (mic level — pulses with sound),
  `yaw_rate`, or `accel`. Audio rides the existing voice-analyzer path.
- **Expression-coupled effects** — optional: the active effect auto-swaps to a
  mood preset as the face changes (angry→fire, happy→celebration, sad→rain,
  shocked→galaxy), restoring your chosen effect for neutral faces.
- **Authoring** — the Effects menu opens to four pages: **Single Effects** (one
  primitive at a time — Select applies it, **Ctrl+Select** opens its settings),
  **Premade Effects** (curated combos with the recipe shown in the side panel;
  Ctrl+Select saves a copy to Custom), **Custom** (the 5-layer builder with
  per-layer Density/Speed/Direction/Motion/Blend, a **Live Preview** toggle that
  applies edits to the panels continuously as you tweak — the sim updates in
  place without resetting — plus Save As…/slots/Load/Delete/Export), and
  **Random** (Surprise-Me generator with Save-to-Custom).
  Presets persist under `cfg["protoface"]["custom_effects"]`.

### Material Colors (12)
Default, Yellow, Orange, White, Green, Purple, Red, Blue, Rainbow, Flow Noise, H Rainbow, Black

### GIF Playback
8 GIF slots (labels configurable via `serial.teensy.gif_names[]` in config)

### Controls
| Control | Range |
|---|---|
| Color (preset) | Teal, Cyan, Red, Green, Purple, White |
| Color (custom) | Full RGB picker |
| Brightness | 0–255, step 5 |
| Accent Brightness | 0–10 |
| Face Size | 0–10 |
| Fan Speed (Teensy face fan) | 0–10 |
| Microphone | Toggle |
| Mic Level | 0–10 |
| Boop Sensor | Toggle |
| Spectrum Mirror | Toggle |

- **Release Control** — relinquishes HUD control; Teensy resumes autonomous animation
- **Panel Preview** — live 128×64 LED canvas as floating HUD window

### Cooling Fans (Pi-driven PWM)
Helmet cooling fans driven directly from the Pi GPIO via software PWM (2-pin fan
through a MOSFET, or a 4-pin fan's control line) — **up to 4 fans grouped into 2
independently-controlled zones** (e.g. intake / exhaust). Menu: **System →
Cooling Fans** — global Enabled, then per-zone Mode (**Manual** fixed speed /
**Auto** ramps with CPU temperature between configurable min/max), Speed
(0–100%), and a live output/temp readout. Pins/behaviour in
`config["fans"]["zones"]`; use GPIO clear of HUB75 (see
`hardware/carrier-board/PINMAP.md`). Distinct from the Teensy face-fan control
above.

---

## 10. XR Display Controls

| Control | Range |
|---|---|
| Lens Brightness | 1–7 |
| Dimming | 0–9 |
| HUD Brightness | 1–9 |
| Recenter Tracking | Action |
| Gaze Lock | Action |
| 3D SBS Mode | Action |

---

## 11. Input Methods

### GPIO Buttons (3 physical buttons)
| Button | Short press | ≥1.5 s hold | ≥5 s hold |
|---|---|---|---|
| Button 1 (GPIO 17) | Toggle left USB PiP | Autofocus left camera | Capture left photo |
| Button 2 (GPIO 27) | Toggle right USB PiP | Autofocus right camera | Capture right photo |
| Button 3 (GPIO 22) | Menu select / toast select | — | — |

All thresholds configurable via config.json (`af_trigger_time_ms`, `pip_trigger_time_ms`, `capture_trigger_time_ms`).

### Keyboard Shortcuts
| Key | Action |
|---|---|
| M | Open/close menu |
| Up / Down | Navigate menu or toast |
| Enter / Right | Select menu item |
| Backspace / Left | Menu back |
| E | Toggle Edge Highlight |
| D | Toggle Background Desaturate |
| W | Toggle Motion Highlight |
| 1 | Toggle USB Cam 1 PiP |
| 2 | Toggle USB Cam 2 PiP |
| 3 | Toggle focus Manual/Auto |
| 4 | Autofocus both cameras |
| , | Focus step near (−20) |
| . | Focus step far (+20) |
| Escape / P | Quit |
| Ctrl+Q / Ctrl+K | Force quit |

### SmartKnob
Rotate to navigate menus and toasts; detent count matches current menu level.

---

## 12. System Monitoring & Connectivity

| Metric | Update Rate | Where Shown |
|---|---|---|
| CPU % | 1 s | System Panel + sparkline |
| RAM used/total | 1 s | System Panel + sparkline |
| Uptime | 1 s | System Panel |
| FPS + frame time | Every frame | System Panel + FPS overlay |
| WiFi SSID, IP, dBm | 5 s | System Panel + health indicator |
| Ping latency | 2 s | System Panel + sparkline |
| Bluetooth devices | 10 s | System Panel |
| Teensy RTT | Per request | System Panel |
| SmartKnob event age | Every frame | System Panel |
| LoRa RSSI/SNR | Per packet | System Panel + LoRa indicator |
| SSH status | On toggle | System Panel |

- **SSH Access** toggle (starts/stops sshd via systemctl)
- **Refresh Bluetooth** action (immediate rescan)
- **Onboard Compass** (MPU-9250): Active toggle, Calibrate action (hard-iron, writes mag_bias to config)

---

## 13. Configuration File (config.json)

| Section | Key Fields |
|---|---|
| `display` | width, height, fullscreen, target_fps, eye_separation |
| `cameras.owlsight_left/right` | libcamera_id, width, height, fps, flip_h/v |
| `cameras.usb_cam_1/2/3` | device, resolution, fps, exposure, wb settings |
| `serial.teensy` | port, baud, gif_names[8] |
| `serial.lora` | port, baud, ping_on_start, node_info_refresh_s |
| `serial.smartknob` | port, baud, sleep_timeout_s, wake_degrees |
| `vitrue` | product_id, use_beast_camera, enable_imu |
| `hud` | compass dimensions, opacity, panel widths, pip settings, effects |
| `gpio` | enabled, pin assignments, hold thresholds |
| `audio` | enabled, output device, sample_rate, master_gain |
| `post_process` | per-effect enable + all parameter values |
| `night_vision` | exposure_ev, shutter_us |
| `clock` | use_24h, show_seconds, show_date, manual_offset_s |
| `system` | ping_host, wifi_iface, ssh_port, crash_dir, photo_dir |
| `mpu9250` | enabled, i2c_bus, declination_deg, mag_bias[3] |
| `android` | enabled, v4l2_sink, adb_serial, overlay settings |
| `colors` | hud_primary, hud_accent, hud_warn, hud_danger, hud_background |
| `splash` | enabled, animated, image, title, subtitle |

Settings modified via the menu are persisted to config.json on exit.

---

## 14. Other Features

### Timers & Alarms
- Countdown timer: preset durations (5/10/30/60 min) or custom (0–99 min + 0–59 s)
- Alarm: set by hour/minute, fires as a pulsing red edge gradient + toast with DISMISS action
- Timer expiry fires orange edge pulse + toast with DISMISS / +2 MIN / +5 MIN actions

### Software Updates (System → Software)
- **Check for Updates** — runs `git fetch origin main`
- **Pull & Rebuild** — runs `git pull` + `./scripts/build.sh`

### In-HUD Updater (System → Updates)
User-initiated branch-based updater. **ProtoHUD never auto-updates** — nothing
fetches, builds, or restarts unless picked from this menu.
- **Current Version** — current branch + short hash + commit subject, with a
  status panel (behind-count, last-checked time).
- **Check for Updates** — the only network step: `git fetch --prune origin`,
  then reports how far behind the current branch is (no code changes).
- **Update This Branch & Restart** — pulls + rebuilds + restarts the current
  branch via `scripts/update.sh <branch> --restart`.
- **Select Branch** — lists remote branches (curated to `main` + `claude/*`,
  with a *Show All Branches* toggle). Highlighting a branch shows its recent
  commits in the context panel; selecting it updates + restarts to it.
- **Rollback Last Update** — restores the build + config saved just before the
  last update (via `scripts/rollback.sh`). Visible only when a rollback point
  exists.
- **Settings/content protection** — `config/config.json` and user faces/effects
  live outside version control, so updates never clobber them. `update.sh`
  records a rollback point (commit + config backup under `state/update/`)
  before every update.
- **Standalone recovery** — `scripts/rollback.sh` works outside ProtoHUD (over
  SSH) if the HUD won't boot: `scripts/rollback.sh --restart` returns to the
  last known-good build, `--main` is a last-resort reset to `origin/main`,
  `--list` shows the recorded rollback point.

### Demo Mode (System → Demo Mode)
Test all notification types without hardware: Trigger Alarm, Trigger Timer Done, LoRa Message, App Toast, Toast Stack (×4), Clear All.

### Crash Reporter
Signal handler writes a JSON crash dump with git hash to `system.crash_dir` (default `/tmp`).

### Splash Screen
Animated hexagon mark or custom PNG logo with title/subtitle, shown at startup for a configurable minimum duration.

### Rotational Timewarp
IMU-driven reprojection reduces rotational display latency from ~16 ms to ~4 ms by warping the last rendered frame using the latest IMU pose before display.

---

## 15. Possible Additions

### Camera & Capture
- **HUB75 panel flash for photo capture** — `ProtoFaceController::set_color(255,255,255)` before capture with 1–2 frame delay, then restore. Infrastructure already wired.
- **USB camera fill in theater mode black bars** — shift the main feed flush to one edge and fill the single contiguous black region with a USB camera feed. Layout coordinates already planned in the theater-mode code.
- **Burst capture mode** — take N frames in rapid succession and save as a numbered sequence.
- **Timelapse / interval capture** — capture at a fixed interval (e.g. every 30 s) and assemble into a video.
- **JPEG option for captures** — stb_image_write supports JPEG; add a format toggle alongside the existing PNG path.
- **Video recording** — pipe the FBO to a V4L2 loopback or encode with FFmpeg. The async write infrastructure from photo capture provides a starting point.
- **Per-eye independent zoom/exposure** — currently zoom and exposure are mirrored; allow full decoupling.

### HUD & Display
- **Minimap overlay** — GPS or LoRa-derived relative position plotted on a small top-down map.
- **Heads-up altimeter / barometer** — BME280 or similar I2C sensor feeding an altitude indicator arm.
- **Battery indicator** — read `/sys/class/power_supply` or INA219 over I2C; show percentage + sparkline in System Panel.
- **Configurable indicator arm layout** — let the user drag/reorder which indicators appear on left vs. right side.
- **Notification action on GPIO** — wire button 3 long-press to dismiss / action the currently focused toast.
- **Custom clock faces** — analog dial, large digital, or minimal dot-matrix styles selectable in the clock submenu.
- **Scrolling LoRa message ticker** — thin single-line ticker along the top edge for incoming messages without occupying the full side panel.

### Vision Assist
- **Depth estimation overlay** — monocular depth model (MiDaS-small) running on the Pi NPU; colorize by depth or use depth as a desaturation mask instead of contrast-based detection.
- **Face/person detection bounding boxes** — lightweight MobileNet or YOLOv8-nano inference; draw labeled outlines on the camera feed.
- **Color blindness correction modes** — shift hues in the GLSL post-process pass to assist deuteranopia / protanopia.
- **Zoom-to-cursor** — magnify a selectable region of the camera feed (e.g. whatever the SmartKnob points at) without scaling the full frame.

### LoRa & Networking
- **Send message UI** — on-screen keyboard (SmartKnob-driven character picker) to compose and send LoRa messages.
- **Node proximity alerts** — toast when a node enters/exits a configurable distance threshold.
- **Track history / breadcrumb trail** — store node position history and draw a faint path on a minimap.
- **MQTT bridge** — publish LoRa node data to an MQTT broker over WiFi for logging or dashboard display.

### Hardware Integrations
- **HUB75 ambient sync** — extend the active HUD color palette to the LED panels in real time (e.g. edge highlight color drives panel color).
- **Haptic feedback for alerts** — use SmartKnob motor for a short pulse on alarm/timer fire or incoming LoRa message.
- **Speaker/buzzer support** — play a short tone on alarm events via the ALSA audio path.
- **Additional GPIO inputs** — extra buttons for dedicated functions (e.g. dedicated capture button, one-touch effect cycle).
- **I/O expander support (buttons + LEDs)** — let `gpio.pins` slots target an MCP23017 (or similar) exposed as a `/dev/gpiochipN` via DT overlay: add an optional per-slot `"chip"` field so `input::GpioInputs` drives expander pins with no new handling code, plus a thin LED-output helper (libgpiod set-value / PCA9685 PWM). Hardware design in `hardware/carrier-board/IO-EXPANSION.md`.
- **Rotary encoder for zoom** — second SmartKnob or simple encoder wired to digital zoom without entering the menu.

### System & Infrastructure
- **OTA update via LoRa or WiFi** — download and apply firmware updates without a keyboard, triggered from the System menu.
- **Config profiles** — save/load named config presets (day, night, indoor, outdoor) from the System menu.
- **Remote HUD control** — WebSocket server exposing state so a phone/tablet can act as a second display or control surface.
- **Log viewer** — System Panel tab showing recent log lines from a ring buffer, scrollable with the SmartKnob.
- **NVMe storage management** — once NVMe is connected, add a Storage section to System: free space, eject, auto-move captures older than N days.
