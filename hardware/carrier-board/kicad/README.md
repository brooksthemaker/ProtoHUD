# ProtoHUD Carrier — KiCad project skeleton

A **hierarchical schematic skeleton** scaffolding the carrier design. Open
`ProtoHUD-Carrier.kicad_pro` in KiCad (**7 or 8** — v8 will offer a one-time
format upgrade on first save). The root sheet contains one **hierarchical
sheet** per functional block; each sub-sheet is pre-annotated with its parts
list and the **exact BCM net names from [`../PINMAP.md`](../PINMAP.md)** so it's
a fill-in guide, not a blank page.

> This is scaffolding, not a finished schematic: symbols and nets aren't placed
> yet. It was generated programmatically and has **not** been opened/validated
> in KiCad in this environment — if anything doesn't load, regenerate or tell me
> and I'll fix the format.

## Sheets

| # | Sheet file | Block | Source doc |
|---|-----------|-------|-----------|
| 1 | `power.kicad_sch` | 5 V input, protection, rails, INA219 | [`../POWER.md`](../POWER.md) |
| 2 | `cm5.kicad_sch` | DF40 connectors, decoupling, nRPIBOOT | [`../REQUIREMENTS.md`](../REQUIREMENTS.md) |
| 3 | `face_buffer.kicad_sch` | 74AHCT245, JP1/JP2, J2 HUB75, J3 MAX7219 | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 4 | `leds.kicad_sch` | 74AHCT1G125, J4 WS2812 | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 5 | `sensors_i2c.kicad_sch` | I²C pull-ups, J5/JX1, MCP23017, JX2 | [`../IO-EXPANSION.md`](../IO-EXPANSION.md) |
| 6 | `usb.kicad_sch` | hub, J9 downstream, J11 backpack uplink | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 7 | `cameras_display.kicad_sch` | J7/J8 CSI, J10 HDMI | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 8 | `gpio_buttons.kicad_sch` | J6 buttons/boop | [`../CONNECTORS.md`](../CONNECTORS.md) |

## Fill-in workflow

1. Open the project; double-click a sheet to enter it. The on-canvas text lists
   the parts and nets for that block.
2. Place symbols (parts per [`../BOM.md`](../BOM.md)) and draw nets, **labeling
   them with the PINMAP net names** so the schematic, the firmware GPIO map, and
   the silkscreen all agree.
3. Promote shared rails/buses (`+5V`, `+3V3`, `GND`, `SDA`/`SCL`, the HUB75
   group) to **hierarchical/global labels** so they connect across sheets.
4. Assign footprints, then lay out the PCB. Honor the
   [`../PINMAP.md`](../PINMAP.md) contention rules (BCM 10 single-owner, SPI1
   off-limits with HUB75).

## Regenerating

```bash
python3 generate_skeleton.py
```
Edit the `SHEETS` table in the script to change the block list or annotations;
re-running rewrites the `.kicad_sch`/`.kicad_pro` with fresh UUIDs.
