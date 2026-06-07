# ProtoHUD Carrier — KiCad project

A **hierarchical schematic** for the two-brain carrier (CM5 = HUB75 only;
RP2354B = all other I/O — see [`../RP2354-IO.md`](../RP2354-IO.md)). Open
`ProtoHUD-Carrier.kicad_pro` in **KiCad 9**. The root sheet has one
**hierarchical sheet** per functional block.

Two generators:
- **`generate_skeleton.py`** — emits the root + project + the *stub* sub-sheets
  (annotated parts/nets, no symbols yet). Idempotent (stable UUIDs).
- **`populate_face_buffer.py`** — emits the **populated** `face_buffer.kicad_sch`
  (real symbols + label netlist). The skeleton generator skips this file so it
  is never clobbered (`POPULATED` set).

> Status: `face_buffer` is populated (HUB75 buffer, proof of the KiCad-9 format —
> validate it in your KiCad). The other ten sheets are annotated stubs awaiting
> population. The populated sheet has not been opened/validated in KiCad in this
> environment — report any load errors and I'll fix the format.

## Sheets

| # | Sheet file | Brain | Block | Source doc |
|---|-----------|-------|-------|-----------|
| 1 | `power.kicad_sch` | — | 5 V input, protection, rails (+5V/+5V_PANEL/+5V_LED/+V_SERVO/+3V3_RP) | [`../POWER.md`](../POWER.md) |
| 2 | `cm5.kicad_sch` | CM5 | DF40 connectors, decoupling, nRPIBOOT, HUB75 GPIO out | [`../REQUIREMENTS.md`](../REQUIREMENTS.md) |
| 3 | `face_buffer.kicad_sch` ✅ | CM5 | U1/U2 74AHCT245 → J2 HUB75 (**populated**) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 4 | `rp2354_io.kicad_sch` | RP2354B | MCU core, 12 MHz xtal, +3V3_RP, BOOTSEL/SWD, SW1, J12 | [`../RP2354-IO.md`](../RP2354-IO.md) |
| 5 | `rp2354_face.kicad_sch` | RP2354B | U10 74AHCT245, J3 MAX7219 (MX_*) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 6 | `leds.kicad_sch` | RP2354B | U11 74AHCT125, J4 WS2812 ×4 (LED1..4_DAT) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 7 | `sensors_i2c.kicad_sch` | RP2354B | I²C0 pull-ups, J5, SENS_INT, expanders | [`../IO-EXPANSION.md`](../IO-EXPANSION.md) |
| 8 | `servos.kicad_sch` | RP2354B | J20–J27 servo headers, +V_SERVO rail (SRV1..8) | [`../RP2354-IO.md`](../RP2354-IO.md) |
| 9 | `gpio_buttons.kicad_sch` | RP2354B | J6 buttons/boop (BTN1..10) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 10 | `usb.kicad_sch` | CM5 | hub, J9 downstream (RP2354B/RP2350/…), J11 uplink | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 11 | `cameras_display.kicad_sch` | CM5 | J7/J8 CSI, J10 HDMI | [`../CONNECTORS.md`](../CONNECTORS.md) |

## Fill-in workflow

1. Open the project; double-click a sheet to enter it. On-canvas text lists the
   block's parts and nets.
2. Place symbols (parts per [`../BOM.md`](../BOM.md)) and draw nets, **labeling
   them with the PINMAP / RP2354-IO net names** so the schematic, the firmware
   GPIO map, and the silkscreen all agree.
3. Promote shared rails/buses (`+5V`, `+3V3_RP`, `GND`, `SDA0`/`SCL0`, the HUB75
   group) to **hierarchical/global labels** so they connect across sheets.
4. Assign footprints, then lay out the PCB.

## Regenerating

```bash
python3 generate_skeleton.py      # stubs + root + project (skips face_buffer)
python3 populate_face_buffer.py   # the populated HUB75 buffer sheet
```
Edit `SHEETS` / `POPULATED` in `generate_skeleton.py` to change the block list.
UUIDs are stable across runs (so the populated sheet's hierarchy link holds).
