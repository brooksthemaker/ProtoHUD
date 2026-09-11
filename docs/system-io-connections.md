# ProtoHUD System — Complete I/O, Connections & Sub-Assembly Reference

One document covering every controller, connection, GPIO assignment, and
expansion option across the three repos: **ProtoHUD** (CM5 host + carrier
board), **ProtoFace** (HUB75 face renderer), and **Smart-Knob-Redux**
(ESP32-S3 haptic dial). Sources: `README.md`, `hardware/carrier-board/*`,
`firmware/*`, `ProtoFace/HARDWARE.md`, `Smart-Knob-Redux/src/main.cpp`.

---

## 1. Sub-Assemblies (Bill of Boards)

| # | Sub-assembly | MCU / SoC | Link to system | Role |
|---|--------------|-----------|----------------|------|
| 1 | **Raspberry Pi CM5** (8 GB rec.) on carrier board | BCM2712 | — (central hub) | Rendering, cameras, audio routing, HUD, HUB75 face |
| 2 | **RP2350 Helmet Audio Processor** | RP2350 | USB (UAC2 audio) | 6× ICS-43434 MEMS mic capture, beamforming, noise suppression, DOA → stereo 48 kHz |
| 3 | **RP2354B I/O Coprocessor** (on custom carrier) | RP2354B (QFN-80, 48 GPIO, 2 MB on-die flash) | USB-CDC via onboard hub | Sensors, MAX7219 chains, WS2812 zones, 8 servos, 10 buttons, 8 ADC inputs |
| 4 | **Button Coprocessor** (optional, Pico 2 W class) | RP2350A/B | USB-CDC | Debounced buttons, boop pads, voice changer, DS18B20 temps, fan PWM, MAX7219 SPI bridge |
| 5 | **SmartKnob** (Smart-Knob-Redux) | ESP32-S3 | USB (CDC + HID dial) | Haptic BLDC detent knob, 3 buttons, menu navigation |
| 6 | **Teensy 4.1** (face path A) | i.MX RT1062 | USB-CDC 115200 | ProtoTracer face-LED driver (alternative to Protoface/HUB75) |
| 7 | **RAK4631 LoRa node** (+ optional RAK3401 1 W PA, RAK12501 GPS) | nRF52840 + SX1262 | USB-CDC 115200 | 868/915 MHz mesh radio, GPS |
| 8 | **VITURE Beast XR glasses** | — | USB-C (DP Alt Mode + USB data) | 3840×1200 SBS stereo display + built-in IMU + audio out |
| 9 | **ESP32-C3 wireless remote** (optional) | ESP32-C3 | USB-serial | ESP-NOW in-paw remote receiver |
| 10 | **Android phone** (optional) | — | USB (ADB / KDE Connect tether) | Screen mirror, notifications, battery, file inbox |
| 11 | **Teensy 4.1 audio bridge** (fallback only) | i.MX RT1062 | USB Audio 6-ch | Alternative mic-array bridge if RP2350 path unavailable |

Displays / outputs driven: 2× OWLsight CSI cameras (in), up to 3× USB webcams
(in), HUB75 LED panels (face), MAX7219 chains, WS2812 zones, servos, fans.

---

## 2. System-Level Connection Map

```
                       ┌───────────────────────────────────────────────┐
                       │            Raspberry Pi CM5                   │
  OWLsight Left  ──────┤ CSI0 (22-pin FFC)                             │
  OWLsight Right ──────┤ CSI1 (22-pin FFC)                             │
                       │                                               │
  HUB75 panels ◄───────┤ GPIO ×14 (via 74AHCT245 / Adafruit bonnet)    │
  Fans (2 zones) ◄─────┤ GPIO 18/19 (MOSFET low-side PWM)              │
  RP2354B RUN/BOOTSEL ◄┤ GPIO 7/8                                      │
                       │                                               │
                       │ USB 2.0 host ──► USB2514B 4-port hub ──┬─► RP2354B I/O coproc (CDC)
                       │                                        ├─► RP2350 helmet audio (UAC2)
                       │                                        ├─► SmartKnob ESP32-S3 (CDC+HID)
                       │                                        └─► RAK4631 LoRa (CDC)
                       │ USB 3.0 host ──► USB5744 4-port hub ───┬─► USB webcams ×3
                       │                                        └─► spare SS devices
                       │ USB host (dedicated) ──────────────────► Phone umbilical (ADB/tether)
                       │ USB-C / HDMI0(+melonHD) ───────────────► VITURE Beast XR glasses
                       │ HDMI1 ────────────────────────────────► debug display
                       │ PCIe Gen2 x1 ──► PI7C9X2G404 switch ──┬─► NVMe M.2 (Key-M 2280)
                       │                                        ├─► spare (Coral TPU)
                       │                                        └─► spare (2.5GbE NIC)
                       └───────────────────────────────────────────────┘
```

Two build configurations exist and this doc covers both:

- **Standalone CM5 build** (current README default): buttons + I²C sensors
  wired directly to the CM5 40-pin header; face via Adafruit Triple Matrix
  Bonnet; peripherals on stock USB ports.
- **Two-brain carrier board** (`hardware/carrier-board/`): CM5 drives *only*
  HUB75 + fans; the RP2354B coprocessor owns sensors, buttons, LEDs, servos,
  ADC, connected over USB-CDC.

---

## 3. CM5 — I/O Detail

### 3.1 Dedicated (non-GPIO) interfaces

| Interface | Connected to | Cable / connector |
|-----------|--------------|-------------------|
| CSI0 (CAM0) | OWLsight Left (libcamera id 0) | 22-pin 0.5 mm FFC, flip-lock |
| CSI1 (CAM1) | OWLsight Right (libcamera id 1) | 22-pin 0.5 mm FFC |
| USB-C (or HDMI0 → melonHD LT6711A) | VITURE Beast XR (DP Alt Mode video + USB data for SDK/IMU) | single USB-C |
| HDMI0 / HDMI1 | External display / debug fallback (ESD array on TMDS) | standard HDMI |
| USB 2.0 host ×2 (RP1) | 480 Mbps — feeds USB2514B peripheral hub; one port dedicated to phone umbilical | J9 / J11 |
| USB 3.0 host ×2 (RP1) | 5 Gbps Gen 1 — feeds USB5744 hub / high-bandwidth devices | |
| PCIe Gen 2 x1 | ~500 MB/s — expansion (NVMe etc., §7) | FFC/DF40 per carrier |
| 3.5 mm jack / HDMI audio | Audio outputs (`hw:Headphones`, `hw:vc4hdmi0`) | |

### 3.2 CM5 GPIO — standalone build (README default)

| BCM | Pin # | Function | Wiring |
|-----|-------|----------|--------|
| 17 | 11 | Button 1 (menu select / PiP left) | momentary switch → GND, internal pull-up (libgpiod v2) |
| 27 | 13 | Button 2 (boop snout / PiP right) | switch → GND |
| 22 | 15 | Button 3 (menu / restart) | switch → GND |
| 2 (SDA1) | 3 | I²C-1 data | BNO055 @0x28 · MPU-9250 @0x68 · MPR121 @0x5A (4.7 kΩ pull-ups on breakouts) |
| 3 (SCL1) | 5 | I²C-1 clock | same bus |
| — | 1 | 3.3 V | sensor VCC (3.3 V only — never 5 V on MPU-9250) |
| — | 6 | GND | common |

Up to **8 GPIO switches** total, each with configurable short/long-press
function, pull bias and polarity (`config.json → gpio.pins`).

> Note: buttons on 17/22/27 conflict with HUB75 pins below — in a HUB75 build
> move buttons to other free BCM pins or to a coprocessor.

### 3.3 CM5 GPIO — two-brain carrier board (HUB75 + control)

| BCM | Use | | BCM | Use |
|-----|-----|-|-----|-----|
| 4 | HUB75 OE | | 17 | HUB75 CLK |
| 5 | HUB75 R1 | | 20 | HUB75 D |
| 6 | HUB75 B1 | | 21 | HUB75 STB/LAT |
| 7 | `RP_RUN` (RP2354B reset) | | 22 | HUB75 A |
| 8 | `RP_BOOTSEL` (RP2354B boot) | | 23 | HUB75 B2 |
| 12 | HUB75 R2 | | 24 | HUB75 E |
| 13 | HUB75 G1 | | 26 | HUB75 B |
| 16 | HUB75 G2 | | 27 | HUB75 C |
| 18 | Fan zone 1 PWM (MOSFET gate) | | 19 | Fan zone 2 PWM |

Free: BCM 2, 3, 9, 10, 11, 14, 15, 25. BCM 0/1 = HAT ID EEPROM, leave alone.
All CM5 GPIO is 3.3 V, **not** 5 V-tolerant; HUB75 lines are buffered to 5 V
through 74AHCT245s (or the Adafruit Triple Matrix Bonnet, PID 6358, which
carries level shifters + 3 HUB75 ports + 5 V screw terminal + I²C header).

### 3.4 HUB75 connector (J2, 16-pin IDC)

```
 1 R1   2 G1    3 B1   4 GND   5 R2    6 G2    7 B2   8 E/GND
 9 A   10 B    11 C   12 D    13 CLK  14 LAT  15 OE  16 GND
```
Panel power comes from the dedicated `+5V_PANEL` rail, never the ribbon.
Ribbons < 20 cm. Current build (ProtoFace): 2× 64×32 1:16-scan panels
daisy-chained on port 1 (128×32 canvas), driven by Piomatter through the RP1
PIO (`/dev/pio0`). Avoid HUB75E-labelled panels for 32-row builds.

### 3.5 USB device map (as seen by the CM5)

| Device | udev symlink / ALSA name | Protocol |
|--------|--------------------------|----------|
| Teensy 4.1 face | `/dev/teensy` → ttyACM0 | CDC serial 115200 |
| SmartKnob | `/dev/smartknob` → ttyACM1 | CDC serial 115200 (framed binary) + HID dial |
| RAK4631 LoRa | `/dev/lora` → ttyACM2 | CDC serial 115200 |
| RP2350 helmet audio | `hw:CARD=HelmetAudio6Mic,DEV=0` — VID:PID `1209:B350` | USB Audio Class 2, stereo 48 kHz 16-bit |
| VITURE glasses | `hw:CARD=VITUREXRGlasses` + SDK over USB | DP Alt Mode + USB data |
| USB webcams ×≤3 | `/dev/video*` | UVC |
| Android phone | ADB / `usb0` network | scrcpy → v4l2loopback `/dev/video4` |
| RP2354B coprocessor | USB-CDC (carrier hub) | newline ASCII protocol |
| ESP32-C3 remote | USB-serial | ESP-NOW bridge |

---

## 4. RP2350 Helmet Audio Processor (sub-assembly)

- **Connection to CM5:** USB-A → USB-C, enumerates as UAC2 `Helmet Audio 6-Mic`
  (`1209:B350`). No other wiring to the CM5 — all mic I2S is internal to the
  board.
- **Mics:** 6× ICS-43434 MEMS in a hexagon for 360° capture. I2S L/R select
  pin pairs mics on shared data lines:

| Mic | L/R pin | Azimuth | | Mic | L/R pin | Azimuth |
|-----|---------|---------|-|-----|---------|---------|
| FRONT_L | GND | 330° | | SIDE_R | VDD | 90° |
| FRONT_R | VDD | 30° | | REAR_L | GND | 210° |
| SIDE_L | GND | 270° | | REAR_R | VDD | 150° |

- **Signal path:** mics → I2S → RP2350 (beamforming · NR · DOA) → USB UAC2 →
  CM5 ALSA capture → gain → output (VITURE / headphones / HDMI).
- **Fallback:** Teensy 4.1 `teensy_audio` firmware as a 6-ch USB bridge
  (I2S1 quad: data pins 8 & 6, SCK 21, WS 20; I2S2: data pin 5, SCK 33,
  WS 34; requires the 6-channel `usb_desc.h` patch), or ICS-43434s direct to
  CM5 I2S via `overlays/cm5-6mic.dts`.

---

## 5. RP2354B I/O Coprocessor (carrier board brain #2)

48 GPIO, self-powered from local `+3V3_RP`; links to the CM5 as a USB-CDC
device through the onboard USB2514B hub. SW1 (DPDT / TS3USB221A) switches its
single USB pair between **A: hub→CM5** and **B: J12 USB-C standalone
programming port** (VBUS sense only, never back-fed). SWD header + BOOTSEL/RUN
buttons for debug; UART0 console on GP0/GP1 (3.3 V only).

| GP | Net | Function | | GP | Net | Function |
|----|-----|----------|-|----|-----|----------|
| 0 | DBG_TX | UART0 debug TX | | 20–27 | SRV1–8 | 8× servo PWM (J20–J27, 3.3 V signal) |
| 1 | DBG_RX | UART0 debug RX | | 28–37 | BTN1–10 | buttons/boop, pull-up, active-low (J6) |
| 2 | MX_CLK | SPI0 SCK → MAX7219 | | 38–39 | — | spare |
| 3 | MX_DIN | SPI0 TX → MAX7219 | | 40–47 | AIN0–7 | ADC0–7 analog inputs |
| 4 | SDA0 | I²C0 sensors (J5/JX1 Qwiic) | | | | |
| 5 | SCL0 | I²C0 sensors | | | | |
| 6 | SENS_INT | sensor/expander interrupt | | | | |
| 7–10 | MX_CS1–4 | MAX7219 chain selects (SIO) | | | | |
| 11–15 | — | spare (12–15 HSTX-capable) | | | | |
| 16–19 | LED1–4_DAT | WS2812 zones ×4 (PIO, J4) | | | | |

- **I²C0 sensor bus (J5, Qwiic order GND/3V3/SDA/SCL + INT):** BNO055 @0x28,
  MPU-9250 @0x68, MPR121 boop @0x5A, BH1750 light @0x23, MCP23017 expanders
  @0x20–0x22, 4.7 kΩ pull-ups to `+3V3_RP`.
- **MAX7219 header (J3):** +5V_PANEL, GND, DIN(GP3), CLK(GP2), CS1–4
  (GP7–10) — all buffered to 5 V via 74AHCT245 (U10).
- **WS2812 header (J4):** +5V_LED, GND, DIN1–4 (GP16–19) via 74AHCT125 (U11),
  series R per line, bulk cap on rail.
- **Buttons (J6):** BTN1–10 to GND, internal pull-ups, debounce in firmware;
  BTN10 doubles as boop/capacitive trigger.
- **Servos (J20–J27):** SIG/+V/GND, `+V_SERVO` 5–6 V fused rail, no level
  shift (3.3 V signal is in-spec).

---

## 6. Other Controllers — Pinouts

### 6.1 SmartKnob (Smart-Knob-Redux, ESP32-S3)

USB composite device: CDC (ProtoHUD side-channel) + HID Generic Desktop Dial
with 3 buttons. SimpleFOC BLDC + MT6701 magnetic encoder.

| ESP32-S3 GPIO | Function |
|---------------|----------|
| 1 / 2 / 3 | BLDC phase A / B / C (driver IN1–3) |
| 9 | Driver enable |
| 13 / 12 / 38 | MT6701 SSI CS / CLK / MISO |
| 6 | Encoder push-switch (pull-up, → GND) |
| 7 | Back button |
| 8 | Extra button |

To CM5: USB-C only; framed binary UART protocol at 115200
(detents 0x81, wake 0x82, sleep timeout 0x83, range 0x84, haptics 0x85;
device→host: 0x01 cal ready, 0x02 sleep, 0x03 wake).

### 6.2 Button Coprocessor (firmware/button_coproc, RP2350 Pico 2/2 W class)

USB-CDC, newline ASCII protocol (`HELLO/BTN/PING`, `BOOP`, `TEMP`, `FAN`,
`SPI`, `LEDZ…`, `ADCREAD`). Pin map (RP2350B values; RP2350A fallbacks noted):

| GP | Function |
|----|----------|
| 2–9 | 8 buttons → GND (INPUT_PULLUP, active-low) |
| 39, 44, 12, 31, 32, 33 | TTP223 touch/boop pads 0–5 (active-high) — RP2350A: 0/1 for pads 0–1, 16/17/18 for pads 3–5. GP0/GP1 (UART0) stay free on RP2350B |
| 10 / 11 / 13 | MAX7219 SPI bridge: SPI1 SCK / TX / CS (8 MHz) |
| 14, 15 | Fan PWM zones (25 kHz) |
| 19 | DS18B20 1-Wire bus (4.7 kΩ → 3V3), up to 8 probes |
| 16 / 17 / 18 | Voice changer I2S: BCLK / WS / DOUT → TLV320DAC3100 |
| 20 / 21 | I²C0: TLV320 DAC @0x18 · MPR121 @0x5A · PCA9685 @0x40 |
| 22 | TLV320 reset (RP2350A: doubles as LED-zone data when voice off) |
| 40 (A: 26) | Mic ADC0 — electret → MAX9814 preamp → ADC |
| 34, 35, 36, 38 | 4 direct servo channels (fallback when no PCA9685) |
| 37 (A: 22) | WS2812/APA102 LED test zone data (APA102 clock: GP28) |
| 40, 41, 42 (A: 26–28) | ADC test inputs (flex/pots/battery sense) |

PCA9685 16-ch servo board (primary servo backend when fitted): I²C @0x40 on
GP20/21, logic VCC 3V3, servo V+ from an **external 5–6 V supply** sized for
all-stall (~1 A/servo), grounds common.

### 6.3 Teensy 4.1 face path

USB-CDC 115200 to CM5 (`/dev/teensy`); runs ProtoTracer; drives its own LED
chains. No GPIO to the CM5.

### 6.4 RAK4631 LoRa node

USB-CDC 115200 to CM5 (`/dev/lora`). Internal WisBlock wiring (fixed, do not
modify): NSS P1.10, DIO1 P1.15, RESET P1.06, BUSY P1.14, DIO2 → RF switch
(RAK3401 1 W booster optional). RAK12501 GPS plugs into WisBlock Slot A
(Serial1, 9600 NMEA) — no extra wiring. Antennas: LoRa 868/915 MHz + GPS.

---

## 7. Expansion & Add-Ons

### 7.1 PCIe → NVMe (and friends)

The CM5 exposes **1× PCIe Gen 2 x1 (~500 MB/s raw; Gen 3 unofficial)**. Fan it
out with a packet switch; all downstream devices share the single uplink lane.

| Part | Example P/N | Notes |
|------|-------------|-------|
| PCIe Gen 2 packet switch, 1↑ → 3↓ | **Diodes/Pericom PI7C9X2G404SL** (~$15) | public datasheet, integrated downstream clock buffer |
| Alternate switch | ASMedia ASM1184e (1→4, ~$8) / ASM1182e (1→2) | de-facto Pi 5 expander; no public datasheet — crib open Pi carrier layouts, or prototype with a Pineboards/Waveshare quad-M.2 board first |
| Bigger option | Broadcom/PLX PEX8606 | 6-lane, pricier, BGA |
| M.2 Key-M socket (2280) | TE 2199230-4 | one per downstream port used |
| NVMe 3.3 V rail | ≥3 A buck from +5 V (TPS62869 class) | NVMe peaks 8–10 W — never hang it off logic 3.3 V |
| PERST# / CLKREQ# | CM5 PERST# → switch; switch/GPIO → per-slot PERST#; CLKREQ# pulled per datasheet | |
| AC-coupling | 100 nF 0402 on every TX pair you place | CM5's own TX caps are on-module |

**Suggested slots:** NVMe SSD (recording/photo gallery storage) + 2 spares for
a **Coral TPU** (AI inference) and/or **2.5 GbE NIC**. Bandwidth reality:
everything shares ~500 MB/s — fine for one NVMe + a TPU, not four full-rate
drives.

### 7.2 USB hubs (onboard)

| Hub | Part | Feeds |
|-----|------|-------|
| USB 3.1 Gen 1 ×4 | Microchip **USB5744** (alt: TUSB8041AI, CYUSB3304) | USB cams, future SS devices; per-port TPS2553 VBUS switches ≥900 mA (1.5 A for VITURE if fed here) |
| USB 2.0 ×4 | Microchip **USB2514B** (alt: USB2517 7-port) | RP2354B CDC, RP2350 audio, SmartKnob, LoRa |

Support parts per hub: crystal (25/24 MHz), core regulator, AC-coupling caps
on SS TX pairs, ESD (TPD2EUSB30 / USBLC6-2SC6), decoupling.

### 7.3 melonHD (single-cable VITURE option)

Lontium LT6711A module: CM5 HDMI0 → USB-C DP 1.2 Alt Mode; CM5 USB 2.0 D±
routed past it to the same receptacle so the VITURE SDK/IMU still works. VBUS
sourced by the module (5 V/3 A strap, ~8–15 W budget); CM5 VBUS not connected;
HPD → spare GPIO; fixed-EDID quirk handled with `drm.edid_firmware`. Keep
HDMI1 as debug fallback. Test one module before committing copper.

### 7.4 I/O expansion

- **MCP23017 / MCP23S17** I²C/SPI GPIO expanders (JX1 Qwiic header, @0x20–0x22)
- **ADS1115** extra ADC, **PCA9685** 16-ch servo/PWM — same Qwiic bus
- 74HC165/74HC595 shift registers on the RP2354B SPI0 lane (spare CS on GP11–15)
- Spare interrupts: JX2 (SENS_INT GP6, INT1 on GP11)

### 7.5 Optional sensors & peripherals (all supported in software)

| Device | Bus / address | Purpose |
|--------|---------------|---------|
| BNO055 | I²C 0x28 | preferred absolute-orientation IMU |
| MPU-9250 / GY-9250 | I²C 0x68 (AD0→GND) | backup compass/IMU |
| MPU-6050 | I²C 0x68 (ProtoFace bonnet header) | head-tilt face offset |
| MPR121 | I²C 0x5A | capacitive boop electrodes |
| BH1750 | I²C 0x23 | ambient light |
| DS18B20 ×≤8 | 1-Wire (coproc GP19) | temperature probes |
| INA219 ×N | I²C | per-rail power telemetry → HUD |
| TTP223 / TCRT5000 | GPIO | boop touch/proximity |
| USB microphone | USB | ProtoFace mouth animation (I2S mics clash with HUB75) |
| SDL2 gamepad | USB/BT | optional input |

---

## 8. Power System

| Rail | Feeds | Sizing |
|------|-------|--------|
| `+5V` (main) | CM5, USB hubs/peripherals, buffer B-sides | CM5 4–5 A typ, 6.5 A peak incl. USB |
| `+5V_PANEL` | HUB75 panels (J2/J3) | ~2 A/panel moderate, ~8 A/panel full white — 2 panels: 5 V 10 A+ PSU minimum, own copper + bulk caps, fused |
| `+5V_LED` | WS2812 zones (J4) | fused, bulk cap, inrush isolated |
| `+V_SERVO` (5–6 V) | Servo headers J20–J27 | fused + bulk caps; all-stall sizing |
| `+3V3_RP` | RP2354B, I²C sensors, buffer A-sides | ~85–105 mA actual; spec ≥500 mA LDO/buck |
| NVMe 3.3 V | M.2 slot(s) | dedicated ≥3 A buck (§7.1) |

**Source chain:** Ryobi 40 V pack (belt/backpack) → fuse + reverse-polarity
FET + TVS + low-voltage cutoff → 40 V→5 V buck → **umbilical** (heavy 5 V run,
≤24 A, remote sense; may share a GX16/GX20 circular connector with the phone
USB pair — keep USB segregated from high current) → J1 helmet input (TVS +
bulk) → star distribution, common ground everywhere. Floating grounds corrupt
HUB75 data and can kill driver ICs — bond CM5, bonnet/buffers, panels, and
every PSU return together.

Panels are powered directly from the PSU in parallel (18 AWG+), never chained
through HUB75 OUT connectors.

---

## 9. Quick Cable Checklist

- [ ] 2× 22-pin CSI FFC (CM5 ↔ OWLsight L/R)
- [ ] 1× USB-C (CM5/melonHD ↔ VITURE Beast)
- [ ] 4× USB-A→USB-C (Teensy, SmartKnob, RAK4631, RP2350 audio)
- [ ] 1× USB umbilical (phone, shielded twisted pair)
- [ ] 2× 16-pin HUB75 IDC ribbons (<20 cm)
- [ ] Panel power runs, 18 AWG (PSU → each panel + bonnet screw terminal)
- [ ] Qwiic/JST-SH leads (BNO055, MPU-9250, MPR121, BH1750…)
- [ ] 3× (up to 8×/10×) button leads → GND
- [ ] Fan leads (2 zones, MOSFET-switched, flyback diodes)
- [ ] Servo leads ×≤8 (+V_SERVO rail)
- [ ] WS2812 zone leads ×≤4 (+5V_LED rail)
- [ ] LoRa + GPS antennas (RAK4631)
- [ ] 5 V main umbilical + GX16/GX20 connector
- [ ] M.2 2280 standoff/screw per NVMe slot (if fitted)
