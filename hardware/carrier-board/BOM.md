# Carrier Board — Bill of Materials

Indicative parts list for the [requirements](REQUIREMENTS.md). Reference part
numbers are starting points, not a locked sourcing list — substitute pin- and
spec-compatible equivalents. Quantities assume a **4-panel HUB75** face on the
CM5 **plus** the **RP2354B I/O coprocessor** with the default sensor set,
**8 servos**, and **4 WS2812 zones**.

`Req` ties each line back to a requirement (M = must-have, N = nice-to-have).

## Core / compute

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 1 | CM5 board-to-board connector | Hirose DF40C-100DS-0.4V(51) | 2 | M1 | Matched stack height to CM5 |
| 2 | 40-pin GPIO header (2.54 mm) | generic | 1 | M1 | Debug/bring-up |

## Power

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| — | *External power unit (off-helmet):* Ryobi 40 V dock, ≥50 V fuse/RP-FET/TVS, **40 V→5 V buck** (10–30 A, heatsinked), low-voltage cutoff | — | 1 | M2.1/2 | belt/back; outputs 5 V on the umbilical. See POWER.md |
| 3 | 5 V input connector (helmet) | XT60 / fat polarized | 1 | M2.1 | umbilical from external unit; 10–12 AWG |
| 3b | Bulk cap at J1 | ≥ 2200 µF low-ESR | 1 | M2.1 | rides umbilical drop/spikes |
| 5 | Reverse-polarity P-FET (5 V) | e.g. DMP3098L | 1 | M2.1 | helmet-side, 5 V-class |
| 6 | TVS diode (5 V) | SMBJ5.0A | 1 | M2.1 | helmet-side transient clamp |
| 7 | Bulk cap, CM5 rail | 470–1000 µF, low-ESR | 1–2 | M2.2 | |
| 8 | Bulk cap, HUB75 rail | ≥ 1000 µF | 1+ | M2.3 | At panel connector |
| 9 | Bulk cap, WS2812 rail | 470–1000 µF | 1 | M2.4 | |
| 10 | Fuse, WS2812 rail | per LED count | 1 | M2.4 | |
| 11 | Decoupling caps (assorted) | 0.1 µF / 1 µF / 10 µF | many | M1.2 | Per CM5 design guide |

## HUB75 face buffer (critical) — CM5 side

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 12 | Octal buffer, **AHCT** | SN74AHCT245PWR | 2 | M3.1 | **U1/U2** · 5 V VCC, 3.3 V TTL in → 5 V out. **Not** HC. HUB75 only (MAX7219 buffer is U10, RP2354B side). |
| 13 | HUB75 IDC header, 2×8 shrouded | 2.54 mm boxed | 1–4 | M3.2 | J2 · one per chain |
| 14 | Series resistors (CLK/LAT/OE) | 33 Ω 0603 | ~3 | M3.3 | Optional-fit footprints |
| 15 | Buffer decoupling | 0.1 µF 0603 | 2 | M3.1 | One per '245 (U1/U2) |

> The old backend-select jumper (JP1) and MAX7219-source jumper (JP2) are gone:
> HUB75 (CM5) and MAX7219 (RP2354B) are on different brains. MAX7219's header
> (J3) and buffer (U10) are in the **RP2354B level shifting** section below.

## WS2812 accessory LEDs

The level-shift buffer (U11) is listed once under **RP2354B level shifting**
(item 55) — these are the LED-side connectors only.

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 19 | LED power+data connectors | JST (keyed) | 4 | M6.2 | 5 V, GND, DIN · one per WS2812 zone (`LED1..4_DAT`) |

## I²C sensors

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 20 | I²C pull-up resistors | 4.7 kΩ 0603 | 2 | M5.1 | `SDA0`/`SCL0` → **+3V3_RP** (RP2354B bus, not CM5) |
| 21 | Sensor connectors | keyed 5-pin (or STEMMA QT) | 4 | M5.2 | BNO055/MPU9250/MPR121/BH1750 · 3V3/GND/SDA/SCL + **`SENS_INT`** |

## GPIO / buttons

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 22 | Button series resistors | 330 Ω–1 kΩ 0603 | per pin | M6.1 | |
| 23 | ESD diode array (optional) | e.g. USBLC6 / TVS array | as needed | M6.1 | Boop/button lines |
| 24 | Button/boop connectors | keyed | as needed | M6.1 | Avoid HUB75-claimed BCM lines |

## Cameras / USB / provisioning

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 25 | CSI FFC connector, 22-pin 0.5 mm | flip-lock | 2 | M7.1 | Two CSI eyes |
| 26 | Downstream USB-C receptacles | USB 3.1 (full-feature, 24-pin) | 4 | M8.1 / N1 | **J40–J43** · the 4 hub ports (SuperSpeed). Peripherals (RP2350 audio, knob, LoRa, VITURE, USB cams) plug in here |
| 27 | nRPIBOOT jumper + USB port | header + micro/USB-C | 1 | M9.1 | eMMC flashing |

## USB 3.1 hub (VL817) — CM5 side

Replaces the former USB 2.0 hub. VL817 is a 4-port **USB 3.1 Gen1 (5 Gbps)**
hub; pin-strap configured (no firmware/EEPROM required). Upstream = one CM5
**USB 3.0** port (SuperSpeed + USB 2.0). All four downstream ports are USB-C
(item 26). See [`CONNECTORS.md`](CONNECTORS.md) and the `kicad/usb.kicad_sch` sheet.

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 26a | USB 3.1 Gen1 4-port hub | **VIA VL817-Q7** (QFN-68) | 1 | N1 | **U7** · strap-config; integrated 3.3/1.2 V regulators (off `+5V`, not `+3V3_RP`) |
| 26b | Hub crystal + load caps | 25 MHz + 2× 18 pF | 1 | N1 | Y2 · check VL817 CL spec for cap value |
| 26c | Hub decoupling | 1 µF (VDD33/VDD12) + 0.1 µF | several | N1 | VDD33/VDD12 are internal-reg outputs — decouple, don't drive |
| 26d | Reset / strap resistors | 10 kΩ + 0.1 µF | 3 | N1 | RESET RC, SELF_PWR strap |
| 26e | SuperSpeed AC-coupling caps | 0.1 µF 0402 | 10 | N1 | 2 per TX pair (upstream + 4 ports); place at the transmitter |
| 26f | CC pull-ups (Rp, DFP) | 56 kΩ 1% | 8 | N1 | 2 per USB-C port; advertise default USB current (host side) |
| 26g | Per-port VBUS PTC | resettable fuse ~1 A | 4 | N1 | F4–F7 · current-limit each port (or use a TPS2553 load switch) |
| 26h | USB2 ESD array | USBLC6-2SC6 | 4 | N1 | U20–U23 · on each port's D+/D- |
| 26i | SuperSpeed ESD array | TPD4EUSB30 / equiv | 4 | N1 | on each port's active SS pair (layout-time) |

## Nice-to-have add-ons

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 28 | USB hub controller | *moved → "USB 3.1 hub (VL817)" section (item 26a)* | — | N1 | Now USB 3.1 Gen1 (VL817), core to the design |
| 29 | Current-sense monitors | INA219 / INA260 | 1–4 | N2 | Per-rail telemetry to HUD |
| 30 | e-Fuse | TPS259x | 1–4 | N3 | Latch-off short protection |
| 31 | RTC coin-cell holder + cell | CR1220 + holder | 1 | N4 | CM5 onboard RTC backup |
| 32 | PWM fan header (4-pin) | 2.54 mm | 1 | N5 | Active cooling |
| 33 | Diagnostic LEDs + resistors | 0603 LED + R | several | N6 | Power-good / heartbeat |
| 34 | STEMMA QT / Qwiic connectors | JST-SH 1 mm | as needed | N8 | Solderless sensors |
| 35 | Test points | loop / pad | many | N9 | Rails + CLK/LAT/OE/SDA/SCL |
| 36 | HDMI connectors + ESD | micro/full HDMI, ESD array | 1–2 | N10 | CM5 dual HDMI / VITURE |
| 36b | Fan headers (4-pin) | PC-fan 2.54 mm | 2–4 | N5 | J12 · up to 4 fans in 2 zones |
| 36c | Low-side N-MOSFET + gate R/pulldown | e.g. AO3400 / IRLML | 2 | N5 | one per fan zone (gate ← BCM 18/19) |
| 36d | Flyback diode | e.g. 1N5819 | 2–4 | N5 | across each fan |

## I/O expansion (optional — buttons + LEDs, keep options open)

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 37 | I²C GPIO expander | MCP23017 | 1–8 | N14 | 16 bidir GPIO each; addr `0x20–0x22`; buttons + LED outputs |
| 38 | INT pull-up + header | 10 kΩ + INTA → BCM 25 | 1 | N14 | interrupt-driven button reads |
| 39 | Spare I²C / STEMMA QT header | JST-SH 1 mm | 1–2 | N15 | drop-in 2nd expander / ADS1115 / PCA9685 |
| 40 | (future) analog ADC | ADS1115 | 0–1 | N15 | 4-ch 16-bit, addr `0x48–0x4B` |
| 41 | (future) PWM / LED driver | PCA9685 | 0–1 | N15 | 16-ch PWM, addr `0x40–0x46` |
| 42 | (alt) SPI expander / shift regs | MCP23S17 / 74HC165 / 74HC595 | 0+ | N16 | second lane on free SPI0 |

## RP2354B I/O coprocessor

The second brain — owns sensors, WS2812, buttons, MAX7219, and servos; talks to
the CM5 as USB-CDC. See [`RP2354-IO.md`](RP2354-IO.md). *(Req tags `M-RP*` are
placeholders pending the two-brain rev of [`REQUIREMENTS.md`](REQUIREMENTS.md).)*

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 43 | RP2354B MCU (QFN-80) | Raspberry Pi RP2354B | 1 | M-RP | 48 GPIO, 2 MB on-package stacked flash (no external QSPI) |
| 44 | Crystal, 12 MHz | e.g. ABM8-12.000MHZ | 1 | M-RP | XIN/XOUT · required for native USB |
| 45 | Crystal load caps | 2× 15 pF 0603 (per XTAL spec) | 2 | M-RP | tune to crystal C_L |
| 46 | Decoupling caps, RP2354B | 0.1 µF 0603 (per IOVDD/DVDD/USB pin) + bulk | ~10 | M-RP | Per RP2350 hardware design guide |
| 47 | Core-regulator caps | 1 µF/4.7 µF (VREG IN/OUT) | 2–3 | M-RP | internal core LDO; per RP2350 design guide |
| 48 | Core-regulator inductor + caps | only if switched-mode VREG scheme used | 0–1 | M-RP | omit if internal LDO mode; per chosen power scheme |
| 49 | 3.3 V regulator for **+3V3_RP** | AP2112K-3.3 (LDO) or small buck | 1 | M-RP | off `+5V`, ≥ 500 mA · feeds RP core/IO + sensors + buffer A-side |
| 50 | BOOTSEL button | SMD tact | 1 | M-RP | UF2 bootloader entry |
| 51 | RUN/reset button | SMD tact | 1 | M-RP | RUN pin reset |
| 52 | SWD debug header | 1×4 (SWCLK/SWDIO/GND/3V3) or Cortex 2×5 | 1 | M-RP | probe flash + live debug |
| 53 | UART debug-console header | 1×3/1×4 2.54 mm | 1 | M-RP | `DBG_TX`/`DBG_RX` (GP0/GP1) |

## RP2354B level shifting

5 V-logic loads shift on the RP2354B side (`74AHCT*` TTL VIH reads 3.3 V as
high). Servos and I²C sensors stay 3.3 V-native.

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 54 | Octal buffer, **AHCT** | SN74AHCT245PWR | 1 | M-RP | **U10** · 5 V VCC · MAX7219 `MX_DIN`/`MX_CLK`/`MX_CS1..4` (3.3→5 V) |
| 55 | Quad buffer, **AHCT** | SN74AHCT125 | 1 | M-RP | **U11** · 5 V VCC · WS2812 ×4 `LED1..4_DAT` (3.3→5 V). *(same part as item 18)* |
| 56 | Buffer decoupling | 0.1 µF 0603 | 2 | M-RP | one per buffer (U10/U11) |

## USB selector / programming (RP2354B)

RP2350 has one USB pair — shared between a **dedicated CM5 USB host port** (the
CDC link; SW1 position A) and a standalone program port (J12; position B) via the
SW1 selector. The RP2354B no longer hangs off the on-board hub — it connects
directly to the CM5's second USB 3.0 port (USB 2.0 lanes). See
[`RP2354-IO.md`](RP2354-IO.md#usb-selector-cm5-hub--standalone-port).

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 57 | USB 2.0 selector — **option A** | DPDT slide switch (**SW1**) | 1 | M-RP | manual D+/D− steer (CM5 port ⇄ J12); fine for full-speed USB |
| 57b | USB 2.0 selector — **option B** | TS3USB221A mux | 1 | M-RP | alt to SW1 · cleaner electronic switching (pick one) |
| 58 | Standalone USB-C receptacle | USB 2.0 Type-C | 1 | M-RP | **J12** · isolated program port (VBUS sense only, do not back-feed) |
| 59 | USB ESD protection | USBLC6-2 | 1 | M-RP | on the RP2354B `RP_DP/RP_DM` pair |

## Servos (RP2354B PWM)

8 PWM channels (GP20–27) → 8 headers; V+ and GND are the shared `+V_SERVO` rail.

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 60 | 3-pin servo headers, 2.54 mm | generic | 8 | M-RP | **J20–J27** · pin order **SIG / +V_SERVO / GND** (`SRV1..8`) |
| 61 | Servo-rail fuse | per servo count/stall | 1 | M-RP | protects `+V_SERVO` (fused tap off `+5V`) |
| 62 | Servo-rail bulk cap | ≥ 1000 µF low-ESR | 1+ | M-RP | rides stall surges; isolates logic |
| 63 | (optional) 6 V buck for `+V_SERVO` | small step regulator | 0–1 | M-RP | **optional** · only if 6 V servos used; else rail = `+5V` |

---

### Not on this board
6× **ICS-43434 / INMP441** mics and the audio DSP live on the **RP2350 helmet
audio board**, which connects to the carrier as a **USB UAC2** device (item 26).
See [`../../firmware/teensy_audio/README.md`](../../firmware/teensy_audio/README.md)
and the audio section of the top-level `README.md`.
