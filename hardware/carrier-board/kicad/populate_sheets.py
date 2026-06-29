#!/usr/bin/env python3
"""Populate ALL carrier schematic sheets with real symbols + a label netlist.

Supersedes populate_face_buffer.py. Reuses the KiCad-9 emission format proven by
the face_buffer proof sheet (validated to open in KiCad 9). One shared symbol
library + a generic box-symbol builder; connectivity is by net LABELS placed on
pin connection points (global labels for rails / cross-sheet nets, local labels
for within-sheet nets) so it survives `Symbol > Update from Library`.

Big ICs (CM5, RP2354B, USB hub) are drawn as CONDENSED symbols exposing only the
pins the carrier uses — the CM5 is physically 2× DF40-100 and the RP2354B is
QFN-80; full-pad transcription is deferred to footprint/layout time.

Run:  python3 populate_sheets.py     (writes the 11 populated *.kicad_sch)
Pairs with generate_skeleton.py, which emits the root + project and skips every
file listed in its POPULATED set.
"""
import os, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = "ProtoHUD-Carrier"
SCH_VERSION = "20250114"          # KiCad 9 schematic format
GEN = "protohud_populate"

# Stable sheet-object UUIDs — MUST match generate_skeleton.py sheet_uuids.
SHEET_UUID = {
    "power":           "7fcf71da-42d5-4843-bf7d-f52f6f1fa308",
    "cm5":             "3812e784-9e95-4c5b-856a-ca4e97dc1851",
    "face_buffer":     "eff6a452-77ae-416d-87a8-cfe219f371e8",
    "rp2354_io":       "a1b2c3d4-0001-4aaa-9bbb-000000000004",
    "rp2354_face":     "a1b2c3d4-0002-4aaa-9bbb-000000000005",
    "leds":            "833500fc-08ba-4260-bcba-d50b8d06254c",
    "sensors_i2c":     "aa17a73e-e56b-4b1f-a092-064b3bde23cb",
    "servos":          "a1b2c3d4-0003-4aaa-9bbb-000000000008",
    "gpio_buttons":    "4d61ad14-7947-4b5f-b9ac-ea72b52475d0",
    "usb":             "b12d177a-080c-49c4-8372-6cdd7e6e9f72",
    "cameras_display": "c814e905-b80c-42d1-beeb-925e48192849",
}

def u():
    return str(uuid.uuid4())

# ── S-expr helpers (identical behaviour to the proven proof sheet) ───────────
def eff(size=1.27, justify="", hide=False):
    j = f" (justify {justify})" if justify else ""
    h = " (hide yes)" if hide else ""
    return f"(effects (font (size {size} {size})){j}{h})"

def prop(name, value, x, y, rot=0, hide=False):
    return f'    (property "{name}" "{value}" (at {x:.2f} {y:.2f} {rot}) {eff(1.27, "", hide)})'

# ── symbol library ──────────────────────────────────────────────────────────
LIB = []                 # list of lib_symbol s-expr strings
PINS = {}                # lib_id -> {pin_number: (px, py)}

def _pin(num, name, etype, x, y, rot, length=2.54):
    return (f'      (pin {etype} line (at {x:.2f} {y:.2f} {rot}) (length {length})\n'
            f'        (name "{name}" {eff(1.0)})\n'
            f'        (number "{num}" {eff(1.0)}))')

def box_symbol(lib_id, ref_prefix, value, left, right, width=25.4, hide_nums=False):
    """left/right: list of (number, name, etype). Builds a rectangular symbol."""
    base = lib_id.split(":")[1]
    n = max(len(left), len(right), 1)
    top = (n - 1) / 2 * 2.54
    half = width / 2
    pins = []
    coords = {}
    for i, (num, name, et) in enumerate(left):
        y = top - i * 2.54
        pins.append(_pin(num, name, et, -half - 2.54, y, 0))
        coords[num] = (-half - 2.54, y)
    for i, (num, name, et) in enumerate(right):
        y = top - i * 2.54
        pins.append(_pin(num, name, et, half + 2.54, y, 180))
        coords[num] = (half + 2.54, y)
    bot = top - (n - 1) * 2.54
    rect = (f'      (rectangle (start {-half:.2f} {top + 2.54:.2f}) (end {half:.2f} {bot - 2.54:.2f})\n'
            f'        (stroke (width 0.254) (type default)) (fill (type background)))')
    pn = '    (pin_numbers (hide yes))\n' if hide_nums else ''
    LIB.append(
        f'  (symbol "{lib_id}"\n{pn}'
        f'    (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)\n'
        f'{prop("Reference", ref_prefix, 0, top + 3.81)}\n'
        f'{prop("Value", value, 0, bot - 3.81)}\n'
        f'{prop("Footprint", "", 0, 0, hide=True)}\n'
        f'{prop("Datasheet", "", 0, 0, hide=True)}\n'
        f'    (symbol "{base}_0_1"\n{rect}\n    )\n'
        f'    (symbol "{base}_1_1"\n' + "\n".join(pins) + '\n    )\n  )')
    PINS[lib_id] = coords
    return lib_id

def passive2(lib_id, ref_prefix, value, vertical=True, cap=False):
    """2-pin passive (R / C / Crystal / Fuse / TVS), pins on Y axis."""
    base = lib_id.split(":")[1]
    if cap:
        body = ('      (polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) (stroke (width 0.508) (type default)) (fill (type none)))\n'
                '      (polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) (stroke (width 0.508) (type default)) (fill (type none)))')
        len1 = 2.794
    else:
        body = ('      (rectangle (start -1.016 2.54) (end 1.016 -2.54) (stroke (width 0.254) (type default)) (fill (type none)))')
        len1 = 1.27
    p1 = _pin("1", "~", "passive", 0, 3.81, 270, len1)
    p2 = _pin("2", "~", "passive", 0, -3.81, 90, len1)
    LIB.append(
        f'  (symbol "{lib_id}"\n    (pin_numbers (hide yes))\n'
        f'    (pin_names (offset 0)) (exclude_from_sim no) (in_bom yes) (on_board yes)\n'
        f'{prop("Reference", ref_prefix, 2.54, 0)}\n'
        f'{prop("Value", value, -2.54, 0)}\n'
        f'{prop("Footprint", "", 0, 0, hide=True)}\n'
        f'{prop("Datasheet", "", 0, 0, hide=True)}\n'
        f'    (symbol "{base}_0_1"\n{body}\n    )\n'
        f'    (symbol "{base}_1_1"\n{p1}\n{p2}\n    )\n  )')
    PINS[lib_id] = {"1": (0, 3.81), "2": (0, -3.81)}
    return lib_id

def conn(n):
    """Generic 1×N connector symbol, all pins on the left."""
    lib_id = f"Connector_Generic:Conn_01x{n:02d}"
    if lib_id in PINS:
        return lib_id
    left = [(str(i + 1), f"Pin_{i+1}", "passive") for i in range(n)]
    return box_symbol(lib_id, "J", f"Conn_01x{n:02d}", left, [], width=7.62, hide_nums=True)

# ── instance + label emitters (proven format) ───────────────────────────────
def instance(lib_id, ref, value, sx, sy, ref_dy=None):
    if ref_dy is None:
        ref_dy = -16
    pin_lines = "\n".join(f'    (pin "{nm}" (uuid {u()}))' for nm in PINS[lib_id])
    return f"""  (symbol
    (lib_id "{lib_id}")
    (at {sx:.2f} {sy:.2f} 0)
    (unit 1)
    (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)
    (uuid {u()})
{prop("Reference", ref, sx, sy + ref_dy)}
{prop("Value", value, sx, sy + ref_dy + 2.0)}
{prop("Footprint", "", sx, sy, hide=True)}
{prop("Datasheet", "", sx, sy, hide=True)}
{pin_lines}
    (instances
      (project "{PROJECT}"
        (path "/{CUR_SHEET_UUID}"
          (reference "{ref}") (unit 1))))
  )"""

def pin_xy(lib_id, num, sx, sy):
    px, py = PINS[lib_id][num]
    return (sx + px, sy - py)

def lbl(name, x, y, kind="local", rot=0):
    uid = u()
    if kind == "global":
        return f'  (global_label "{name}" (shape bidirectional) (at {x:.2f} {y:.2f} {rot}) {eff(1.27, "left")} (uuid {uid}))'
    if kind == "power":
        return f'  (global_label "{name}" (shape input) (at {x:.2f} {y:.2f} {rot}) {eff(1.27, "left")} (uuid {uid}))'
    return f'  (label "{name}" (at {x:.2f} {y:.2f} {rot}) {eff(1.27, "left bottom")} (uuid {uid}))'

def at_pin(lib_id, num, sx, sy, name, kind="local"):
    x, y = pin_xy(lib_id, num, sx, sy)
    rot = 180 if PINS[lib_id][num][0] < 0 else 0
    return lbl(name, x, y, kind, rot)

# ── define the symbol library (built once, embedded in every sheet) ─────────
SYM_245   = box_symbol("74xx:74AHCT245", "U", "74AHCT245",
    [("1","DIR","input"),("19","~{OE}","input"),("2","A1","input"),("3","A2","input"),
     ("4","A3","input"),("5","A4","input"),("6","A5","input"),("7","A6","input"),
     ("8","A7","input"),("9","A8","input"),("20","VCC","power_in"),("10","GND","power_in")],
    [("18","B1","output"),("17","B2","output"),("16","B3","output"),("15","B4","output"),
     ("14","B5","output"),("13","B6","output"),("12","B7","output"),("11","B8","output")],
    width=30.48)
SYM_125   = box_symbol("74xx:74AHCT125", "U", "74AHCT125",
    [("1","1~{OE}","input"),("2","1A","input"),("4","2~{OE}","input"),("5","2A","input"),
     ("10","3~{OE}","input"),("9","3A","input"),("13","4~{OE}","input"),("12","4A","input"),
     ("14","VCC","power_in"),("7","GND","power_in")],
    [("3","1Y","output"),("6","2Y","output"),("8","3Y","output"),("11","4Y","output")],
    width=27.94)
SYM_R     = passive2("Device:R", "R", "R")
SYM_C     = passive2("Device:C", "C", "C", cap=True)
SYM_XTAL  = passive2("Device:Crystal", "Y", "12MHz")
SYM_FUSE  = passive2("Device:Fuse", "F", "Fuse")
SYM_TVS   = passive2("Device:D_TVS", "D", "SMBJ5.0A")
SYM_PFET  = box_symbol("Device:Q_PMOS_GSD", "Q", "DMP3098L",
    [("1","G","input")], [("2","D","passive"),("3","S","passive")], width=10.16)
SYM_NFET  = box_symbol("Device:Q_NMOS_GSD", "Q", "NMOS",
    [("1","G","input")], [("2","D","passive"),("3","S","passive")], width=10.16)
SYM_LDO   = box_symbol("Regulator_Linear:AP2112K-3.3", "U", "AP2112K-3.3",
    [("1","VIN","power_in"),("3","EN","input"),("2","GND","power_in")],
    [("5","VOUT","power_out"),("4","BYP","passive")], width=20.32)
SYM_ESD   = box_symbol("Interface_USB:USBLC6-2SC6", "U", "USBLC6-2",
    [("1","IO1","bidirectional"),("2","GND","power_in"),("3","IO2","bidirectional")],
    [("6","IO1","bidirectional"),("5","VBUS","power_in"),("4","IO2","bidirectional")],
    width=17.78)
SYM_USBC  = box_symbol("Connector:USB_C_Receptacle_USB2.0", "J", "USB_C",
    [("A4","VBUS","power_out"),("A9","VBUS","power_out"),("A1","GND","power_in"),
     ("A6","D+","bidirectional"),("A7","D-","bidirectional"),
     ("A5","CC1","bidirectional"),("B5","CC2","bidirectional")], [], width=17.78)
SYM_SW1   = box_symbol("Connector:USB_Selector", "SW", "USB sel (DPDT/mux)",
    [("1","COM_D+","bidirectional"),("2","COM_D-","bidirectional")],
    [("3","A_D+","bidirectional"),("4","A_D-","bidirectional"),
     ("5","B_D+","bidirectional"),("6","B_D-","bidirectional")], width=25.4)
# VL817 — 4-port USB 3.1 Gen1 (5 Gbps) hub. Condensed symbol: mnemonic pin
# "numbers" expose only the carrier nets (real QFN-68 pads are a layout task).
SYM_HUB3  = box_symbol("Interface_USB:VL817", "U", "VL817-Q7",
    [("UP_DP","UP_D+","bidirectional"),("UP_DM","UP_D-","bidirectional"),
     ("UP_SSRXP","UP_SSRX+","bidirectional"),("UP_SSRXM","UP_SSRX-","bidirectional"),
     ("UP_SSTXP","UP_SSTX+","bidirectional"),("UP_SSTXM","UP_SSTX-","bidirectional"),
     ("VBUS_DET","VBUS_DET","input"),("RESETB","~{RESET}","input"),
     ("SELFPWR","SELF_PWR","input"),("XI","XI","input"),("XO","XO","output"),
     ("VDD5","VDD5","power_in"),("VDD33","VDD33","power_out"),
     ("VDD12","VDD12","power_out"),("GND","GND","power_in")],
    [("DP1","P1_D+","bidirectional"),("DM1","P1_D-","bidirectional"),
     ("SSTXP1","P1_SSTX+","bidirectional"),("SSTXM1","P1_SSTX-","bidirectional"),
     ("SSRXP1","P1_SSRX+","bidirectional"),("SSRXM1","P1_SSRX-","bidirectional"),
     ("DP2","P2_D+","bidirectional"),("DM2","P2_D-","bidirectional"),
     ("SSTXP2","P2_SSTX+","bidirectional"),("SSTXM2","P2_SSTX-","bidirectional"),
     ("SSRXP2","P2_SSRX+","bidirectional"),("SSRXM2","P2_SSRX-","bidirectional"),
     ("DP3","P3_D+","bidirectional"),("DM3","P3_D-","bidirectional"),
     ("SSTXP3","P3_SSTX+","bidirectional"),("SSTXM3","P3_SSTX-","bidirectional"),
     ("SSRXP3","P3_SSRX+","bidirectional"),("SSRXM3","P3_SSRX-","bidirectional"),
     ("DP4","P4_D+","bidirectional"),("DM4","P4_D-","bidirectional"),
     ("SSTXP4","P4_SSTX+","bidirectional"),("SSTXM4","P4_SSTX-","bidirectional"),
     ("SSRXP4","P4_SSRX+","bidirectional"),("SSRXM4","P4_SSRX-","bidirectional")],
    width=33.02)
# USB-C 3.1 receptacle (downstream / host-facing). Condensed: VBUS/GND/CC/D±
# + one SuperSpeed lane (TX1/RX1). Full flip needs A/B-side D± bonded at the
# footprint + a per-port 2:1 SS mux for orientation 2 (see usb sheet note).
SYM_USBC3 = box_symbol("Connector:USB_C_Receptacle_USB3.1", "J", "USB_C_3.1",
    [("A4","VBUS","power_in"),("A1","GND","power_in"),
     ("A5","CC1","bidirectional"),("B5","CC2","bidirectional"),
     ("A6","D+","bidirectional"),("A7","D-","bidirectional"),
     ("A2","SSTX1+","bidirectional"),("A3","SSTX1-","bidirectional"),
     ("B11","SSRX1+","bidirectional"),("B10","SSRX1-","bidirectional")],
    [], width=20.32)

# RP2354B — condensed: only pins the carrier uses.
SYM_RP = box_symbol("MCU_RaspberryPi:RP2354B", "U", "RP2354B",
    [("49","USB_DP","bidirectional"),("48","USB_DM","bidirectional"),
     ("RUN","RUN","input"),("SWCLK","SWCLK","input"),("SWDIO","SWDIO","bidirectional"),
     ("XIN","XIN","input"),("XOUT","XOUT","output"),
     ("IOVDD","IOVDD","power_in"),("DVDD","DVDD","power_in"),("GND","GND","power_in"),
     ("0","GP0 DBG_TX","output"),("1","GP1 DBG_RX","input"),
     ("2","GP2 MX_CLK","output"),("3","GP3 MX_DIN","output"),
     ("4","GP4 SDA0","bidirectional"),("5","GP5 SCL0","bidirectional"),
     ("6","GP6 SENS_INT","input")],
    [("7","GP7 MX_CS1","output"),("8","GP8 MX_CS2","output"),
     ("9","GP9 MX_CS3","output"),("10","GP10 MX_CS4","output"),
     ("16","GP16 LED1","output"),("17","GP17 LED2","output"),
     ("18","GP18 LED3","output"),("19","GP19 LED4","output"),
     ("20","GP20 SRV1","output"),("21","GP21 SRV2","output"),
     ("22","GP22 SRV3","output"),("23","GP23 SRV4","output"),
     ("24","GP24 SRV5","output"),("25","GP25 SRV6","output"),
     ("26","GP26 SRV7","output"),("27","GP27 SRV8","output"),
     ("28","GP28-37 BTN1-10","input"),("40","GP40-47 AIN0-7","input")],
    width=45.72)

# CM5 — condensed module (physically 2× DF40-100).
SYM_CM5 = box_symbol("Module:RaspberryPi_CM5", "U", "CM5 (2x DF40-100)",
    [("5V","+5V","power_in"),("3V3","+3V3","power_out"),("GND","GND","power_in"),
     ("CSI0","CSI0 (MIPI)","output"),("CSI1","CSI1 (MIPI)","output"),
     ("HDMI0","HDMI0","output"),("HDMI1","HDMI1","output"),
     # USB3.0 port #0 -> VL817 hub upstream (USB2 pair + SuperSpeed)
     ("USB_DP","USB0_D+","bidirectional"),("USB_DM","USB0_D-","bidirectional"),
     ("USB_SSTXP","USB0_SSTX+","bidirectional"),("USB_SSTXM","USB0_SSTX-","bidirectional"),
     ("USB_SSRXP","USB0_SSRX+","bidirectional"),("USB_SSRXM","USB0_SSRX-","bidirectional"),
     # USB3.0 port #1 -> RP2354B (off-hub, uses USB2 pair only)
     ("RPUSB_DP","USB1_D+ (RP)","bidirectional"),("RPUSB_DM","USB1_D- (RP)","bidirectional"),
     # USB2.0 port -> backpack phone uplink (J11)
     ("USB2_DP","USB2_D+ (bp)","bidirectional"),("USB2_DM","USB2_D- (bp)","bidirectional"),
     ("BCM7","BCM7 RP_RUN","output"),("BCM8","BCM8 RP_BOOTSEL","output"),
     ("BCM18","BCM18 FAN1","output"),("BCM19","BCM19 FAN2","output")],
    [("BCM4","BCM4 OE","output"),("BCM5","BCM5 R1","output"),("BCM6","BCM6 B1","output"),
     ("BCM12","BCM12 R2","output"),("BCM13","BCM13 G1","output"),("BCM16","BCM16 G2","output"),
     ("BCM17","BCM17 CLK","output"),("BCM20","BCM20 D","output"),("BCM21","BCM21 STB","output"),
     ("BCM22","BCM22 A","output"),("BCM23","BCM23 B2","output"),("BCM24","BCM24 E","output"),
     ("BCM26","BCM26 B","output"),("BCM27","BCM27 C","output")],
    width=50.8)
# HDMI / FFC big connectors
for _n in (4,):
    conn(_n)


# ── per-sheet builders : return (doc_lines, items, nets) ─────────────────────
def sh_power():
    items, nets, doc = [], [], [
        "1. POWER — 5 V input -> rails (+3V3_RP local).  See POWER.md.",
        "J1 5V in -> RP-FET(Q1) -> TVS(D1) -> +5V bus; fuses to PANEL/LED/SERVO;",
        "AP2112 LDO -> +3V3_RP. Bulk caps per rail. Rails are GLOBAL labels.",
    ]
    J1 = conn(2)
    items.append(instance(J1, "J1", "5V IN", 40, 40))
    nets += [at_pin(J1,"1",40,40,"+5V_IN"), at_pin(J1,"2",40,40,"GND","power")]
    items.append(instance(SYM_PFET, "Q1", "P-FET", 70, 40))
    nets += [at_pin(SYM_PFET,"1",70,40,"+5V_IN"), at_pin(SYM_PFET,"2",70,40,"+5V_IN"),
             at_pin(SYM_PFET,"3",70,40,"+5V","power")]
    items.append(instance(SYM_TVS, "D1", "SMBJ5.0A", 95, 45))
    nets += [at_pin(SYM_TVS,"1",95,45,"+5V","power"), at_pin(SYM_TVS,"2",95,45,"GND","power")]
    items.append(instance(SYM_C, "C1", "2200uF", 110, 45))
    nets += [at_pin(SYM_C,"1",110,45,"+5V","power"), at_pin(SYM_C,"2",110,45,"GND","power")]
    # rail fuses
    for i,(ref,rail,x) in enumerate([("F1","+5V_PANEL",140),("F2","+5V_LED",160),("F3","+V_SERVO",180)]):
        items.append(instance(SYM_FUSE, ref, "Fuse", x, 40))
        nets += [at_pin(SYM_FUSE,"1",x,40,"+5V","power"), at_pin(SYM_FUSE,"2",x,40,rail,"power")]
        items.append(instance(SYM_C, f"C{10+i}", "1000uF", x, 60))
        nets += [at_pin(SYM_C,"1",x,60,rail,"power"), at_pin(SYM_C,"2",x,60,"GND","power")]
    # +3V3_RP LDO
    items.append(instance(SYM_LDO, "U1", "AP2112K-3.3", 70, 90))
    nets += [at_pin(SYM_LDO,"1",70,90,"+5V","power"), at_pin(SYM_LDO,"3",70,90,"+5V","power"),
             at_pin(SYM_LDO,"2",70,90,"GND","power"), at_pin(SYM_LDO,"5",70,90,"+3V3_RP","power")]
    items.append(instance(SYM_C, "C20", "10uF", 50, 90))
    nets += [at_pin(SYM_C,"1",50,90,"+5V","power"), at_pin(SYM_C,"2",50,90,"GND","power")]
    items.append(instance(SYM_C, "C21", "10uF", 100, 90))
    nets += [at_pin(SYM_C,"1",100,90,"+3V3_RP","power"), at_pin(SYM_C,"2",100,90,"GND","power")]
    return doc, items, nets

def sh_cm5():
    items, nets, doc = [], [], [
        "2. CM5 — drives ONLY HUB75 (+ CSI/HDMI/USB). Condensed module symbol;",
        "physically 2x DF40-100. HUB75 BCM -> face_buffer via global labels.",
        "BCM7/8 = RP2354B RUN/BOOTSEL; BCM18/19 = fan MOSFET gates.",
    ]
    items.append(instance(SYM_CM5, "U2", "CM5", 110, 90, ref_dy=-40))
    cm = SYM_CM5
    nets += [at_pin(cm,"5V",110,90,"+5V","power"), at_pin(cm,"3V3",110,90,"+3V3","power"),
             at_pin(cm,"GND",110,90,"GND","power")]
    for p,net in [("CSI0","CSI0"),("CSI1","CSI1"),("HDMI0","HDMI0"),("HDMI1","HDMI1"),
                  ("USB_DP","CM5_USB_DP"),("USB_DM","CM5_USB_DM"),       # USB3 p#0 USB2 -> hub
                  ("USB_SSRXP","CM5_SSRX_P"),("USB_SSRXM","CM5_SSRX_M"), # hub TX -> CM5 RX (caps at hub)
                  ("RPUSB_DP","CM5_RPUSB_DP"),("RPUSB_DM","CM5_RPUSB_DM"),# USB3 p#1 USB2 -> RP2354B
                  ("USB2_DP","CM5_USB2_DP"),("USB2_DM","CM5_USB2_DM"),   # USB2 port -> backpack J11
                  ("BCM7","RP_RUN"),("BCM8","RP_BOOTSEL"),("BCM18","FAN1_G"),("BCM19","FAN2_G")]:
        nets.append(at_pin(cm,p,110,90,net,"global"))
    # USB3 upstream TX pair: AC-coupling caps belong at the transmitter (CM5).
    # CM5 SSTX -> C -> CM5_SSTX_* (link net) -> hub UP_SSRX.
    items.append(instance(SYM_C,"C5","100n",55,40)); nets+=[at_pin(SYM_C,"1",55,40,"CM5_SSTX_PI","local"),at_pin(SYM_C,"2",55,40,"CM5_SSTX_P","global")]
    items.append(instance(SYM_C,"C6","100n",70,40)); nets+=[at_pin(SYM_C,"1",70,40,"CM5_SSTX_MI","local"),at_pin(SYM_C,"2",70,40,"CM5_SSTX_M","global")]
    nets += [at_pin(cm,"USB_SSTXP",110,90,"CM5_SSTX_PI","local"), at_pin(cm,"USB_SSTXM",110,90,"CM5_SSTX_MI","local")]
    hub = {"BCM4":"GPIO4","BCM5":"GPIO5","BCM6":"GPIO6","BCM12":"GPIO12","BCM13":"GPIO13",
           "BCM16":"GPIO16","BCM17":"GPIO17","BCM20":"GPIO20","BCM21":"GPIO21","BCM22":"GPIO22",
           "BCM23":"GPIO23","BCM24":"GPIO24","BCM26":"GPIO26","BCM27":"GPIO27"}
    for p,net in hub.items():
        nets.append(at_pin(cm,p,110,90,net,"global"))
    # fan MOSFETs + headers
    for i,(q,gate,hdr,x) in enumerate([("Q10","FAN1_G","J30",170),("Q11","FAN2_G","J31",190)]):
        items.append(instance(SYM_NFET, q, "NMOS", x, 150))
        nets += [at_pin(SYM_NFET,"1",x,150,gate,"global"), at_pin(SYM_NFET,"3",x,150,"GND","power")]
        J = conn(4)
        items.append(instance(J, hdr, "FAN", x, 175))
        nets += [at_pin(J,"1",x,175,"GND","power"), at_pin(J,"2",x,175,"+5V","power")]
    # nRPIBOOT + eMMC USB
    JB = conn(2); items.append(instance(JB, "JP_BOOT", "nRPIBOOT", 60, 150))
    return doc, items, nets

def sh_face_buffer():
    items, nets, doc = [], [], [
        "3. FACE BUFFER — HUB75 only, 3.3->5 V (CM5 side).",
        "U3/U4 74AHCT245 @5V; DIR=+5V /OE=GND; 33R on CLK/LAT/OE -> J2 (2x8 IDC).",
        "GPIOxx global labels come from sheet 2 (CM5).",
    ]
    J2 = box_symbol("Connector_Generic:Conn_02x08_Odd_Even","J","HUB75",
        [(str(2*i+1),f"P{2*i+1}","passive") for i in range(8)],
        [(str(2*i+2),f"P{2*i+2}","passive") for i in range(8)], width=10.16, hide_nums=True)
    U1MAP=[("2","GPIO5","18","R1"),("3","GPIO13","17","G1"),("4","GPIO6","16","B1"),
           ("5","GPIO12","15","R2"),("6","GPIO16","14","G2"),("7","GPIO23","13","B2"),
           ("8","GPIO22","12","A"),("9","GPIO26","11","B")]
    U2MAP=[("2","GPIO27","18","C"),("3","GPIO20","17","D"),("4","GPIO24","16","E"),
           ("5","GPIO17","15","CLK_RAW"),("6","GPIO21","14","LAT_RAW"),("7","GPIO4","13","OE_RAW")]
    for ref,sx,sy,mp in [("U3",70,70,U1MAP),("U4",70,135,U2MAP)]:
        items.append(instance(SYM_245, ref, "74AHCT245", sx, sy, ref_dy=-22))
        nets += [at_pin(SYM_245,"1",sx,sy,"+5V","power"), at_pin(SYM_245,"19",sx,sy,"GND","power"),
                 at_pin(SYM_245,"20",sx,sy,"+5V","power"), at_pin(SYM_245,"10",sx,sy,"GND","power")]
        for pi,gp,po,sig in mp:
            nets.append(at_pin(SYM_245,pi,sx,sy,gp,"global"))
            nets.append(at_pin(SYM_245,po,sx,sy,sig,"local"))
    for ref,nin,nout,x in [("R1","CLK_RAW","CLK",130),("R2","LAT_RAW","LAT",140),("R3","OE_RAW","OE",150)]:
        items.append(instance(SYM_R, ref, "33", x, 120))
        nets += [at_pin(SYM_R,"1",x,120,nin,"local"), at_pin(SYM_R,"2",x,120,nout,"local")]
    items.append(instance(J2,"J2","HUB75",185,95,ref_dy=-22))
    J2MAP={1:"R1",2:"G1",3:"B1",4:"GND",5:"R2",6:"G2",7:"B2",8:"E",9:"A",10:"B",
           11:"C",12:"D",13:"CLK",14:"LAT",15:"OE",16:"GND"}
    for pn,net in J2MAP.items():
        nets.append(at_pin(J2,str(pn),185,95,net,"power" if net=="GND" else "local"))
    for ref,x in [("C1",50,),("C2",50)]:
        pass
    items.append(instance(SYM_C,"C1","100n",45,60)); nets+=[at_pin(SYM_C,"1",45,60,"+5V","power"),at_pin(SYM_C,"2",45,60,"GND","power")]
    items.append(instance(SYM_C,"C2","100n",45,125)); nets+=[at_pin(SYM_C,"1",45,125,"+5V","power"),at_pin(SYM_C,"2",45,125,"GND","power")]
    return doc, items, nets

def sh_rp2354_io():
    items, nets, doc = [], [], [
        "4. RP2354B I/O MCU — USB-CDC to CM5 via hub.  See RP2354-IO.md.",
        "Condensed symbol (QFN-80). GP nets are global labels to other sheets.",
        "USB pair -> SW1 selector -> CM5 USB3 port#1 (A, off-hub) / J12 USB-C (B). 12MHz xtal, SWD, BOOTSEL.",
    ]
    items.append(instance(SYM_RP, "U5", "RP2354B", 110, 100, ref_dy=-46))
    rp = SYM_RP
    nets += [at_pin(rp,"IOVDD",110,100,"+3V3_RP","power"), at_pin(rp,"DVDD",110,100,"+3V3_RP","power"),
             at_pin(rp,"GND",110,100,"GND","power")]
    nets += [at_pin(rp,"49",110,100,"RP_USB_DP","global"), at_pin(rp,"48",110,100,"RP_USB_DM","global"),
             at_pin(rp,"RUN",110,100,"RP_RUN","global"),
             at_pin(rp,"SWCLK",110,100,"SWCLK","local"), at_pin(rp,"SWDIO",110,100,"SWDIO","local"),
             at_pin(rp,"XIN",110,100,"XIN","local"), at_pin(rp,"XOUT",110,100,"XOUT","local")]
    gp = {"0":"DBG_TX","1":"DBG_RX","2":"MX_CLK","3":"MX_DIN","4":"SDA0","5":"SCL0","6":"SENS_INT",
          "7":"MX_CS1","8":"MX_CS2","9":"MX_CS3","10":"MX_CS4","16":"LED1_DAT","17":"LED2_DAT",
          "18":"LED3_DAT","19":"LED4_DAT","20":"SRV1","21":"SRV2","22":"SRV3","23":"SRV4",
          "24":"SRV5","25":"SRV6","26":"SRV7","27":"SRV8","28":"BTN_BUS","40":"AIN_BUS"}
    for p,net in gp.items():
        nets.append(at_pin(rp,p,110,100,net,"global"))
    # crystal
    items.append(instance(SYM_XTAL,"Y1","12MHz",60,120))
    nets += [at_pin(SYM_XTAL,"1",60,120,"XIN","local"), at_pin(SYM_XTAL,"2",60,120,"XOUT","local")]
    # SW1 selector + J12 USB-C + ESD
    items.append(instance(SYM_SW1,"SW1","USB sel",170,70,ref_dy=-12))
    nets += [at_pin(SYM_SW1,"1",170,70,"RP_USB_DP","global"), at_pin(SYM_SW1,"2",170,70,"RP_USB_DM","global"),
             at_pin(SYM_SW1,"3",170,70,"CM5_RPUSB_DP","global"), at_pin(SYM_SW1,"4",170,70,"CM5_RPUSB_DM","global"),
             at_pin(SYM_SW1,"5",170,70,"J12_DP","local"), at_pin(SYM_SW1,"6",170,70,"J12_DM","local")]
    items.append(instance(SYM_USBC,"J12","USB-C prog",200,110,ref_dy=-14))
    nets += [at_pin(SYM_USBC,"A6",200,110,"J12_DP","local"), at_pin(SYM_USBC,"A7",200,110,"J12_DM","local"),
             at_pin(SYM_USBC,"A1",200,110,"GND","power")]
    items.append(instance(SYM_ESD,"U6","USBLC6-2",185,140))
    nets += [at_pin(SYM_ESD,"1",185,140,"J12_DP","local"), at_pin(SYM_ESD,"3",185,140,"J12_DM","local"),
             at_pin(SYM_ESD,"2",185,140,"GND","power")]
    # SWD + BOOTSEL/RUN buttons
    JS = conn(4); items.append(instance(JS,"J13","SWD",60,150))
    nets += [at_pin(JS,"1",60,150,"SWCLK","local"), at_pin(JS,"2",60,150,"SWDIO","local"),
             at_pin(JS,"3",60,150,"GND","power"), at_pin(JS,"4",60,150,"+3V3_RP","power")]
    items.append(instance(SYM_C,"C30","100n",45,90)); nets+=[at_pin(SYM_C,"1",45,90,"+3V3_RP","power"),at_pin(SYM_C,"2",45,90,"GND","power")]
    return doc, items, nets

def sh_rp2354_face():
    items, nets, doc = [], [], [
        "5. MAX7219 buffer — RP2354B side, 3.3->5 V.  U10 74AHCT245 -> J3.",
        "MX_CLK/MX_DIN/MX_CS1..4 (global, from RP2354B) buffered to 5 V.",
    ]
    items.append(instance(SYM_245,"U10","74AHCT245",80,90,ref_dy=-22))
    inmap=[("2","MX_DIN","18","M_DIN"),("3","MX_CLK","17","M_CLK"),("4","MX_CS1","16","M_CS1"),
           ("5","MX_CS2","15","M_CS2"),("6","MX_CS3","14","M_CS3"),("7","MX_CS4","13","M_CS4")]
    nets += [at_pin(SYM_245,"1",80,90,"+5V","power"),at_pin(SYM_245,"19",80,90,"GND","power"),
             at_pin(SYM_245,"20",80,90,"+5V","power"),at_pin(SYM_245,"10",80,90,"GND","power")]
    for pi,gp,po,sig in inmap:
        nets.append(at_pin(SYM_245,pi,80,90,gp,"global"))
        nets.append(at_pin(SYM_245,po,80,90,sig,"local"))
    J3 = conn(8); items.append(instance(J3,"J3","MAX7219",150,90,ref_dy=-12))
    j3={1:"+5V",2:"GND",3:"M_DIN",4:"M_CLK",5:"M_CS1",6:"M_CS2",7:"M_CS3",8:"M_CS4"}
    for pn,net in j3.items():
        kind = "power" if net in ("+5V","GND") else "local"
        nets.append(at_pin(J3,str(pn),150,90,net,kind))
    items.append(instance(SYM_C,"C40","100n",55,70)); nets+=[at_pin(SYM_C,"1",55,70,"+5V","power"),at_pin(SYM_C,"2",55,70,"GND","power")]
    return doc, items, nets

def sh_leds():
    items, nets, doc = [], [], [
        "6. WS2812 LEDs — RP2354B side, 4 zones.  U11 74AHCT125 -> J4a..d.",
        "LED1..4_DAT (global, PIO from RP2354B) buffered to 5 V; +5V_LED fused.",
    ]
    items.append(instance(SYM_125,"U11","74AHCT125",80,95,ref_dy=-16))
    nets += [at_pin(SYM_125,"14",80,95,"+5V","power"),at_pin(SYM_125,"7",80,95,"GND","power")]
    chans=[("2","3","LED1_DAT","LED1_5V"),("5","6","LED2_DAT","LED2_5V"),
           ("9","8","LED3_DAT","LED3_5V"),("12","11","LED4_DAT","LED4_5V")]
    oes=["1","4","10","13"]
    for oe in oes:
        nets.append(at_pin(SYM_125,oe,80,95,"GND","power"))   # /OE low = enabled
    for a,y,gnet,onet in chans:
        nets.append(at_pin(SYM_125,a,80,95,gnet,"global"))
        nets.append(at_pin(SYM_125,y,80,95,onet,"local"))
    for i,(a,y,gnet,onet) in enumerate(chans):
        J = conn(3); ref=f"J4{chr(97+i)}"; x=140+i*18
        items.append(instance(J,ref,f"WS2812-{i+1}",x,120,ref_dy=-12))
        nets += [at_pin(J,"1",x,120,"+5V_LED","power"), at_pin(J,"2",x,120,onet,"local"),
                 at_pin(J,"3",x,120,"GND","power")]
        items.append(instance(SYM_R,f"R1{i}","330",x,135))
        nets += [at_pin(SYM_R,"1",x,135,onet,"local"), at_pin(SYM_R,"2",x,135,onet,"local")]
    items.append(instance(SYM_C,"C50","100n",55,75)); nets+=[at_pin(SYM_C,"1",55,75,"+5V","power"),at_pin(SYM_C,"2",55,75,"GND","power")]
    items.append(instance(SYM_C,"C51","1000uF",165,150)); nets+=[at_pin(SYM_C,"1",165,150,"+5V_LED","power"),at_pin(SYM_C,"2",165,150,"GND","power")]
    return doc, items, nets

def sh_sensors():
    items, nets, doc = [], [], [
        "7. Sensors I2C0 — RP2354B master, 3.3 V direct.  J5 + 4.7k pull-ups.",
        "SDA0/SCL0/SENS_INT global from RP2354B; pull-ups to +3V3_RP. STEMMA QT JX1.",
        "Addrs: BNO055 0x28, MPU9250 0x68, MPR121 0x5A, BH1750 0x23.",
    ]
    items.append(instance(SYM_R,"R20","4.7k",70,80))
    nets += [at_pin(SYM_R,"1",70,80,"+3V3_RP","power"), at_pin(SYM_R,"2",70,80,"SDA0","global")]
    items.append(instance(SYM_R,"R21","4.7k",85,80))
    nets += [at_pin(SYM_R,"1",85,80,"+3V3_RP","power"), at_pin(SYM_R,"2",85,80,"SCL0","global")]
    J5 = conn(5); items.append(instance(J5,"J5","I2C sensors",130,90,ref_dy=-12))
    j5={1:("GND","power"),2:("+3V3_RP","power"),3:("SDA0","global"),4:("SCL0","global"),5:("SENS_INT","global")}
    for pn,(net,kind) in j5.items():
        nets.append(at_pin(J5,str(pn),130,90,net,kind))
    JX = conn(4); items.append(instance(JX,"JX1","STEMMA QT",170,90,ref_dy=-12))
    jx={1:("GND","power"),2:("+3V3_RP","power"),3:("SDA0","global"),4:("SCL0","global")}
    for pn,(net,kind) in jx.items():
        nets.append(at_pin(JX,str(pn),170,90,net,kind))
    return doc, items, nets

def sh_servos():
    items, nets, doc = [], [], [
        "8. Servos — 8 ch, RP2354B PWM (3.3 V signal direct).  J20..J27.",
        "SRV1..8 global from RP2354B. Pin order SIG / +V_SERVO / GND. +V_SERVO fused.",
    ]
    for i in range(8):
        J = conn(3); ref=f"J{20+i}"; x=50+(i%4)*30; y=80+(i//4)*40
        items.append(instance(J,ref,f"SRV{i+1}",x,y,ref_dy=-12))
        nets += [at_pin(J,"1",x,y,f"SRV{i+1}","global"),
                 at_pin(J,"2",x,y,"+V_SERVO","power"), at_pin(J,"3",x,y,"GND","power")]
    items.append(instance(SYM_C,"C60","1000uF",185,90)); nets+=[at_pin(SYM_C,"1",185,90,"+V_SERVO","power"),at_pin(SYM_C,"2",185,90,"GND","power")]
    return doc, items, nets

def sh_buttons():
    items, nets, doc = [], [], [
        "9. Buttons / boop — RP2354B GPIO (MCU debounce).  J6 = BTN1..10.",
        "Wire switches to GND (active_low); internal pull-ups in firmware.",
    ]
    J6 = conn(12); items.append(instance(J6,"J6","buttons",110,95,ref_dy=-18))
    j6={1:("+3V3_RP","power"),2:("GND","power")}
    for i in range(10):
        j6[i+3]=(f"BTN{i+1}","global")
    for pn,(net,kind) in j6.items():
        nets.append(at_pin(J6,str(pn),110,95,net,kind))
    return doc, items, nets

def sh_usb():
    items, nets, doc = [], [], [
        "10. USB 3.1 hub — VL817 (4-port, 5 Gbps / Gen1).  CM5 USB3 port#0 upstream",
        "-> 4x USB-C downstream (J40..J43, SuperSpeed).  RP2354B is OFF-hub (CM5 USB3",
        "port#1 via SW1).  J11 backpack = CM5 USB2 port.  SS TX pairs have AC caps;",
        "USB-C ports are HOST/DFP -> CC Rp pull-ups; per-port VBUS PTC from +5V.",
        "NOTE: one SS lane wired (TX1/RX1) -> USB3 in one C orientation; full flip",
        "needs a per-port 2:1 SS mux (DNP option).  USB2 D+/- works both ways.",
    ]
    hub = SYM_HUB3
    items.append(instance(hub,"U7","VL817-Q7",95,100,ref_dy=-34))
    # Power: 5 V in. VDD33/VDD12 are internal-regulator OUTPUTS -> decouple only
    # (keeps the hub off the RP2354B's local +3V3_RP rail).
    nets += [at_pin(hub,"VDD5",95,100,"+5V","power"), at_pin(hub,"GND",95,100,"GND","power")]
    items.append(instance(SYM_C,"C70","1uF",30,150));  nets+=[at_pin(SYM_C,"1",30,150,"VDD33_HUB","local"),at_pin(SYM_C,"2",30,150,"GND","power")]
    items.append(instance(SYM_C,"C71","1uF",42,150));  nets+=[at_pin(SYM_C,"1",42,150,"VDD12_HUB","local"),at_pin(SYM_C,"2",42,150,"GND","power")]
    items.append(instance(SYM_C,"C72","100n",54,150)); nets+=[at_pin(SYM_C,"1",54,150,"+5V","power"),at_pin(SYM_C,"2",54,150,"GND","power")]
    nets += [at_pin(hub,"VDD33",95,100,"VDD33_HUB","local"), at_pin(hub,"VDD12",95,100,"VDD12_HUB","local")]
    # 25 MHz crystal + load caps
    items.append(instance(SYM_XTAL,"Y2","25MHz",40,62))
    nets += [at_pin(SYM_XTAL,"1",40,62,"HUB_XI","local"), at_pin(SYM_XTAL,"2",40,62,"HUB_XO","local")]
    nets += [at_pin(hub,"XI",95,100,"HUB_XI","local"), at_pin(hub,"XO",95,100,"HUB_XO","local")]
    items.append(instance(SYM_C,"C73","18p",28,74)); nets+=[at_pin(SYM_C,"1",28,74,"HUB_XI","local"),at_pin(SYM_C,"2",28,74,"GND","power")]
    items.append(instance(SYM_C,"C74","18p",52,74)); nets+=[at_pin(SYM_C,"1",52,74,"HUB_XO","local"),at_pin(SYM_C,"2",52,74,"GND","power")]
    # Reset RC + self-power strap + VBUS-detect (host present = carrier powered)
    items.append(instance(SYM_R,"R70","10k",30,40)); nets+=[at_pin(SYM_R,"1",30,40,"VDD33_HUB","local"),at_pin(SYM_R,"2",30,40,"HUB_RST","local")]
    items.append(instance(SYM_C,"C75","100n",42,40)); nets+=[at_pin(SYM_C,"1",42,40,"HUB_RST","local"),at_pin(SYM_C,"2",42,40,"GND","power")]
    items.append(instance(SYM_R,"R71","10k",58,40)); nets+=[at_pin(SYM_R,"1",58,40,"VDD33_HUB","local"),at_pin(SYM_R,"2",58,40,"HUB_SELF","local")]
    nets += [at_pin(hub,"RESETB",95,100,"HUB_RST","local"), at_pin(hub,"SELFPWR",95,100,"HUB_SELF","local"),
             at_pin(hub,"VBUS_DET",95,100,"+5V","power")]
    # --- Upstream: CM5 USB3 port#0 ---
    # USB2 pair direct; SS RX from CM5 (TX caps live at CM5); SS TX to CM5 via caps here.
    nets += [at_pin(hub,"UP_DP",95,100,"CM5_USB_DP","global"), at_pin(hub,"UP_DM",95,100,"CM5_USB_DM","global"),
             at_pin(hub,"UP_SSRXP",95,100,"CM5_SSTX_P","global"), at_pin(hub,"UP_SSRXM",95,100,"CM5_SSTX_M","global")]
    items.append(instance(SYM_C,"C76","100n",150,38)); nets+=[at_pin(SYM_C,"1",150,38,"UP_TX_PI","local"),at_pin(SYM_C,"2",150,38,"CM5_SSRX_P","global")]
    items.append(instance(SYM_C,"C77","100n",162,38)); nets+=[at_pin(SYM_C,"1",162,38,"UP_TX_MI","local"),at_pin(SYM_C,"2",162,38,"CM5_SSRX_M","global")]
    nets += [at_pin(hub,"UP_SSTXP",95,100,"UP_TX_PI","local"), at_pin(hub,"UP_SSTXM",95,100,"UP_TX_MI","local")]
    # --- 4 downstream USB-C ports ---
    for i,(n,ref) in enumerate([("1","J40"),("2","J41"),("3","J42"),("4","J43")]):
        x = 150 + i*32; ytx = 62
        # hub TX -> AC caps (at transmitter) -> connector TX1
        items.append(instance(SYM_C,f"C8{i}a","100n",x,ytx));   nets+=[at_pin(SYM_C,"1",x,ytx,f"P{n}_TXPI","local"),at_pin(SYM_C,"2",x,ytx,f"P{n}_TXP","local")]
        items.append(instance(SYM_C,f"C8{i}b","100n",x+9,ytx)); nets+=[at_pin(SYM_C,"1",x+9,ytx,f"P{n}_TXMI","local"),at_pin(SYM_C,"2",x+9,ytx,f"P{n}_TXM","local")]
        nets += [at_pin(hub,f"SSTXP{n}",95,100,f"P{n}_TXPI","local"), at_pin(hub,f"SSTXM{n}",95,100,f"P{n}_TXMI","local")]
        # hub RX <- connector RX1 (device TX caps are in the cable peer); USB2 D+/-
        nets += [at_pin(hub,f"SSRXP{n}",95,100,f"P{n}_RXP","local"), at_pin(hub,f"SSRXM{n}",95,100,f"P{n}_RXM","local"),
                 at_pin(hub,f"DP{n}",95,100,f"P{n}_DP","local"), at_pin(hub,f"DM{n}",95,100,f"P{n}_DM","local")]
        # USB-C receptacle
        J = SYM_USBC3; jx = x+2; jy = 120
        items.append(instance(J,ref,f"USB-C P{n}",jx,jy,ref_dy=-30))
        nets += [at_pin(J,"A1",jx,jy,"GND","power"),
                 at_pin(J,"A6",jx,jy,f"P{n}_DP","local"), at_pin(J,"A7",jx,jy,f"P{n}_DM","local"),
                 at_pin(J,"A2",jx,jy,f"P{n}_TXP","local"), at_pin(J,"A3",jx,jy,f"P{n}_TXM","local"),
                 at_pin(J,"B11",jx,jy,f"P{n}_RXP","local"), at_pin(J,"B10",jx,jy,f"P{n}_RXM","local")]
        # VBUS via per-port PTC from +5V (self-powered hub)
        items.append(instance(SYM_FUSE,f"F{4+i}","PTC 1A",jx,jy-22))
        nets += [at_pin(SYM_FUSE,"1",jx,jy-22,"+5V","power"), at_pin(SYM_FUSE,"2",jx,jy-22,f"P{n}_VBUS","local"),
                 at_pin(J,"A4",jx,jy,f"P{n}_VBUS","local")]
        # CC Rp pull-ups (DFP, default USB current): 56k to +5V
        items.append(instance(SYM_R,f"R8{i}a","56k",jx-7,jy+24)); nets+=[at_pin(SYM_R,"1",jx-7,jy+24,f"P{n}_VBUS","local"),at_pin(SYM_R,"2",jx-7,jy+24,f"P{n}_CC1","local")]
        items.append(instance(SYM_R,f"R8{i}b","56k",jx+7,jy+24)); nets+=[at_pin(SYM_R,"1",jx+7,jy+24,f"P{n}_VBUS","local"),at_pin(SYM_R,"2",jx+7,jy+24,f"P{n}_CC2","local")]
        nets += [at_pin(J,"A5",jx,jy,f"P{n}_CC1","local"), at_pin(J,"B5",jx,jy,f"P{n}_CC2","local")]
        # USB2 D+/- ESD array (SS ESD = TPD4EUSB30 at layout, see BOM)
        items.append(instance(SYM_ESD,f"U{20+i}","USBLC6-2",jx,jy+38))
        nets += [at_pin(SYM_ESD,"1",jx,jy+38,f"P{n}_DP","local"), at_pin(SYM_ESD,"3",jx,jy+38,f"P{n}_DM","local"),
                 at_pin(SYM_ESD,"2",jx,jy+38,"GND","power"), at_pin(SYM_ESD,"5",jx,jy+38,f"P{n}_VBUS","local")]
    # Backpack uplink — its own CM5 USB2 port
    J11 = conn(4); items.append(instance(J11,"J11","backpack uplink",30,175,ref_dy=-12))
    nets += [at_pin(J11,"1",30,175,"+5V","power"), at_pin(J11,"2",30,175,"CM5_USB2_DM","global"),
             at_pin(J11,"3",30,175,"CM5_USB2_DP","global"), at_pin(J11,"4",30,175,"GND","power")]
    return doc, items, nets

def sh_cameras():
    items, nets, doc = [], [], [
        "11. Cameras + display (CM5).  J7/J8 = 22-pin CSI FFC; J10 = HDMI.",
        "MIPI/HDMI route from CM5 DF40 (sheet 2); verify pin map vs CM5 datasheet.",
        "Length-match differential pairs at layout.",
    ]
    for ref,net,x in [("J7","CSI0",70),("J8","CSI1",110)]:
        J = conn(22); items.append(instance(J,ref,f"CSI {ref}",x,100,ref_dy=-30))
        nets += [at_pin(J,"1",x,100,"GND","power"), at_pin(J,"22",x,100,net,"global")]
    J10 = conn(4); items.append(instance(J10,"J10","HDMI",160,90,ref_dy=-12))
    nets += [at_pin(J10,"1",160,90,"HDMI0","global"), at_pin(J10,"2",160,90,"HDMI1","global"),
             at_pin(J10,"3",160,90,"GND","power"), at_pin(J10,"4",160,90,"+5V","power")]
    return doc, items, nets

BUILDERS = {
    "power": sh_power, "cm5": sh_cm5, "face_buffer": sh_face_buffer,
    "rp2354_io": sh_rp2354_io, "rp2354_face": sh_rp2354_face, "leds": sh_leds,
    "sensors_i2c": sh_sensors, "servos": sh_servos, "gpio_buttons": sh_buttons,
    "usb": sh_usb, "cameras_display": sh_cameras,
}
TITLES = {
    "power":"1. Power","cm5":"2. CM5 (HUB75 + cameras + USB)","face_buffer":"3. Face Buffer (HUB75)",
    "rp2354_io":"4. RP2354B I/O MCU","rp2354_face":"5. MAX7219 Buffer (RP2354B)",
    "leds":"6. WS2812 LEDs (RP2354B)","sensors_i2c":"7. Sensors I2C (RP2354B)",
    "servos":"8. Servos (RP2354B)","gpio_buttons":"9. Buttons / Boop (RP2354B)",
    "usb":"10. USB 3.1 Hub (VL817)","cameras_display":"11. Cameras + Display (CM5)",
}

# ── emit ─────────────────────────────────────────────────────────────────────
CUR_SHEET_UUID = None
def emit(fname):
    global CUR_SHEET_UUID
    CUR_SHEET_UUID = SHEET_UUID[fname]
    doc, items, nets = BUILDERS[fname]()
    doc_text = "\n".join(
        f'  (text "{t}" (at 15 {15 + i*4:.1f} 0) {eff(1.5, "left bottom")} (uuid {u()}))'
        for i, t in enumerate(doc))
    lib_block = "\n".join(LIB)
    out = f"""(kicad_sch
  (version {SCH_VERSION})
  (generator "{GEN}")
  (generator_version "9.0")
  (uuid {CUR_SHEET_UUID})
  (paper "A3")
  (title_block
    (title "ProtoHUD Carrier — {TITLES[fname]}")
    (rev "A")
    (company "ProtoHUD")
  )
  (lib_symbols
{lib_block}
  )
{doc_text}
{chr(10).join(items)}
{chr(10).join(nets)}
  (sheet_instances
    (path "/" (page "1"))
  )
)
"""
    with open(os.path.join(HERE, f"{fname}.kicad_sch"), "w") as fh:
        fh.write(out)
    return len(items), len(nets)

if __name__ == "__main__":
    for f in BUILDERS:
        ni, nn = emit(f)
        print(f"{f:16s}  {ni:3d} symbols  {nn:3d} labels")
