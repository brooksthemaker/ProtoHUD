# ProtoHUD Carrier — KiCad project

A **hierarchical schematic** for the two-brain carrier (CM5 = HUB75 only;
RP2354B = all other I/O — see [`../RP2354-IO.md`](../RP2354-IO.md)). Open
`ProtoHUD-Carrier.kicad_pro` in **KiCad 9**. The root sheet has one
**hierarchical sheet** per functional block.

Two generators:
- **`populate_sheets.py`** — emits all **11 populated** sub-sheets (real symbols
  + a label netlist; global labels for rails/cross-sheet nets). Reuses the
  KiCad-9 format proven by the original face_buffer proof sheet.
- **`generate_skeleton.py`** — emits the root sheet + project file (and would
  emit annotated *stubs* for any sheet not in its `POPULATED` set; all 11 are
  now populated, so it only writes root + project). Stable UUIDs.

> Status: **all 11 sheets are populated.** Symbols use official-style `lib_id`s
> with embedded cached bodies and **condensed big-IC symbols** (the CM5 = 2×
> DF40-100 and RP2354B = QFN-80 show only the pins the carrier uses — full pads
> are a footprint/layout-time task). Connectivity is by net labels. Generated
> programmatically and **not yet opened in KiCad in this environment** — open in
> KiCad 9, run ERC (expect to-be-cleaned warnings: add `PWR_FLAG`s on rails,
> tidy unconnected pins), and report any load errors.

## Sheets

| # | Sheet file | Brain | Block | Source doc |
|---|-----------|-------|-------|-----------|
| 1 | `power.kicad_sch`  ✅ | — | 5 V input, protection, rails (+5V/+5V_PANEL/+5V_LED/+V_SERVO/+3V3_RP) | [`../POWER.md`](../POWER.md) |
| 2 | `cm5.kicad_sch`  ✅ | CM5 | DF40 connectors, decoupling, nRPIBOOT, HUB75 GPIO out | [`../REQUIREMENTS.md`](../REQUIREMENTS.md) |
| 3 | `face_buffer.kicad_sch` ✅ | CM5 | U1/U2 74AHCT245 → J2 HUB75 (**populated**) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 4 | `rp2354_io.kicad_sch`  ✅ | RP2354B | MCU core, 12 MHz xtal, +3V3_RP, BOOTSEL/SWD, SW1, J12 | [`../RP2354-IO.md`](../RP2354-IO.md) |
| 5 | `rp2354_face.kicad_sch`  ✅ | RP2354B | U10 74AHCT245, J3 MAX7219 (MX_*) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 6 | `leds.kicad_sch`  ✅ | RP2354B | U11 74AHCT125, J4 WS2812 ×4 (LED1..4_DAT) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 7 | `sensors_i2c.kicad_sch`  ✅ | RP2354B | I²C0 pull-ups, J5, SENS_INT, expanders | [`../IO-EXPANSION.md`](../IO-EXPANSION.md) |
| 8 | `servos.kicad_sch`  ✅ | RP2354B | J20–J27 servo headers, +V_SERVO rail (SRV1..8) | [`../RP2354-IO.md`](../RP2354-IO.md) |
| 9 | `gpio_buttons.kicad_sch`  ✅ | RP2354B | J6 buttons/boop (BTN1..10) | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 10 | `usb.kicad_sch`  ✅ | CM5 | hub, J9 downstream (RP2354B/RP2350/…), J11 uplink | [`../CONNECTORS.md`](../CONNECTORS.md) |
| 11 | `cameras_display.kicad_sch`  ✅ | CM5 | J7/J8 CSI, J10 HDMI | [`../CONNECTORS.md`](../CONNECTORS.md) |

All 11 are populated (✅) — symbols placed, nets labelled. Cross-sheet nets are
**global labels** (rails `+5V`/`+5V_PANEL`/`+5V_LED`/`+V_SERVO`/`+3V3_RP`/`GND`,
the `GPIOxx` HUB75 group, `MX_*`/`LED*_DAT`/`SRV*`/`BTN*`, `*_USB_*`), so blocks
connect without manual sheet-pin plumbing.

## Review / finishing workflow

1. Open the project in KiCad 9; double-click each sheet. Run **`Symbol > Update
   Symbols from Library`** to swap the embedded cached bodies for the pretty
   official ones (matched by `lib_id` + pin number).
2. Expand the **condensed CM5 / RP2354B** symbols to full pinout when assigning
   footprints (the carrier physically uses 2× DF40-100 for the CM5 and a QFN-80
   for the RP2354B).
3. Run **ERC** and clean expected warnings: add `PWR_FLAG`s on the rails, tie
   off unconnected buffer/ESD pins, confirm net names vs [`../PINMAP.md`](../PINMAP.md).
4. Assign footprints (parts per [`../BOM.md`](../BOM.md)), then lay out the PCB.

## Regenerating

```bash
python3 populate_sheets.py        # all 11 populated sub-sheets
python3 generate_skeleton.py      # root sheet + project file
```
UUIDs are stable across runs (sheet hierarchy links hold). Edit `SHEET_UUID` /
the per-sheet builders in `populate_sheets.py`; keep `POPULATED` in
`generate_skeleton.py` in sync.
