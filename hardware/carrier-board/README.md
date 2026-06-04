# ProtoHUD Carrier Board

Reference design notes for a custom carrier board that hosts the **Raspberry Pi
Compute Module 5 (CM5)** and breaks out everything ProtoHUD drives: the HUB75
face panels, the I²C sensor bus, the WS2812 accessory LEDs, the GPIO buttons,
the CSI cameras, and the USB peripheral stack (RP2350 helmet audio, smart knob,
LoRa, VITURE glasses).

This folder is documentation only — there are no board files here yet. It
captures **what the board must do** so a schematic/layout (KiCad) can be built
against a fixed spec.

- [`BLOCK-DIAGRAM.md`](BLOCK-DIAGRAM.md) — system block diagram (voltage domains + shifting boundary)
- [`PINMAP.md`](PINMAP.md) — master CM5 GPIO/net allocation (the source of truth)
- [`CONNECTORS.md`](CONNECTORS.md) — pin-by-pin pinouts for every connector + jumper
- [`POWER.md`](POWER.md) — power tree, rail budget, sizing, battery + protection
- [`kicad/`](kicad/) — KiCad hierarchical schematic skeleton (one sheet per block)
- [`REQUIREMENTS.md`](REQUIREMENTS.md) — must-have requirements + nice-to-haves
- [`BOM.md`](BOM.md) — bill of materials
- [`MULTI-BACKEND.md`](MULTI-BACKEND.md) — running HUB75 + MAX7219 + custom panels at once (pin budget + wiring)
- [`IO-EXPANSION.md`](IO-EXPANSION.md) — expanding GPIO for buttons + LEDs (MCP23017 on I²C, keep options open)

## Why a carrier board at all

The CM5 GPIO bank (RP1) is a **fixed 3.3 V CMOS** interface — there is no
software/firmware setting to run the pins at a different voltage, and the inputs
are **not 5 V tolerant**. Several ProtoHUD loads are 5 V-logic, so a hand-wired
build tends to be marginal. The carrier's main job is to make those interfaces
*correct* rather than *lucky*:

| Interface | Device | Logic | On the carrier |
|-----------|--------|-------|----------------|
| HUB75 | Face panels | **5 V** (VIH ≈ 3.5 V) | **3.3 V → 5 V buffer (74AHCT245)** — required |
| WS2812 | Accessory LEDs | **5 V** (VIH ≈ 3.5 V) | **3.3 V → 5 V buffer** on data line — required |
| I²C bus 1 | BNO055, MPU9250, MPR121, BH1750 | 3.3 V | Direct + 4.7 kΩ pull-ups |
| GPIO | Boop / buttons | 3.3 V | Direct (series R + optional ESD) |
| CSI | 2× cameras | MIPI | 22-pin FFC connectors |
| USB | RP2350 audio, knob, LoRa, VITURE | USB 2.0 | Hub / breakout |

> Driving HUB75 at 3.3 V is the classic "panels dark / flickery / wrong colors"
> failure: a panel's logic-high threshold is ≈ 0.7 × 5 V ≈ 3.5 V, *above* the
> CM5's 3.3 V output. The 74AHCT245 (TTL input thresholds, 5 V output) fixes it.
> This is exactly what an Adafruit RGB Matrix Bonnet's "active" buffer does.

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
sufficient — no bidirectional level translator needed.

### Other buses

| Bus | Pins | Notes |
|-----|------|-------|
| I²C bus 1 | SDA = BCM 2, SCL = BCM 3 | 4.7 kΩ pull-ups to 3.3 V; sensor connectors |
| WS2812 | BCM 10 (SPI0 MOSI) | Buffered to 5 V; power injected separately |
| GPIO buttons | e.g. BCM 27 (boop) | **Caution: BCM 27 = HUB75 "C"** — see below |

### ⚠️ Pin contention with HUB75

HUB75 claims 14 GPIOs, several of which collide with other default
assignments (documented in `README.md`'s Protoface integration table — e.g.
boop on BCM 27 vs HUB75 "C", boop BCM 17 vs CLK, SPI IMU vs HUB75 data). The
carrier should:

- Route button / boop / aux GPIO to lines **not** consumed by HUB75, and
- Provide jumper- or 0Ω-selectable assignment for the few that must be shared,

so a HUB75 build and a non-HUB75 (MAX7219) build can use one board.

## Power topology

Three conceptually separate 5 V domains sharing a common ground:

1. **CM5 5 V** — size for ≥ 5 A (CM5 + USB peripherals). Reverse-polarity and
   over-current protected.
2. **HUB75 panel 5 V** — *high current, separate rail.* Four 64×32 P2.5 panels
   can pull double-digit amps at full white; do not run panel power through the
   CM5 rail. Bulk capacitance at the panel connector.
3. **WS2812 5 V** — injected at the LED connector, fused.

3.3 V for the sensors comes from the CM5's 3.3 V rail (current config draws
~47 mA — see `src/main.cpp` `rail_currents_mA`); add a local 3.3 V buck only if
the sensor set grows.

See [`REQUIREMENTS.md`](REQUIREMENTS.md) for the full breakdown and
[`BOM.md`](BOM.md) for parts.
