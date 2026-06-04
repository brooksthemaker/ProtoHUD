#!/usr/bin/env python3
"""Generate a KiCad 7+ hierarchical schematic skeleton for the ProtoHUD carrier.

Emits ProtoHUD-Carrier.kicad_pro, a root sheet with one hierarchical sheet per
functional block, and an annotated stub sub-sheet for each block (parts list +
the exact BCM net names from PINMAP.md as on-canvas text). Re-run to regenerate.

This is a SKELETON: it opens in KiCad as the hierarchy; you place symbols and
draw nets per the annotations. Targets the KiCad 7 file format (v8 upgrades it
on first save).
"""
import os, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "ProtoHUD-Carrier"
VERSION = "20230121"           # KiCad 7 schematic format
GEN = "protohud_skeleton"

def u():                       # fresh UUID string
    return str(uuid.uuid4())

# ── Sheet plan: (file, title, content lines) ────────────────────────────────
SHEETS = [
    ("power", "1. Power", [
        "POWER — 5 V input from backpack umbilical (external 40V->5V buck off-board)",
        "Parts: J1 5V in (XT60) + remote-sense pair, bulk cap >=2200uF,",
        "  5V reverse-polarity P-FET, TVS SMBJ5.0A, per-rail bulk caps,",
        "  rail fuses (HUB75 / WS2812), optional INA219 x N telemetry.",
        "Rails: +5V (CM5+USB), +5V_PANEL (fused), +5V_LED (fused),",
        "  +3V3 (from CM5), GND (common star).",
        "See POWER.md.",
    ]),
    ("cm5", "2. CM5", [
        "CM5 — compute module + housekeeping",
        "Parts: 2x Hirose DF40C-100DS-0.4V(51), decoupling per CM5 guide,",
        "  nRPIBOOT jumper + USB (eMMC flash), 40-pin debug header.",
        "Nets: all BCM lines (see PINMAP.md), +5V, +3V3, GND.",
        "I2C1: SDA=GPIO2, SCL=GPIO3.  SPI0: MOSI=10 SCLK=11 MISO=9 CE0=8 CE1=7.",
    ]),
    ("face_buffer", "3. Face Buffer (HUB75 / MAX7219)", [
        "FACE BUFFER — 3.3V->5V, pick one backend via JP1",
        "Parts: U1/U2 74AHCT245 @5V, JP1 backend select (HUB75<->MAX7219),",
        "  JP2 MAX7219 source (SPI0 vs bit-bang), J2 HUB75 2x8 IDC,",
        "  J3 MAX7219 header (5V/GND/DIN/CLK/CS x4), 33R series (CLK), decoupling.",
        "HUB75 BCM: R1=5 G1=13 B1=6 R2=12 G2=16 B2=23 A=22 B=26 C=27 D=20 E=24",
        "           OE=4 CLK=17 STB=21   (all CM5->panel, buffered to 5V)",
        "MAX7219:  DIN=GPIO10(MOSI)|14  CLK=GPIO11(SCLK)|15  CS1..4=GPIO8/7/25/9",
    ]),
    ("leds", "4. WS2812 LEDs", [
        "WS2812 ACCESSORY LEDS",
        "Parts: U3 74AHCT1G125 @5V, J4 LED conn (5V/DIN/GND), bulk cap, series R.",
        "Net: WS2812 DIN from GPIO10 (SPI0 MOSI), buffered to 5V; +5V_LED (fused).",
        "NOTE: GPIO10 shared with MAX7219-SPI0 — one owner at a time (PINMAP rule 1).",
    ]),
    ("sensors_i2c", "5. Sensors + I2C expansion", [
        "I2C BUS 1 — sensors + I/O expanders (3V3, direct)",
        "Parts: 4.7k SDA/SCL pull-ups, J5 I2C/Qwiic, JX1 STEMMA QT expansion,",
        "  MCP23017 x1-8 (buttons+LEDs), JX2 expander INT header.",
        "Nets: SDA=GPIO2, SCL=GPIO3, +3V3, GND.  INT0=GPIO25, INT1=GPIO18.",
        "Addrs: BNO055 0x28, MPU9250 0x68, MPR121 0x5A, BH1750 0x23,",
        "       MCP23017 0x20-0x22 (avoid 0x23).  See IO-EXPANSION.md.",
    ]),
    ("usb", "6. USB", [
        "USB — in-helmet hub + backpack uplink",
        "Parts: USB2514B/2517 hub, J9 downstream ports (RP2350 audio / knob /",
        "  LoRa / VITURE / cams), J11 backpack USB uplink (phone->CM5, scrcpy/ADB),",
        "  USBLC6 ESD on data pairs.",
        "Nets: CM5 USB D+/D-, VBUS.  J11 = dedicated CM5 host port (see CONNECTORS.md).",
    ]),
    ("cameras_display", "7. Cameras + Display", [
        "CSI CAMERAS + HDMI",
        "Parts: J7/J8 22-pin 0.5mm CSI FFC (2 eyes), J10 2x HDMI + ESD array.",
        "Nets: MIPI data/clock differential pairs + cam I2C (verify pin numbers",
        "  against CM5 datasheet); HDMI TMDS pairs + DDC + HPD + CEC.",
        "Keep differential pairs length-matched.",
    ]),
    ("gpio_buttons", "8. GPIO Buttons", [
        "GPIO BUTTONS / BOOP — direct CM5 lines (few switches; bulk via I2C expander)",
        "Parts: J6 header, series R, optional ESD array.",
        "Nets: GPIO14, GPIO15, GPIO18, GPIO19 (free w/ HUB75), +3V3, GND.",
        "Wire switches to GND (active_low); enable internal pull-ups in config.",
        "NOTE: GPIO18/19 shared with expander INT (JX2) — allocate per build.",
    ]),
]

root_uuid = u()
sheet_uuids = {f: u() for f, _, _ in SHEETS}

def effects(size=1.27, justify="left bottom", hide=False):
    h = " (hide yes)" if hide else ""
    return f"(effects (font (size {size} {size})){' (justify ' + justify + ')' if justify else ''}{h})"

def write(path, text):
    with open(os.path.join(HERE, path), "w") as fh:
        fh.write(text)

# ── Sub-sheets ──────────────────────────────────────────────────────────────
for fname, title, lines in SHEETS:
    texts = []
    y = 25.4
    for ln in [title, "-" * 60] + lines:
        safe = ln.replace('"', "'")
        texts.append(
            f'  (text "{safe}" (at 25.4 {y:.2f} 0) '
            f'(effects (font (size 1.5 1.5)) (justify left bottom)) (uuid {u()}))'
        )
        y += 3.0
    body = "\n".join(texts)
    write(f"{fname}.kicad_sch", f"""(kicad_sch
  (version {VERSION})
  (generator {GEN})
  (uuid {sheet_uuids[fname]})
  (paper "A3")
  (title_block
    (title "ProtoHUD Carrier — {title}")
    (rev "A")
    (company "ProtoHUD")
  )
  (lib_symbols)
{body}
  (sheet_instances
    (path "/" (page "1"))
  )
)
""")

# ── Root sheet (hierarchical sheet objects) ─────────────────────────────────
sheet_objs = []
x, y = 25.4, 25.4
for i, (fname, title, _) in enumerate(SHEETS):
    sheet_objs.append(f"""  (sheet (at {x:.2f} {y:.2f}) (size 60 25) (fields_autoplaced)
    (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000))
    (uuid {sheet_uuids[fname]})
    (property "Sheetname" "{title}" (at {x:.2f} {y-0.7:.2f} 0) {effects(1.27,'left bottom')})
    (property "Sheetfile" "{fname}.kicad_sch" (at {x:.2f} {y+25.8:.2f} 0) {effects(1.27,'left top')})
  )""")
    y += 32
    if (i + 1) % 4 == 0:
        y = 25.4
        x += 80

write(f"{PROJECT}.kicad_sch", f"""(kicad_sch
  (version {VERSION})
  (generator {GEN})
  (uuid {root_uuid})
  (paper "A3")
  (title_block
    (title "ProtoHUD Carrier Board — root")
    (rev "A")
    (company "ProtoHUD")
    (comment 1 "Hierarchical skeleton — see hardware/carrier-board docs")
  )
  (lib_symbols)
{chr(10).join(sheet_objs)}
  (sheet_instances
    (path "/" (page "1"))
  )
)
""")

# ── Project file (.kicad_pro) ───────────────────────────────────────────────
pages = '[\n      ["' + root_uuid + '", "Root"]'
for fname, title, _ in SHEETS:
    pages += ',\n      ["' + sheet_uuids[fname] + '", "' + title + '"]'
pages += "\n    ]"

write(f"{PROJECT}.kicad_pro", """{
  "board": { "design_settings": {}, "layer_presets": [], "viewports": [] },
  "boards": [],
  "cvpcb": { "equivalence_files": [] },
  "libraries": { "pinned_footprint_libs": [], "pinned_symbol_libs": [] },
  "meta": { "filename": "%s.kicad_pro", "version": 1 },
  "net_settings": { "classes": [ { "name": "Default", "clearance": 0.2 } ] },
  "pcbnew": { "page_layout_descr_file": "" },
  "schematic": {
    "drawing": {},
    "legacy_lib_dir": "",
    "legacy_lib_list": []
  },
  "sheets": %s,
  "text_variables": {}
}
""" % (PROJECT, pages))

print(f"Generated {PROJECT}.kicad_pro + root + {len(SHEETS)} sub-sheets in {HERE}")
