# Carrier Board — Bill of Materials

Indicative parts list for the [requirements](REQUIREMENTS.md). Reference part
numbers are starting points, not a locked sourcing list — substitute pin- and
spec-compatible equivalents. Quantities assume a **4-panel HUB75** face build
with the default sensor set.

`Req` ties each line back to a requirement (M = must-have, N = nice-to-have).

## Core / compute

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 1 | CM5 board-to-board connector | Hirose DF40C-100DS-0.4V(51) | 2 | M1 | Matched stack height to CM5 |
| 2 | 40-pin GPIO header (2.54 mm) | generic | 1 | M1 | Debug/bring-up |

## Power

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 3 | 5 V input connector | XT30 / barrel / screw term | 1 | M2.1 | Main 5 V in |
| 4 | Input fuse / polyfuse | per current budget | 1 | M2.1 | |
| 5 | Reverse-polarity P-FET | e.g. DMP3098L / SiR | 1 | M2.1 | Ideal-diode style |
| 6 | TVS diode (5 V) | SMBJ5.0A | 1 | M2.1 | Transient clamp |
| 7 | Bulk cap, CM5 rail | 470–1000 µF, low-ESR | 1–2 | M2.2 | |
| 8 | Bulk cap, HUB75 rail | ≥ 1000 µF | 1+ | M2.3 | At panel connector |
| 9 | Bulk cap, WS2812 rail | 470–1000 µF | 1 | M2.4 | |
| 10 | Fuse, WS2812 rail | per LED count | 1 | M2.4 | |
| 11 | Decoupling caps (assorted) | 0.1 µF / 1 µF / 10 µF | many | M1.2 | Per CM5 design guide |

## HUB75 level shifting (critical)

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 12 | Octal buffer, **AHCT** | SN74AHCT245PWR | 2 | M3.1 | 5 V VCC, 3.3 V TTL in → 5 V out. **Not** HC. |
| 13 | HUB75 IDC header, 2×8 shrouded | 2.54 mm boxed | 1–4 | M3.2 | One per chain |
| 14 | Series resistors (CLK/data) | 33 Ω 0603 | ~14 | M3.3 | Optional-fit footprints |
| 15 | Buffer decoupling | 0.1 µF 0603 | 2 | M3.1 | One per '245 |

## WS2812 accessory LEDs

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 16 | Single-gate buffer (3.3→5) | SN74AHCT1G125 | 1 | M4.1 | Or reuse a '245 channel |
| 17 | LED power+data connector | JST (keyed) | 1 | M4.1/2 | 5 V, GND, DIN |

## I²C sensors

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 18 | I²C pull-up resistors | 4.7 kΩ 0603 | 2 | M5.1 | SDA/SCL → 3.3 V |
| 19 | Sensor connectors | keyed 4-pin (or STEMMA QT) | 4 | M5.2 | BNO055/MPU9250/MPR121/BH1750 |

## GPIO / buttons

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 20 | Button series resistors | 330 Ω–1 kΩ 0603 | per pin | M6.1 | |
| 21 | ESD diode array (optional) | e.g. USBLC6 / TVS array | as needed | M6.1 | Boop/button lines |
| 22 | Button/boop connectors | keyed | as needed | M6.1 | Avoid HUB75-claimed BCM lines |

## Cameras / USB / provisioning

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 23 | CSI FFC connector, 22-pin 0.5 mm | flip-lock | 2 | M7.1 | Two CSI eyes |
| 24 | USB host connectors / headers | USB-A / JST | as needed | M8.1 | RP2350 audio, knob, LoRa, VITURE, USB cams |
| 25 | nRPIBOOT jumper + USB port | header + micro/USB-C | 1 | M9.1 | eMMC flashing |

## Nice-to-have add-ons

| # | Part | Example P/N | Qty | Req | Notes |
|---|------|-------------|-----|-----|-------|
| 26 | USB 2.0 hub controller | USB2514B / USB2517 | 1 | N1 | Consolidate USB peripherals |
| 27 | Current-sense monitors | INA219 / INA260 | 1–4 | N2 | Per-rail telemetry to HUD |
| 28 | e-Fuse | TPS259x | 1–4 | N3 | Latch-off short protection |
| 29 | RTC coin-cell holder + cell | CR1220 + holder | 1 | N4 | CM5 onboard RTC backup |
| 30 | PWM fan header (4-pin) | 2.54 mm | 1 | N5 | Active cooling |
| 31 | Diagnostic LEDs + resistors | 0603 LED + R | several | N6 | Power-good / heartbeat |
| 32 | STEMMA QT / Qwiic connectors | JST-SH 1 mm | as needed | N8 | Solderless sensors |
| 33 | Test points | loop / pad | many | N9 | Rails + CLK/LAT/OE/SDA/SCL |
| 34 | HDMI connectors + ESD | micro/full HDMI, ESD array | 1–2 | N10 | CM5 dual HDMI / VITURE |

---

### Not on this board
6× **ICS-43434 / INMP441** mics and the audio DSP live on the **RP2350 helmet
audio board**, which connects to the carrier as a **USB UAC2** device (item 24).
See [`../../firmware/teensy_audio/README.md`](../../firmware/teensy_audio/README.md)
and the audio section of the top-level `README.md`.
