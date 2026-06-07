# ProtoHUD Carrier Board

Reference design notes for a custom carrier board with a **two-brain**
architecture:

- a **Raspberry Pi Compute Module 5 (CM5)** that drives **only the HUB75 face
  panels** (plus CSI cameras, HDMI, the USB peripheral stack, and power), and
- a dedicated **RP2354B I/O coprocessor** that owns **everything else** — the
  I²C sensor bus, WS2812 accessory LEDs, GPIO buttons/boop, the MAX7219 face
  backend, and **servos** — talking to the CM5 as a **USB-CDC** device.

The USB peripheral stack (RP2350 helmet audio, smart knob, LoRa, VITURE glasses)
hangs off an onboard USB hub.

This folder is documentation only — there are no board files here yet (a KiCad
schematic is being populated in [`kicad/`](kicad/)). It captures **what the
board must do** so a schematic/layout can be built against a fixed spec.

- [`RP2354-IO.md`](RP2354-IO.md) — **the I/O coprocessor** (pin budget, buses, servos, USB-CDC, programming) ⭐
- [`BLOCK-DIAGRAM.md`](BLOCK-DIAGRAM.md) — system block diagram (two brains, voltage domains, shifting boundaries)
- [`PINMAP.md`](PINMAP.md) — master GPIO/net allocation, CM5 + RP2354B (the source of truth)
- [`CONNECTORS.md`](CONNECTORS.md) — pin-by-pin pinouts for every connector + jumper
- [`POWER.md`](POWER.md) — power tree, rail budget, sizing, battery + protection
- [`kicad/`](kicad/) — KiCad hierarchical schematic (one sheet per block)
- [`REQUIREMENTS.md`](REQUIREMENTS.md) — must-have requirements + nice-to-haves
- [`BOM.md`](BOM.md) — bill of materials
- [`MULTI-BACKEND.md`](MULTI-BACKEND.md) — running HUB75 + MAX7219 + custom panels at once (now on separate brains)
- [`IO-EXPANSION.md`](IO-EXPANSION.md) — expanding I/O beyond the RP2354B (I²C expanders, keep options open)

## Why a carrier board at all

Two problems a hand-wired build hits, both solved here:

**1. Logic levels.** Both the CM5 GPIO bank (RP1) and the RP2354B GPIO are
**fixed 3.3 V CMOS**, not 5 V-tolerant. Several ProtoHUD loads are 5 V-logic, so
the carrier's job is to make those interfaces *correct* rather than *lucky* with
the right buffers on the right brain:

| Interface | Device | Brain | Logic | On the carrier |
|-----------|--------|-------|-------|----------------|
| HUB75 | Face panels | **CM5** | **5 V** (VIH ≈ 3.5 V) | **3.3 V → 5 V buffer (74AHCT245)** — required |
| MAX7219 | Alt face matrix | **RP2354B** | **5 V** | **3.3 V → 5 V buffer (74AHCT245)** |
| WS2812 | Accessory LEDs | **RP2354B** | **5 V** | **3.3 V → 5 V buffer (74AHCT125)** on data |
| I²C sensors | BNO055, MPU9250, MPR121, BH1750 | **RP2354B** | 3.3 V | Direct + 4.7 kΩ pull-ups |
| Servos ×8 | PWM | **RP2354B** | accepts 3.3 V | Direct; own 5–6 V power rail |
| Buttons / boop | switches | **RP2354B** | 3.3 V | Direct (pull-ups + optional ESD) |
| CSI | 2× cameras | **CM5** | MIPI | 22-pin FFC connectors |
| USB | RP2350 audio, knob, LoRa, VITURE, RP2354B | **CM5** | USB 2.0 | Onboard hub |

> Driving HUB75 at 3.3 V is the classic "panels dark / flickery / wrong colors"
> failure: a panel's logic-high threshold is ≈ 0.7 × 5 V ≈ 3.5 V, *above* the
> 3.3 V output. The 74AHCT245 (TTL input thresholds, 5 V output) fixes it — the
> same active buffer an Adafruit RGB Matrix Bonnet uses, and the same trick the
> RP2354B uses for its MAX7219/WS2812 loads.

**2. Pin contention.** HUB75 alone eats 14 of the CM5's ~28 usable GPIO and
blocks SPI1. Putting *all other* I/O on the **RP2354B** (which has 48 GPIO +
PIO) removes the contention entirely — the CM5 only ever drives HUB75, and the
RP2354B has room to spare. See [`RP2354-IO.md`](RP2354-IO.md).

## Signal map (must match the firmware)

The silkscreen and net names should mirror the pin assignments in the code so
the in-HUD GPIO visualizer stays accurate.

### HUB75 — `src/main.cpp` (`kHub75`) / Adafruit RGB-HAT pinout

| Signal | BCM | Signal | BCM | Signal | BCM |
|--------|-----|--------|-----|--------|-----|
| R1 | 5  | R2 | 12 | A  | 22 |
| G1 | 13 | G2 | 16 | B  | 26 |
| B1 | 6  | B2 | 23 | C  | 27 |
| CLK | 17 | OE | 4  | D  | 20 |
| STB/LAT | 21 | | | E  | 24 |

All 14 lines are **CM5 → panel** (unidirectional), so a plain octal buffer is
sufficient — no bidirectional level translator needed. **This is the only thing
the CM5 GPIO drives.**

### RP2354B I/O nets

Everything else is on the RP2354B (full table in
[`RP2354-IO.md`](RP2354-IO.md#pin-allocation-source-of-truth)):

| Bus | RP2354B GPIO | Net | Notes |
|-----|--------------|-----|-------|
| I²C0 sensors | GP4/GP5 (+GP6 INT) | `SDA0`/`SCL0` | 4.7 kΩ pull-ups to 3.3 V; BNO055/MPU9250/MPR121/BH1750 |
| MAX7219 (SPI0) | GP2/GP3 + GP7–10 | `MX_*` | buffered to 5 V → J3 |
| WS2812 ×4 (PIO) | GP16–GP19 | `LED1..4_DAT` | buffered to 5 V; power injected separately |
| Servos ×8 (PWM) | GP20–GP27 | `SRV1..8` | 3.3 V direct; 5–6 V servo rail |
| Buttons/boop | GP28–GP37 | `BTN1..10` | pull-ups; MCU debounce |

### No more pin contention

With HUB75 the *only* CM5 GPIO consumer and all other I/O on the RP2354B, the
old HUB75-vs-everything contention is gone. A useful side effect: **HUB75 and
MAX7219 can run simultaneously** (different brains) — see
[`MULTI-BACKEND.md`](MULTI-BACKEND.md).

## Power topology

5 V domains sharing a common ground, plus 3.3 V derived locally:

1. **CM5 5 V** — size for ≥ 5 A (CM5 + USB peripherals). Reverse-polarity and
   over-current protected.
2. **HUB75 panel 5 V** — *high current, separate rail.* Four 64×32 P2.5 panels
   can pull double-digit amps at full white; do not run panel power through the
   CM5 rail. Bulk capacitance at the panel connector.
3. **WS2812 5 V** — injected at the LED connector, fused.
4. **Servo 5–6 V** — own fused, bulk-capped rail so stall surges don't disturb
   logic (8 servos).
5. **3.3 V (RP2354B)** — local LDO/buck off 5 V, feeds the RP2354B + the I²C
   sensors + the buffers' 3.3 V side. (Sensors no longer draw from the CM5 3V3.)

See [`POWER.md`](POWER.md) for the full breakdown, [`REQUIREMENTS.md`](REQUIREMENTS.md)
for requirements, and [`BOM.md`](BOM.md) for parts.
