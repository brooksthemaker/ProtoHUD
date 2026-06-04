# Carrier Board — Requirements

Spec for the ProtoHUD CM5 carrier. **Must-have** items are required for a
functional, in-spec board. **Nice-to-have** items improve robustness,
serviceability, or telemetry but can be cut for a v1.

---

## Must-have requirements

### M1 — CM5 mounting & I/O
- **R1.1** Dual 100-pin Hirose **DF40C-100DS-0.4V(51)** board-to-board
  connectors at the CM5-specified pitch and stack height.
- **R1.2** Decoupling and rail sequencing per the *CM5 Datasheet / IO Board
  design guide*. Follow Raspberry Pi's reference for bulk + per-pin caps.
- **R1.3** Break out the 40-pin GPIO header (2.54 mm) for bring-up/debug, with
  silk net names matching the firmware (see `README.md` signal map).

### M2 — Power
- **R2.1** Single primary **5 V input**, protected: fuse/polyfuse,
  reverse-polarity (P-FET), and a TVS for transient suppression.
- **R2.2** CM5 5 V rail sized for **≥ 5 A** (CM5 + USB peripherals).
- **R2.3** **Separate, high-current 5 V rail for HUB75 panel power** — *not*
  routed through the CM5 rail. Local bulk capacitance (≥ 1000 µF) near the
  panel connector. Common ground with the CM5 rail.
- **R2.4** **Separate, fused 5 V for WS2812** power injection at the LED
  connector.
- **R2.5** 3.3 V for sensors from the CM5 3.3 V rail (budget ≈ 47 mA today;
  see `src/main.cpp` `rail_currents_mA`). No local regulator required at the
  current load.

### M3 — Face display backend (HUB75 **or** MAX7219 — pick one)
Both backends are 5 V-logic and share one 3.3 V → 5 V buffer block; a jumper
(JP1) routes the buffered signals to whichever face connector is populated.

- **R3.1** **74AHCT245** (or AHCT244/541) octal buffer(s) powered at **5 V**,
  level-shifting the face signals from 3.3 V → 5 V. **Must be HCT/AHCT family**
  (TTL VIH ≈ 2 V), not HC (which needs VIH ≈ 3.5 V at a 5 V rail and would not
  read 3.3 V as high).
- **R3.2 (HUB75)** Buffer all **14** HUB75 signals (R1 G1 B1 R2 G2 B2 A B C D E
  CLK STB OE) to a standard **2×8 (16-pin) shrouded IDC** connector (J2).
- **R3.3** Optional series resistors (~33 Ω footprint) on CLK and high-speed
  lines to tame ringing on ribbon cables. Keep buffer-to-connector traces short.
- **R3.4 (MAX7219)** Buffer the MAX7219 chain signals — **DIN, CLK, CS×4** —
  to a header (J3) carrying 5 V, GND, DIN, CLK, CS1–CS4. DIN/CLK/CS are all
  CM5 → driver (unidirectional), so the same '245 covers them; DOUT daisies
  module-to-module and never returns to the CM5.
- **R3.5** Source the MAX7219 signals to support **both transports**
  (`src/face/max7219_chain.h`): hardware **SPI0** (DIN = BCM 10/MOSI, CLK =
  BCM 11/SCLK, CS = BCM 7/8) *and* **bit-banged GPIO** (shared DIN/CLK + per-
  chain CS). Use jumper/0 Ω selection so SPI0-MOSI isn't claimed by both WS2812
  and MAX7219 simultaneously (see N11).

### M4 — WS2812 accessory LEDs
- **R4.1** Level-shift the single data line (BCM 10 / SPI0 MOSI) 3.3 V → 5 V
  (one AHCT125/AHCT1G125 channel, or a '245 channel).
- **R4.2** LED power connector with the fused 5 V from R2.4 and shared ground.

### M5 — I²C sensor bus
- **R5.1** Break out **I²C bus 1** (SDA = BCM 2, SCL = BCM 3) with **4.7 kΩ**
  pull-ups to 3.3 V.
- **R5.2** Sensor connectors for BNO055 (0x28), MPU9250 (0x68), MPR121 boop
  (0x5A), BH1750 light (0x23). Standardized keyed connectors preferred.

### M6 — GPIO buttons / boop
- **R6.1** Break out button/boop GPIO at 3.3 V with series resistors; route to
  lines **not** claimed by HUB75 (see contention note in `README.md`).
- **R6.2** Pull bias selectable to match config (`active_low` momentary-to-GND).

### M7 — Cameras
- **R7.1** **2× 22-pin 0.5 mm FFC** CSI connectors (CM5/Pi-5 style) for the two
  CSI eyes (`csi_expected: 2`).

### M8 — USB peripheral stack
- **R8.1** Provide USB host breakout for the always-USB devices: **RP2350
  helmet audio (UAC2)**, **smart knob (ACM)**, **LoRa RAK4631 (ACM)**, and the
  **VITURE glasses** (USB-C + display). USB cameras as needed.
- **R8.2** Account for CM5's limited native USB count — see N1 (onboard hub).

### M9 — Provisioning
- **R9.1** **nRPIBOOT** jumper/header + a USB device port so the CM5 eMMC can be
  flashed with `rpiboot` without removing the module.

---

## Nice-to-have

- **N1 — Onboard USB 2.0 hub** (e.g. 4–7 port, USB2514B/USB2517) to consolidate
  RP2350 audio + knob + LoRa + cameras + VITURE behind the CM5's USB, freeing
  cabling and avoiding an external dongle hub.
- **N2 — Per-rail telemetry** (INA219/INA260 on 3.3 V / 5 V / panel / LED rails)
  so ProtoHUD can show live power draw alongside the existing rail estimates.
- **N3 — e-Fuse per rail** (e.g. TPS259x) for latch-off short protection
  instead of slow polyfuses.
- **N4 — RTC battery** connector/coin-cell holder for the CM5's onboard RTC
  (keeps wall-clock across power-off).
- **N5 — PWM fan header** (4-pin) + thermal pad area for active cooling.
- **N6 — Power-good / diagnostic LEDs** per rail, plus a heartbeat LED.
- **N7 — Spare buffer channels** from the HUB75 '245s broken out to a header
  for ad-hoc 5 V signals.
- **N8 — STEMMA QT / Qwiic (JST-SH 1 mm)** connectors on the I²C bus for
  solderless sensor swaps.
- **N9 — Test points** on every rail and key signals (CLK, LAT, OE, SDA, SCL).
- **N10 — HDMI breakout** for the CM5's two HDMI outputs (VITURE / external
  display) with proper ESD parts.
- **N11 — Jumper/0 Ω-selectable GPIO mapping** for the HUB75-contended lines so
  one board serves both HUB75 and MAX7219 face backends.
- **N12 — Mounting holes + outline** matched to the helmet shell, with
  connector placement chosen for cable routing inside the helmet.
- **N13 — Reverse-mount/ low-profile** connectors where the board sits close to
  the shell.

---

## Out of scope (lives elsewhere)

- **Microphone array + audio DSP** — handled entirely by the **RP2350 helmet
  audio board** (6× ICS-43434/INMP441, beamforming/NR/DOA), which presents UAC2
  over USB. The carrier only needs to provide that USB connection (R8.1).
- HUB75 panel internals; LoRa RF; VITURE internals.
