#!/usr/bin/env python3
"""Generate a fully-populated `face_buffer.kicad_sch` (KiCad 9 format).

This is the PROOF sheet for the carrier schematic-capture effort: it turns the
annotated stub from `generate_skeleton.py` into a real, openable schematic with
placed symbols, embedded (cached) symbol definitions, and a label-driven
netlist that matches `../PINMAP.md` / `../CONNECTORS.md`.

Design notes:
  * Symbols reference the OFFICIAL KiCad lib_ids (74xx, Device, Connector_*),
    so `Symbol > Update Symbols from Library` will swap the embedded cached
    bodies for the pretty library ones (matched by lib_id + pin number).
  * Connectivity is by net LABELS placed exactly on each pin's connection
    point — survives a symbol-update without re-routing wires.
  * Cross-sheet nets (CM5 GPIO lines, rails) are hierarchical/global labels.

Re-run:  python3 populate_face_buffer.py
"""
import os, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "ProtoHUD-Carrier"
SCH_VERSION = "20250114"          # KiCad 9 schematic S-expr format
GEN = "protohud_populate"
# UUID of the face_buffer sheet *object* inside the root sheet (instance path).
SHEET_OBJ_UUID = "eff6a452-77ae-416d-87a8-cfe219f371e8"

def u():
    return str(uuid.uuid4())

# ── tiny S-expr helpers ─────────────────────────────────────────────────────
def eff(size=1.27, justify="", hide=False):
    j = f" (justify {justify})" if justify else ""
    h = " (hide yes)" if hide else ""
    return f"(effects (font (size {size} {size})){j}{h})"

def prop(name, value, x, y, rot=0, hide=False, size=1.27, justify=""):
    return (f'    (property "{name}" "{value}" (at {x:.2f} {y:.2f} {rot}) '
            f'{eff(size, justify, hide)})')

# ── library symbol (cached) definitions ─────────────────────────────────────
# Each entry: lib_id -> (value, [pins]) where a pin is
#   (number, name, etype, x, y, rot)   in SYMBOL coordinates (+Y up).
# etype in: input output bidirectional power_in passive open_collector ...
def pin_line(num, name, etype, x, y, rot, length=2.54):
    return (f'      (pin {etype} line (at {x:.2f} {y:.2f} {rot}) (length {length})\n'
            f'        (name "{name}" {eff(1.0)})\n'
            f'        (number "{num}" {eff(1.0)}))')

def rect(x1, y1, x2, y2):
    return (f'      (rectangle (start {x1:.2f} {y1:.2f}) (end {x2:.2f} {y2:.2f})\n'
            f'        (stroke (width 0.254) (type default)) (fill (type background)))')

def lib_symbol(lib_id, value, body_graphics, pins, pin_names_offset=1.016,
               hide_pin_numbers=False):
    base = lib_id.split(":")[1]
    pn = "    (pin_numbers (hide yes))\n" if hide_pin_numbers else ""
    out = [f'  (symbol "{lib_id}"']
    out.append(pn.rstrip("\n") if pn else None)
    out.append(f'    (pin_names (offset {pin_names_offset}))')
    out.append('    (exclude_from_sim no) (in_bom yes) (on_board yes)')
    out.append(prop("Reference", value[0], 0, 0, hide=False))
    out.append(prop("Value", value[1], 0, 0, hide=False))
    out.append(prop("Footprint", "", 0, 0, hide=True))
    out.append(prop("Datasheet", "", 0, 0, hide=True))
    out = [l for l in out if l is not None]
    # graphic sub-symbol
    out.append(f'    (symbol "{base}_0_1"')
    out.extend(body_graphics)
    out.append('    )')
    # pin sub-symbol
    out.append(f'    (symbol "{base}_1_1"')
    out.extend(pins)
    out.append('    )')
    out.append('  )')
    return "\n".join(out)


# ---- 74AHCT245 octal buffer -------------------------------------------------
def sym_245():
    pins = []
    # left side (inputs), pin tips point right (rot 0)
    left = [("1", "DIR", "input", 10.16), ("19", "~{OE}", "input", 7.62),
            ("2", "A1", "input", 2.54), ("3", "A2", "input", 0.0),
            ("4", "A3", "input", -2.54), ("5", "A4", "input", -5.08),
            ("6", "A5", "input", -7.62), ("7", "A6", "input", -10.16),
            ("8", "A7", "input", -12.7), ("9", "A8", "input", -15.24)]
    for num, name, et, y in left:
        pins.append(pin_line(num, name, et, -12.7, y, 0))
    right = [("20", "VCC", "power_in", 10.16), ("10", "GND", "power_in", 7.62),
             ("18", "B1", "output", 2.54), ("17", "B2", "output", 0.0),
             ("16", "B3", "output", -2.54), ("15", "B4", "output", -5.08),
             ("14", "B5", "output", -7.62), ("13", "B6", "output", -10.16),
             ("12", "B7", "output", -12.7), ("11", "B8", "output", -15.24)]
    for num, name, et, y in right:
        pins.append(pin_line(num, name, et, 12.7, y, 180))
    body = [rect(-10.16, 12.7, 10.16, -17.78)]
    return lib_symbol("74xx:74AHCT245", ("U", "74AHCT245"), body, pins)

# ---- Device:R ---------------------------------------------------------------
def sym_R():
    pins = [pin_line("1", "~", "passive", 0, 3.81, 270, 1.27),
            pin_line("2", "~", "passive", 0, -3.81, 90, 1.27)]
    body = [(f'      (rectangle (start -1.016 2.54) (end 1.016 -2.54)\n'
             f'        (stroke (width 0.254) (type default)) (fill (type none)))')]
    return lib_symbol("Device:R", ("R", "R"), body, pins, hide_pin_numbers=True)

# ---- Device:C ---------------------------------------------------------------
def sym_C():
    pins = [pin_line("1", "~", "passive", 0, 3.81, 270, 2.794),
            pin_line("2", "~", "passive", 0, -3.81, 90, 2.794)]
    body = [
        '      (polyline (pts (xy -2.032 0.762) (xy 2.032 0.762))'
        ' (stroke (width 0.508) (type default)) (fill (type none)))',
        '      (polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762))'
        ' (stroke (width 0.508) (type default)) (fill (type none)))',
    ]
    return lib_symbol("Device:C", ("C", "C"), body, pins, hide_pin_numbers=True)

# ---- Connector_Generic:Conn_02x08_Odd_Even ---------------------------------
def sym_conn_02x08():
    pins = []
    for i in range(8):
        y = 8.89 - i * 2.54
        pins.append(pin_line(str(2 * i + 1), "Pin_%d" % (2 * i + 1),
                             "passive", -5.08, y, 0, 2.54))     # odd, left
        pins.append(pin_line(str(2 * i + 2), "Pin_%d" % (2 * i + 2),
                             "passive", 5.08, y, 180, 2.54))    # even, right
    body = [rect(-2.54, 10.16, 2.54, -10.16)]
    return lib_symbol("Connector_Generic:Conn_02x08_Odd_Even",
                      ("J", "Conn_02x08_Odd_Even"), body, pins,
                      hide_pin_numbers=True)

# ---- Connector_Generic:Conn_01x08 ------------------------------------------
def sym_conn_01x08():
    pins = []
    for i in range(8):
        y = 8.89 - i * 2.54
        pins.append(pin_line(str(i + 1), "Pin_%d" % (i + 1),
                             "passive", -5.08, y, 0, 2.54))
    body = [rect(-1.27, 10.16, 1.27, -10.16)]
    return lib_symbol("Connector_Generic:Conn_01x08",
                      ("J", "Conn_01x08"), body, pins, hide_pin_numbers=True)

# ---- Connector_Generic:Conn_01x03 (JP source select) -----------------------
def sym_conn_01x03():
    pins = []
    for i in range(3):
        y = 2.54 - i * 2.54
        pins.append(pin_line(str(i + 1), "Pin_%d" % (i + 1),
                             "passive", -5.08, y, 0, 2.54))
    body = [rect(-1.27, 3.81, 1.27, -3.81)]
    return lib_symbol("Connector_Generic:Conn_01x03",
                      ("J", "Conn_01x03"), body, pins, hide_pin_numbers=True)

# ---- power symbols (+5V, GND) ----------------------------------------------
def sym_power(lib_id, name, glyph_up=True):
    # single power_in pin at origin pointing into the schematic
    if glyph_up:
        pin = pin_line("1", name, "power_in", 0, 0, 90, 0)
        graphics = [
            '      (polyline (pts (xy -0.762 1.27) (xy 0 2.54) (xy 0.762 1.27))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
            '      (polyline (pts (xy 0 0) (xy 0 2.54))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
        ]
    else:  # GND glyph, pin points up, glyph below
        pin = pin_line("1", name, "power_in", 0, 0, 90, 0)
        graphics = [
            '      (polyline (pts (xy 0 0) (xy 0 -1.27))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
            '      (polyline (pts (xy -1.27 -1.27) (xy 1.27 -1.27))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
            '      (polyline (pts (xy -0.762 -1.778) (xy 0.762 -1.778))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
            '      (polyline (pts (xy -0.254 -2.286) (xy 0.254 -2.286))'
            ' (stroke (width 0) (type default)) (fill (type none)))',
        ]
    out = [f'  (symbol "{lib_id}"',
           '    (power) (pin_names (offset 0)) (exclude_from_sim no) (in_bom yes) (on_board yes)',
           prop("Reference", "#PWR", 0, 0, hide=True),
           prop("Value", name, 0, 3.0 if glyph_up else -3.5, hide=False),
           prop("Footprint", "", 0, 0, hide=True),
           prop("Datasheet", "", 0, 0, hide=True),
           f'    (symbol "{lib_id.split(":")[1]}_0_1"']
    out.extend(graphics)
    out.append('    )')
    out.append(f'    (symbol "{lib_id.split(":")[1]}_1_1"')
    out.append(pin)
    out.append('    )')
    out.append('  )')
    return "\n".join(out)


# ── placed-instance + connectivity helpers ──────────────────────────────────
PINMODELS = {}   # lib_id -> {pin_number: (px, py)}  in symbol coords

def register_pins(lib_id, pins_spec):
    PINMODELS[lib_id] = {n: (x, y) for (n, x, y) in pins_spec}

def pin_xy(lib_id, num, sx, sy, mirror_x=False, rot=0):
    """Connection point of pin `num` of an instance at (sx,sy) in SHEET coords.
    Symbol +Y is up; sheet +Y is down -> negate Y. (rot/mirror kept simple:
    only rot 0 and mirror_x used here.)"""
    px, py = PINMODELS[lib_id][num]
    if mirror_x:
        px = -px
    return (sx + px, sy - py)

def label(name, x, y, rot=0, kind="label", shape="input"):
    uid = u()
    if kind == "label":
        return (f'  (label "{name}" (at {x:.2f} {y:.2f} {rot}) '
                f'{eff(1.27, "left bottom")} (uuid {uid}))')
    if kind == "hier":
        return (f'  (hierarchical_label "{name}" (shape {shape}) '
                f'(at {x:.2f} {y:.2f} {rot}) {eff(1.27, "left")} (uuid {uid}))')
    if kind == "global":
        return (f'  (global_label "{name}" (shape {shape}) '
                f'(at {x:.2f} {y:.2f} {rot}) {eff(1.27, "left")} (uuid {uid}))')

def instance(lib_id, ref, value, sx, sy, mirror_x=False, ref_dx=0, ref_dy=-20):
    pins = PINMODELS[lib_id]
    pin_lines = "\n".join(
        f'    (pin "{n}" (uuid {u()}))' for n in pins)
    mir = "    (mirror y)\n" if mirror_x else ""
    return f"""  (symbol
    (lib_id "{lib_id}")
    (at {sx:.2f} {sy:.2f} 0)
{mir}    (unit 1)
    (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
    (uuid {u()})
{prop("Reference", ref, sx + ref_dx, sy + ref_dy)}
{prop("Value", value, sx + ref_dx, sy + ref_dy + 2.0)}
{prop("Footprint", "", sx, sy, hide=True)}
{prop("Datasheet", "", sx, sy, hide=True)}
{pin_lines}
    (instances
      (project "{PROJECT}"
        (path "/{SHEET_OBJ_UUID}"
          (reference "{ref}") (unit 1))))
  )"""


# Register pin models (must mirror the lib symbol pin coords above) -----------
register_pins("74xx:74AHCT245", [
    ("1", -12.7, 10.16), ("19", -12.7, 7.62),
    ("2", -12.7, 2.54), ("3", -12.7, 0.0), ("4", -12.7, -2.54),
    ("5", -12.7, -5.08), ("6", -12.7, -7.62), ("7", -12.7, -10.16),
    ("8", -12.7, -12.7), ("9", -12.7, -15.24),
    ("20", 12.7, 10.16), ("10", 12.7, 7.62),
    ("18", 12.7, 2.54), ("17", 12.7, 0.0), ("16", 12.7, -2.54),
    ("15", 12.7, -5.08), ("14", 12.7, -7.62), ("13", 12.7, -10.16),
    ("12", 12.7, -12.7), ("11", 12.7, -15.24),
])
register_pins("Device:R", [("1", 0, 3.81), ("2", 0, -3.81)])
register_pins("Device:C", [("1", 0, 3.81), ("2", 0, -3.81)])
register_pins("Connector_Generic:Conn_02x08_Odd_Even",
              [(str(2 * i + 1), -5.08, 8.89 - i * 2.54) for i in range(8)] +
              [(str(2 * i + 2), 5.08, 8.89 - i * 2.54) for i in range(8)])
register_pins("Connector_Generic:Conn_01x08",
              [(str(i + 1), -5.08, 8.89 - i * 2.54) for i in range(8)])
register_pins("Connector_Generic:Conn_01x03",
              [(str(i + 1), -5.08, 2.54 - i * 2.54) for i in range(3)])


# ── BUILD THE SHEET ─────────────────────────────────────────────────────────
items = []        # placed symbol instances
nets = []         # labels

# --- U1: 8 HUB75 colour lines.  A_in (BCM) -> B_out (buffered signal) -------
U1_MAP = [  # (pin_in, BCM_net, pin_out, out_signal)
    ("2", "GPIO5",  "18", "R1"), ("3", "GPIO13", "17", "G1"),
    ("4", "GPIO6",  "16", "B1"), ("5", "GPIO12", "15", "R2"),
    ("6", "GPIO16", "14", "G2"), ("7", "GPIO23", "13", "B2"),
    ("8", "GPIO22", "12", "A"),  ("9", "GPIO26", "11", "B"),
]
U2_MAP = [
    ("2", "GPIO27", "18", "C"),  ("3", "GPIO20", "17", "D"),
    ("4", "GPIO24", "16", "E"),  ("5", "GPIO17", "15", "CLK_RAW"),
    ("6", "GPIO21", "14", "LAT_RAW"), ("7", "GPIO4", "13", "OE_RAW"),
    ("8", "SPARE1", "12", "SP1"), ("9", "SPARE2", "11", "SP2"),
]

U1_X, U1_Y = 90, 70
U2_X, U2_Y = 90, 130
items.append(instance("74xx:74AHCT245", "U1", "74AHCT245", U1_X, U1_Y))
items.append(instance("74xx:74AHCT245", "U2", "74AHCT245", U2_X, U2_Y))

def wire(x1, y1, x2, y2):
    return (f'  (wire (pts (xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f}))\n'
            f'    (stroke (width 0) (type default)) (uuid {u()}))')

LID = "74xx:74AHCT245"
for (sx, sy, mp) in [(U1_X, U1_Y, U1_MAP), (U2_X, U2_Y, U2_MAP)]:
    # DIR (pin1) -> +5V ; /OE (pin19) -> GND ; VCC (20) -> +5V ; GND (10) -> GND
    x, y = pin_xy(LID, "1", sx, sy);  nets.append(label("+5V", x, y, 180, "global", "input"));
    x, y = pin_xy(LID, "19", sx, sy); nets.append(label("GND", x, y, 180, "global", "input"))
    x, y = pin_xy(LID, "20", sx, sy); nets.append(label("+5V", x, y, 0, "global", "input"))
    x, y = pin_xy(LID, "10", sx, sy); nets.append(label("GND", x, y, 0, "global", "input"))
    for pin_in, bcm, pin_out, sig in mp:
        x, y = pin_xy(LID, pin_in, sx, sy)
        nets.append(label(bcm, x, y, 180, "hier", "input"))   # from CM5
        x, y = pin_xy(LID, pin_out, sx, sy)
        nets.append(label(sig, x, y, 0, "label"))             # buffered out

# --- Series resistors on CLK / LAT / OE (33R), raw -> J2 net ----------------
SERIES = [("R1", "CLK_RAW", "CLK", 150, 120), ("R2", "LAT_RAW", "LAT", 160, 120),
          ("R3", "OE_RAW", "OE", 170, 120)]
for ref, nin, nout, rx, ry in SERIES:
    items.append(instance("Device:R", ref, "33", rx, ry))
    x, y = pin_xy("Device:R", "1", rx, ry); nets.append(label(nin, x, y, 0, "label"))
    x, y = pin_xy("Device:R", "2", rx, ry); nets.append(label(nout, x, y, 0, "label"))

# --- J2: HUB75 2x8 IDC ------------------------------------------------------
J2_X, J2_Y = 210, 95
items.append(instance("Connector_Generic:Conn_02x08_Odd_Even", "J2",
                      "HUB75", J2_X, J2_Y))
J2_MAP = {1: "R1", 2: "G1", 3: "B1", 4: "GND", 5: "R2", 6: "G2", 7: "B2",
          8: "E", 9: "A", 10: "B", 11: "C", 12: "D", 13: "CLK", 14: "LAT",
          15: "OE", 16: "GND"}
for pnum, net in J2_MAP.items():
    side = 180 if pnum % 2 == 1 else 0
    x, y = pin_xy("Connector_Generic:Conn_02x08_Odd_Even", str(pnum), J2_X, J2_Y)
    kind = "global" if net == "GND" else "label"
    nets.append(label(net, x, y, side, kind, "input"))

# --- J3 / JP2 (MAX7219) intentionally NOT on this sheet -----------------------
# In the RP2354B-coprocessor architecture the CM5 drives ONLY HUB75; MAX7219 is
# owned by the RP2354B I/O MCU (its own SPI + 74AHCT245 buffer on the rp2354
# sheets). This sheet is therefore HUB75-only. U2 channels 7-8 (B7/B8) stay
# spare and break out at an N7 header in the full pass.

# --- Decoupling caps on U1/U2 VCC -------------------------------------------
for ref, cx, cy in [("C1", 70, 55), ("C2", 70, 115)]:
    items.append(instance("Device:C", ref, "100n", cx, cy))
    x, y = pin_xy("Device:C", "1", cx, cy); nets.append(label("+5V", x, y, 0, "global", "input"))
    x, y = pin_xy("Device:C", "2", cx, cy); nets.append(label("GND", x, y, 0, "global", "input"))

# ── on-canvas documentation text ────────────────────────────────────────────
doc = [
    "3. FACE BUFFER — HUB75 only, 3.3 V -> 5 V (CM5 side)",
    "U1/U2 74AHCT245 @ +5V (TTL VIH ~2V reads 3.3V CM5 as HIGH).",
    "DIR=+5V (A->B), /OE=GND (always enabled). 33R series on CLK/LAT/OE.",
    "GPIOxx hier-labels come from sheet 2 (CM5); +5V/GND are global rails.",
    "CM5 drives ONLY HUB75. MAX7219/WS2812/sensors/buttons/servos = RP2354B I/O MCU.",
    "NOTE: cached symbols are reconstructions -> run Symbol>Update from Library.",
]
doc_text = "\n".join(
    f'  (text "{t}" (at 20 {18 + i*3:.1f} 0) {eff(1.5, "left bottom")} (uuid {u()}))'
    for i, t in enumerate(doc))

# ── assemble file ────────────────────────────────────────────────────────────
# +5V / GND are expressed as global labels (not power symbols), so no power
# lib defs are emitted here — keeps the cached library minimal.
lib_defs = "\n".join([
    sym_245(), sym_R(), sym_C(), sym_conn_02x08(), sym_conn_01x08(),
    sym_conn_01x03(),
])

out = f"""(kicad_sch
  (version {SCH_VERSION})
  (generator "{GEN}")
  (generator_version "9.0")
  (uuid {SHEET_OBJ_UUID})
  (paper "A3")
  (title_block
    (title "ProtoHUD Carrier — 3. Face Buffer (HUB75)")
    (rev "A")
    (company "ProtoHUD")
  )
  (lib_symbols
{lib_defs}
  )
{doc_text}
{chr(10).join(items)}
{chr(10).join(nets)}
  (sheet_instances
    (path "/" (page "3"))
  )
)
"""

with open(os.path.join(HERE, "face_buffer.kicad_sch"), "w") as fh:
    fh.write(out)
print("wrote face_buffer.kicad_sch  (%d symbols, %d net labels)"
      % (len(items), len(nets)))
