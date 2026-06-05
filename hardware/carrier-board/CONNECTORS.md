# Carrier Board — connector pinouts

Pin-by-pin for every connector in the [block diagram](BLOCK-DIAGRAM.md) and
[connector schedule](BLOCK-DIAGRAM.md#connector-schedule). Signal sources/BCM
lines come from [`PINMAP.md`](PINMAP.md). **Domain** = the logic/voltage a pin
sits at: `5V-buf` (level-shifted output), `3V3` (direct CM5), `PWR`, or a bus
standard.

Legend: **Dir** is relative to the CM5 — `OUT` = CM5→peripheral, `IN` =
peripheral→CM5, `BIDIR`, `PWR`.

---

## J1 — Main 5 V input  ·  *PWR*

| Pin | Signal | Dir | Notes |
|----:|--------|-----|-------|
| 1 | +5V | PWR | After fuse → reverse-polarity FET → TVS (REQ R2.1) |
| 2 | GND | PWR | |

> The **HUB75 panel 5 V** and **WS2812 5 V** are separate, individually fused
> rails (REQ R2.3/R2.4) — inject panel power at J2's VCC pins or a dedicated
> high-current terminal, **not** through the CM5 rail. Common ground.

---

## J2 — HUB75 face panel  ·  2×8 IDC (HUB75E standard)  ·  *5V-buf*

All signals are CM5 → panel through the **74AHCT245** face buffer (U1/U2). The
BCM column is the CM5-side source ([`PINMAP.md`](PINMAP.md)); the panel sees the
buffered 5 V copy.

| Pin | Signal | BCM (src) | Pin | Signal | BCM (src) |
|----:|--------|----------:|----:|--------|----------:|
| 1 | R1 | 5 | 2 | G1 | 13 |
| 3 | B1 | 6 | 4 | GND | — |
| 5 | R2 | 12 | 6 | G2 | 16 |
| 7 | B2 | 23 | 8 | E | 24 |
| 9 | A | 22 | 10 | B | 26 |
| 11 | C | 27 | 12 | D | 20 |
| 13 | CLK | 17 | 14 | LAT/STB | 21 |
| 15 | OE | 4 | 16 | GND | — |

> Pin 8 is **E** on 1/32-scan (64-row) panels and **GND** on 1/16-scan — match
> your panel. Keep buffer→connector traces short; optional 33 Ω series on CLK
> (REQ R3.3). Panel +5 V is fed from the dedicated panel rail, not this header.

---

## J3 — MAX7219 chain header  ·  *5V-buf*  *(alt face backend — see JP1)*

DIN/CLK/CS are CM5 → driver through a 74AHCT245. Source is jumper-selectable
(see **JP2**): hardware SPI0 or bit-bang GPIO.

| Pin | Signal | BCM (src, SPI0) | BCM (src, bit-bang) | Dir | Notes |
|----:|--------|----------------:|--------------------:|-----|-------|
| 1 | +5V | — | — | PWR | panel rail, fused |
| 2 | GND | — | — | PWR | |
| 3 | DIN | 10 (MOSI) | 14 | OUT | buffered |
| 4 | CLK | 11 (SCLK) | 15 | OUT | buffered |
| 5 | CS1 | 8 (CE0) | 25 | OUT | chain 1 |
| 6 | CS2 | 7 (CE1) | 18 | OUT | chain 2 |
| 7 | CS3 | 25 | 19 | OUT | chain 3 (GPIO CS) |
| 8 | CS4 | 9 | (spare) | OUT | chain 4 (GPIO CS) |

> DOUT daisy-chains module→module and never returns to the CM5, so no
> down-shift is needed. ⚠️ DIN (BCM 10) shares SPI0 MOSI with WS2812 — only one
> owner at a time (PINMAP contention rule 1).

---

## J4 — WS2812 accessory LEDs  ·  *5V-buf*

| Pin | Signal | BCM (src) | Dir | Notes |
|----:|--------|----------:|-----|-------|
| 1 | +5V | — | PWR | WS2812 rail, fused (REQ R2.4) |
| 2 | DIN | 10 (MOSI) | OUT | through 74AHCT125 → 5 V |
| 3 | GND | — | PWR | |

> Timing-critical: keep WS2812 on hardware SPI0. Add a large bulk cap at the
> LED rail and a series resistor on DIN.

---

## J5 — I²C bus 1 (sensors + expanders)  ·  *3V3*

Qwiic/STEMMA QT pin order so 1 mm JST-SH sensors plug straight in.

| Pin | Signal | BCM | Dir | Notes |
|----:|--------|----:|-----|-------|
| 1 | GND | — | PWR | |
| 2 | +3V3 | — | PWR | from CM5 3V3 rail |
| 3 | SDA | 2 | BIDIR | 4.7 kΩ pull-up to 3V3 |
| 4 | SCL | 3 | BIDIR | 4.7 kΩ pull-up to 3V3 |

> Shared by BNO055 0x28, MPU9250 0x68, MPR121 0x5A, BH1750 0x23, and any
> MCP23017 expanders (0x20–0x22). See [`IO-EXPANSION.md`](IO-EXPANSION.md).

---

## J6 — GPIO buttons / boop  ·  *3V3*

Direct CM5 lines for a few on-board switches. For more than this, use an I²C
expander (J5 / JX1) — it costs no Pi pins.

| Pin | Signal | BCM | Dir | Notes |
|----:|--------|----:|-----|-------|
| 1 | +3V3 | — | PWR | |
| 2 | GND | — | PWR | |
| 3 | GPIO14 | 14 | IN | spare (also UART TXD / MAX7219 bit-bang) |
| 4 | GPIO15 | 15 | IN | spare (also UART RXD / MAX7219 bit-bang) |
| 5 | GPIO18 | 18 | IN | spare (I²S free — audio on USB) |
| 6 | GPIO19 | 19 | IN | spare (I²S free) |

> Wire switches to GND (`active_low`); enable internal pull-ups in config.
> Optional series R + ESD array (REQ R6.1). Avoid HUB75-claimed pins — the
> firmware picker already hides them.

---

## J7 / J8 — CSI cameras  ·  22-pin 0.5 mm FFC (Pi 5 / CM5 standard)

Two CSI eyes (`csi_expected: 2`). The 22-pin flat-flex follows the **Raspberry
Pi 5 / CM5 camera standard** — wire per the CM5 datasheet's CAM lane mapping
rather than a hand-typed table (lane numbering is easy to get wrong). Functional
groups carried:

| Group | Signals | Notes |
|-------|---------|-------|
| MIPI data | 2× differential pairs (D0±, D1±) | per CSI lane |
| MIPI clock | 1× differential pair (CK±) | |
| Control I²C | SCL/SDA | sensor config (separate from bus 1) |
| Power | 3V3 (+ enable/regulator) | per camera |
| GPIO | shutdown / LED | optional |

> Use a flip-lock 0.5 mm connector; keep the differential pairs length-matched
> and short. Confirm exact pin numbers against the CM5 datasheet during layout.

---

## J9 — USB (hub uplink + downstream)  ·  USB 2.0

One uplink from a CM5 USB port; downstream ports to the peripheral stack. Each
port is standard USB 2.0:

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | VBUS (+5V) | from CM5 5 V, current-limited per port |
| 2 | D− | 90 Ω differential, length-matched |
| 3 | D+ | |
| 4 | GND | |

> Downstream: RP2350 helmet audio, smart knob, LoRa RAK4631, VITURE, USB cams.
> An onboard hub (USB2514B, REQ N1) consolidates these behind one uplink.

---

## J11 — Backpack USB umbilical (phone → CM5 host)  ·  USB 2.0

The phone (docked in the backpack) connects to a **CM5 USB host** port via the
tether; ProtoHUD mirrors it with scrcpy/ADB. Dedicate one CM5 port to this so
the long run doesn't share the in-helmet hub (J9).

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | VBUS (+5V) | passive dock: CM5 powers/charges phone. Charge-injection dock: **leave VBUS isolated** from CM5 |
| 2 | D− | shielded twisted pair; keep clear of power conductors |
| 3 | D+ | |
| 4 | GND | tie to system ground |
| — | SHIELD | cable shield → chassis/ground at one end |

> Pairs with the 5 V power feed (J1) as the **backpack→helmet umbilical**. They
> can share one multi-pin circular connector (GX16/GX20 / push-pull) — if so,
> segregate the USB pair from the high-current 5 V pins. See
> [`POWER.md`](POWER.md#umbilical-backpack--helmet).

## J10 — HDMI out  ·  HDMI standard (×2)

CM5 exposes two HDMI outputs. Use standard HDMI receptacles with an **ESD
protection array** on the TMDS pairs (REQ N10). Pinout is the HDMI standard —
TMDS data 0/1/2 + clock (differential pairs), DDC SCL/SDA, CEC, HPD, +5 V. Wire
per the connector datasheet; route TMDS pairs length-matched with ground
adjacency. Drives VITURE / external display.

---

## J12 — Cooling fan headers  ·  up to 4 fans in 2 zones

Driven by `sys::FanController` (software PWM). Two zones, each switched by a
**low-side N-MOSFET** whose gate is the zone's GPIO; up to two fan connectors per
zone share the zone speed. Use GPIO clear of HUB75 (default **BCM 18 = Zone 1**,
**BCM 19 = Zone 2**; PINMAP free set).

Per fan connector (4-pin PC-fan compatible):

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | GND (switched) | fan return through the zone MOSFET drain |
| 2 | +V (fan rail) | 5 V or 12 V depending on fan; **not** the CM5 rail |
| 3 | PWM/ctrl | tie to GND-switching for 2-pin fans; or drive a 4-pin fan's control line from the zone GPIO (add a buffer if the fan needs 5 V logic) |
| 4 | TACH | optional RPM sense back to a spare GPIO |

| Zone | Default GPIO | Fan headers |
|------|--------------|-------------|
| Zone 1 (e.g. Intake) | BCM 18 | 1–2 |
| Zone 2 (e.g. Exhaust) | BCM 19 | 1–2 |

> Each zone needs one MOSFET (gate ← GPIO, with a gate resistor + pulldown) and a
> flyback diode across each fan. Software PWM ≈100 Hz suits MOSFET-switched 2-pin
> fans; for quiet 4-pin control use a 25 kHz hardware-PWM pin/overlay later.

---

## Jumpers

### JP1 — Face backend select
Routes the 74AHCT245 face-buffer outputs to **one** of the face connectors.

| Position | Routes buffer → | Active backend |
|----------|-----------------|----------------|
| A (default) | J2 | HUB75 |
| B | J3 | MAX7219 |

> For running **both** simultaneously, don't share via JP1 — populate a
> dedicated buffer per panel (see [`MULTI-BACKEND.md`](MULTI-BACKEND.md)).

### JP2 — MAX7219 source select
Selects where J3's DIN/CLK come from.

| Position | DIN / CLK source | Use |
|----------|------------------|-----|
| SPI0 (default) | BCM 10 / 11 | hardware SPI (no WS2812 sharing SPI0) |
| GPIO | BCM 14 / 15 | bit-bang (WS2812 owns SPI0) |

---

## Expansion headers

### JX1 — I²C expansion / STEMMA QT (Qwiic)  ·  *3V3*
Solderless drop-in for a 2nd MCP23017, ADS1115, or PCA9685 (REQ N15). Same
electrical net as J5; Qwiic pin order:

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | GND | |
| 2 | +3V3 | |
| 3 | SDA (BCM 2) | |
| 4 | SCL (BCM 3) | |

### JX2 — Expander interrupts  ·  *3V3*
INT lines from I²C expanders → spare CM5 GPIO for interrupt-driven reads.

| Pin | Signal | BCM | Notes |
|----:|--------|----:|-------|
| 1 | INT0 | 25 | primary MCP23017 INTA |
| 2 | INT1 | 18 | 2nd expander (shares J6 — pick one use) |
| 3 | GND | — | |

> BCM 18/19 are shared between J6 (buttons) and JX2 (INT) — allocate per build.

### SPI0 lane (optional second expansion path)
If I²C fills, MCP23S17 / 74HC165 / 74HC595 ride **SPI0** (BCM 8/9/10/11 + CS),
which is free with HUB75 — but contends with WS2812/MAX7219 on BCM 10 (PINMAP
rule 1). Break out as needed.

---

## Keying & orientation (layout reminders)

- HUB75 (J2): boxed/shrouded 2×8 with a key slot; pin 1 silk triangle.
- Power (J1) and LED/MAX7219 (J3/J4): keyed, polarized connectors — reversing
  5 V/GND is destructive.
- I²C/Qwiic (J5/JX1): use the JST-SH 1 mm Qwiic standard so off-the-shelf
  cables work.
- Label every connector with its **J-number and the PINMAP net names** so the
  silk matches the firmware's GPIO visualizer.
