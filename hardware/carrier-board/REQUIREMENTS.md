# Carrier Board — Requirements

Spec for the ProtoHUD **two-brain** carrier: a **CM5** that drives only HUB75,
and an **RP2354B I/O coprocessor** that owns every other peripheral over USB-CDC
(see [`RP2354-IO.md`](RP2354-IO.md)). **Must-have** items are required for a
functional, in-spec board. **Nice-to-have** items improve robustness,
serviceability, or telemetry but can be cut for a v1.

---

## Must-have requirements

### M1 — CM5 mounting & I/O
- **R1.1** Dual 100-pin Hirose **DF40C-100DS-0.4V(51)** board-to-board
  connectors at the CM5-specified pitch and stack height.
- **R1.2** Decoupling and rail sequencing per the *CM5 Datasheet / IO Board
  design guide*. Follow Raspberry Pi's reference for bulk + per-pin caps.
- **R1.3** Break out a few spare CM5 GPIO + I²C/UART to a 2.54 mm debug header
  for bring-up. The CM5 GPIO is otherwise **HUB75-only** (M3).

### M2 — Power
- **R2.1** Single primary **5 V input**, protected: fuse/polyfuse,
  reverse-polarity (P-FET), and a TVS for transient suppression.
- **R2.2** CM5 5 V rail sized for **≥ 5 A** (CM5 + USB peripherals).
- **R2.3** **Separate, high-current 5 V rail for HUB75 panel power** (`+5V_PANEL`)
  — *not* routed through the CM5 rail. Local bulk capacitance (≥ 1000 µF) near
  the panel connector. Common ground.
- **R2.4** **Separate, fused 5 V for WS2812** (`+5V_LED`) power injection at the
  LED connector(s).
- **R2.5** **Separate, fused 5–6 V servo rail** (`+V_SERVO`) with bulk caps
  (≥ 1000 µF) — sized for the servo set's stall/inrush so surges don't disturb
  logic. Optional dedicated 6 V buck if 6 V servos are used.
- **R2.6** **Local 3.3 V rail** (`+3V3_RP`) from an LDO/buck off 5 V (≥ 500 mA),
  feeding the **RP2354B**, the I²C sensors, and the 3.3 V side of the buffers.
  Sensors no longer draw from the CM5 3V3.

### M3 — HUB75 face (CM5 side)
- **R3.1** **74AHCT245** (or AHCT244/541) octal buffer(s) at **5 V**, level-
  shifting the 14 HUB75 signals 3.3 V → 5 V. **Must be HCT/AHCT family**
  (TTL VIH ≈ 2 V), not HC.
- **R3.2** Buffer all **14** HUB75 signals (R1 G1 B1 R2 G2 B2 A B C D E CLK STB
  OE) to a standard **2×8 (16-pin) shrouded IDC** connector (J2). Source pins =
  CM5 `kHub75` map ([`PINMAP.md`](PINMAP.md)).
- **R3.3** Optional series resistors (~33 Ω) on CLK/LAT/OE to tame ringing.
  Keep buffer-to-connector traces short.
- **R3.4** HUB75 is the **only** consumer of CM5 GPIO. No backend-select jumper
  is needed — MAX7219 lives on the RP2354B (M5), so both can run at once.

### M4 — RP2354B I/O coprocessor (core)
- **R4.1** **RP2354B** (QFN-80, 48 GPIO, 2 MB on-package flash) with a
  **12 MHz crystal** + load caps (required for USB) and decoupling/core-regulator
  parts per the RP2350 hardware design guide.
- **R4.2** RP2354B enumerates to the CM5 as a **USB-CDC ACM** device through the
  onboard hub (M11), extending the `coproc_inputs` protocol
  ([`../../docs/coprocessor-input.md`](../../docs/coprocessor-input.md)).
- **R4.3** **Two programming paths:** (a) in-system over the CM5-hub USB
  (BOOTSEL/UF2), and (b) a **standalone USB-C port** (J12) selected by a
  **USB selector SW1** that disconnects the RP2354B's USB pair from the hub.
- **R4.4** **BOOTSEL** button, **RUN/reset** button, and an **SWD header**
  (SWCLK/SWDIO/GND/3V3) for debug-probe flashing.
- **R4.5** USB selector = DPDT slide switch (full-speed-adequate) or a
  `TS3USB221A` mux; **isolate J12 VBUS** (MCU is self-powered from `+3V3_RP`).
  Add ESD (USBLC6-2) on the RP2354B USB pair.

### M5 — MAX7219 face backend (RP2354B side)
- **R5.1** **74AHCT245** (U10) @ 5 V buffering **DIN, CLK, CS×4**
  (`MX_DIN`=GP3, `MX_CLK`=GP2, `MX_CS1..4`=GP7–GP10) → header **J3**
  (5 V, GND, DIN, CLK, CS1–CS4). All MCU → driver (unidirectional); DOUT daisies
  module→module and never returns.
- **R5.2** Source is the RP2354B **SPI0** (single transport — no bit-bang/jumper
  needed; the old SPI0/WS2812 contention is gone, they're separate pins/PIO).

### M6 — WS2812 accessory LEDs (RP2354B side)
- **R6.1** Level-shift up to **4 data zones** (`LED1..4_DAT` = GP16–GP19,
  PIO-driven) 3.3 V → 5 V with a **74AHCT125** (U11, quad).
- **R6.2** LED power connector(s) with the fused `+5V_LED` (R2.4) and shared GND.

### M7 — I²C sensor bus (RP2354B side)
- **R7.1** Break out **RP2354B I²C0** (`SDA0`=GP4, `SCL0`=GP5) with **4.7 kΩ**
  pull-ups to `+3V3_RP`, plus a `SENS_INT` line (GP6).
- **R7.2** Sensor connectors for BNO055 (0x28), MPU9250 (0x68), MPR121 boop
  (0x5A), BH1750 light (0x23). Keyed connectors / STEMMA QT preferred.

### M8 — Buttons / boop (RP2354B side)
- **R8.1** Break out button/boop GPIO (`BTN1..10` = GP28–GP37) at 3.3 V with
  internal pull-ups; MCU does debounce/long-press.
- **R8.2** Pull bias selectable to match config (`active_low` momentary-to-GND).

### M9 — Servos (RP2354B side)
- **R9.1** **8 servo channels** (`SRV1..8` = GP20–GP27, PWM) on **eight 3-pin
  headers** (J20–J27), pin order **SIG / +V_SERVO / GND**.
- **R9.2** Servo signal driven directly at **3.3 V** (accepted by hobby/digital
  servos); optional 74AHCT buffer footprint for 5 V signal if needed.
- **R9.3** Servo V+ from the dedicated `+V_SERVO` rail (R2.5), **not** logic 5 V.

### M10 — Cameras (CM5 side)
- **R10.1** **2× 22-pin 0.5 mm FFC** CSI connectors (CM5/Pi-5 style) for the two
  CSI eyes (`csi_expected: 2`).

### M11 — USB peripheral stack
- **R11.1** **Onboard USB 2.0 hub** (e.g. USB2514B/USB2517) consolidating the
  RP2354B (CDC), **RP2350 helmet audio (UAC2)**, **smart knob (ACM)**, **LoRa
  RAK4631 (ACM)**, **VITURE glasses**, and USB cameras behind the CM5's USB.
- **R11.2** Account for CM5's native USB count; dedicate one CM5 host port to the
  phone uplink (J11) separate from the hub.

### M12 — Provisioning
- **R12.1** **nRPIBOOT** jumper/header + a USB device port so the CM5 eMMC can be
  flashed with `rpiboot` without removing the module.
- **R12.2** RP2354B programmable in-system and standalone per **M4.3** (no
  removal needed).

---

## Nice-to-have

- **N1 — Per-rail telemetry** (INA219/INA260 on 5 V / panel / LED / servo /
  3V3 rails) so ProtoHUD can show live power draw. On the RP2354B I²C0 (or CM5
  I²C if preferred).
- **N2 — e-Fuse per rail** (e.g. TPS259x) for latch-off short protection
  instead of slow polyfuses — especially the servo and panel rails.
- **N3 — RTC battery** connector/coin-cell holder for the CM5's onboard RTC.
- **N4 — PWM fan header** (4-pin) + thermal pad area — drivable by the RP2354B
  (a spare PWM) or the CM5.
- **N5 — Power-good / diagnostic LEDs** per rail + heartbeat LEDs on both brains.
- **N6 — Spare buffer channels** from the '245/'125 broken out to a header.
- **N7 — STEMMA QT / Qwiic (JST-SH 1 mm)** on the RP2354B I²C0 for solderless
  sensor swaps.
- **N8 — Test points** on every rail and key signals (HUB75 CLK/LAT/OE, SDA0/
  SCL0, MX_*, servo rail, USB pairs).
- **N9 — HDMI breakout** for the CM5's two HDMI outputs (VITURE / external
  display) with proper ESD parts.
- **N10 — Servo current sense** (per-rail or per-bank) so the HUD can show servo
  load / detect stalls.
- **N11 — More servo channels** — the RP2354B has 24 PWM; break additional
  channels if the build needs > 8 (watch the `+V_SERVO` rail budget).
- **N12 — Mounting holes + outline** matched to the helmet shell, with
  connector placement for cable routing inside the helmet.
- **N13 — Reverse-mount / low-profile** connectors where the board sits close to
  the shell.
- **N14 — I²C expander(s)** on the RP2354B I²C0 — MCP23017 / ADS1115 / PCA9685 —
  only if the RP2354B's ~10 spare GPIO + 8 ADC are exhausted. Addresses
  `0x20–0x22` / `0x48–0x4B` / `0x40–0x46`; route INT to a spare RP2354B GPIO.
  See [`IO-EXPANSION.md`](IO-EXPANSION.md).
- **N15 — Spare expansion header** (RP2354B spare GPIO GP11–15/GP38–39 + ADC
  GP40–47) and a STEMMA QT for drop-in parts with no respin.
- **N16 — Second SPI/shift-register lane** via RP2354B PIO (SPI0 is taken by
  MAX7219) if the I²C bus fills.

---

## Out of scope (lives elsewhere)

- **Microphone array + audio DSP** — handled by the **separate RP2350 helmet
  audio board** (6× ICS-43434/INMP441, beamforming/NR/DOA), UAC2 over USB. The
  carrier only provides that USB connection (R11.1). This is a *different* MCU
  from the RP2354B I/O coprocessor.
- HUB75 panel internals; LoRa RF; VITURE internals.
