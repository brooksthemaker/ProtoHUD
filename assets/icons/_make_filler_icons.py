#!/usr/bin/env python3
"""Generate filler PNG icons for the ProtoHUD info-panel pipeline.

Pure stdlib (no PIL): a tiny supersampled rasterizer writes RGBA PNGs for every
icon name the HUD looks up. They are PLACEHOLDERS — overwrite any <name>.png in
this folder with your own art (same name) and rebuild; IconCache fits each into
its slot (aspect preserved) and never recolors, so author them already-colored.

Run:  python3 assets/icons/_make_filler_icons.py
"""
import math
import os
import struct
import zlib

BASE = 96          # output resolution (px, square)
SS   = 3           # supersample factor
B    = BASE * SS   # working canvas size
CX   = CY = B / 2.0
S    = B * 0.40    # glyph half-extent (matches the C++ glyph math's `s`)
EDGE = max(2.0, S * 0.055)
EDGE_COL = (12, 18, 26)

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ── Palette ──────────────────────────────────────────────────────────────────
SUN   = (255, 200, 60)
MOON  = (226, 232, 246)
CRATER= (176, 186, 206)
CLOUD = (203, 213, 225)
CLOUDD= (150, 162, 176)
RAIN  = (90, 165, 235)
SNOW  = (236, 245, 255)
BOLT  = (255, 210, 70)
FOG   = (196, 206, 218)
WHITE = (238, 245, 255)
RED   = (235, 80, 70)
FACE  = (250, 238, 234)
TEAL  = (0, 210, 180)
TEALF = (224, 252, 246)
CYAN  = (70, 195, 255)
AMBER = (255, 184, 64)
DARK  = (26, 34, 44)

# ── Raster primitives (hard-edged on the big canvas; AA comes from downscale) ──
def _new():
    return bytearray(B * B * 4)

def _set(buf, x, y, col, a=255):
    xi, yi = int(x), int(y)
    if 0 <= xi < B and 0 <= yi < B:
        i = (yi * B + xi) * 4
        buf[i] = col[0]; buf[i+1] = col[1]; buf[i+2] = col[2]; buf[i+3] = a

def disc(buf, cx, cy, r, col):
    if r <= 0: return
    r2 = r * r
    for y in range(max(0, int(cy-r)), min(B-1, int(cy+r)) + 1):
        dy = y + 0.5 - cy
        for x in range(max(0, int(cx-r)), min(B-1, int(cx+r)) + 1):
            dx = x + 0.5 - cx
            if dx*dx + dy*dy <= r2:
                _set(buf, x, y, col)

def erase_disc(buf, cx, cy, r):
    r2 = r * r
    for y in range(max(0, int(cy-r)), min(B-1, int(cy+r)) + 1):
        dy = y + 0.5 - cy
        for x in range(max(0, int(cx-r)), min(B-1, int(cx+r)) + 1):
            dx = x + 0.5 - cx
            if dx*dx + dy*dy <= r2:
                i = (y * B + x) * 4
                buf[i] = buf[i+1] = buf[i+2] = buf[i+3] = 0

def line(buf, x0, y0, x1, y1, w, col):
    dx, dy = x1 - x0, y1 - y0
    L = math.hypot(dx, dy)
    steps = max(1, int(L))
    r = w * 0.5
    for i in range(steps + 1):
        t = i / steps
        disc(buf, x0 + dx*t, y0 + dy*t, r, col)

def arc(buf, cx, cy, rad, a0, a1, w, col):
    steps = max(2, int(abs(a1 - a0) * rad))
    r = w * 0.5
    for i in range(steps + 1):
        a = a0 + (a1 - a0) * i / steps
        disc(buf, cx + math.cos(a)*rad, cy + math.sin(a)*rad, r, col)

def poly(buf, pts, col):
    ys = [p[1] for p in pts]
    n = len(pts)
    for y in range(max(0, int(min(ys))), min(B-1, int(max(ys))) + 1):
        yc = y + 0.5
        xs = []
        for i in range(n):
            x0, y0 = pts[i]; x1, y1 = pts[(i+1) % n]
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                xs.append(x0 + (yc - y0) / (y1 - y0) * (x1 - x0))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(max(0, int(round(xs[i]))), min(B-1, int(round(xs[i+1]))) + 1):
                _set(buf, x, y, col)

def rrect(buf, x, y, w, h, rad, col):
    rad = min(rad, w/2, h/2)
    poly(buf, [(x+rad, y), (x+w-rad, y), (x+w-rad, y+h), (x+rad, y+h)], col)
    poly(buf, [(x, y+rad), (x+w, y+rad), (x+w, y+h-rad), (x, y+h-rad)], col)
    for cx, cy in [(x+rad, y+rad), (x+w-rad, y+rad), (x+rad, y+h-rad), (x+w-rad, y+h-rad)]:
        disc(buf, cx, cy, rad, col)

def rrect_stroke(buf, x, y, w, h, rad, wd, col):
    rrect(buf, x, y, w, h, rad, col)
    rrect(buf, x+wd, y+wd, w-2*wd, h-2*wd, max(0, rad-wd), (0, 0, 0))
    # re-erase the inner region to transparent
    erase_rrect(buf, x+wd, y+wd, w-2*wd, h-2*wd, max(0, rad-wd))

def erase_rrect(buf, x, y, w, h, rad):
    rad = min(rad, w/2, h/2)
    for yy in range(max(0, int(y)), min(B-1, int(y+h)) + 1):
        for xx in range(max(0, int(x)), min(B-1, int(x+w)) + 1):
            cx = min(max(xx + 0.5, x + rad), x + w - rad)
            cy = min(max(yy + 0.5, y + rad), y + h - rad)
            if (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= rad * rad:
                i = (yy * B + xx) * 4
                buf[i] = buf[i+1] = buf[i+2] = buf[i+3] = 0

# Outlined variants: a slightly larger dark shape first, then the colored one.
def odisc(buf, cx, cy, r, col):
    disc(buf, cx, cy, r + EDGE, EDGE_COL); disc(buf, cx, cy, r, col)

def oline(buf, x0, y0, x1, y1, w, col):
    line(buf, x0, y0, x1, y1, w + 2*EDGE, EDGE_COL); line(buf, x0, y0, x1, y1, w, col)

def opoly(buf, pts, col):
    n = len(pts)
    for i in range(n):
        line(buf, pts[i][0], pts[i][1], pts[(i+1) % n][0], pts[(i+1) % n][1], 2*EDGE, EDGE_COL)
    poly(buf, pts, col)

# ── Shared shapes ────────────────────────────────────────────────────────────
def cloud(buf, ox, oy, scale, col):
    cx, cy = CX + ox, CY + oy
    parts = [(-0.35, 0.0, 0.30), (0.02, -0.20, 0.38), (0.40, 0.0, 0.28)]
    for px, py, pr in parts:                     # dark underlay
        disc(buf, cx + S*px*scale, cy + S*py*scale, S*pr*scale + EDGE, EDGE_COL)
    rrect(buf, cx - S*0.62*scale - EDGE, cy - EDGE, S*1.18*scale + 2*EDGE,
          S*0.36*scale + 2*EDGE, S*0.16*scale, EDGE_COL)
    for px, py, pr in parts:                     # color
        disc(buf, cx + S*px*scale, cy + S*py*scale, S*pr*scale, col)
    rrect(buf, cx - S*0.62*scale, cy, S*1.18*scale, S*0.36*scale, S*0.16*scale, col)

def sun(buf, ox, oy, r):
    cx, cy = CX + ox, CY + oy
    for i in range(8):                           # rays (dark underlay, then bright)
        a = i / 8 * 2 * math.pi
        line(buf, cx+math.cos(a)*r*1.30, cy+math.sin(a)*r*1.30,
             cx+math.cos(a)*r*1.85, cy+math.sin(a)*r*1.85, r*0.34, EDGE_COL)
    for i in range(8):
        a = i / 8 * 2 * math.pi
        line(buf, cx+math.cos(a)*r*1.35, cy+math.sin(a)*r*1.35,
             cx+math.cos(a)*r*1.78, cy+math.sin(a)*r*1.78, r*0.18, SUN)
    odisc(buf, cx, cy, r, SUN)

def moon(buf, ox, oy, r):
    cx, cy = CX + ox, CY + oy
    odisc(buf, cx, cy, r, MOON)
    disc(buf, cx - r*0.30, cy - r*0.20, r*0.16, CRATER)
    disc(buf, cx + r*0.28, cy + r*0.12, r*0.20, CRATER)
    disc(buf, cx + r*0.02, cy + r*0.42, r*0.10, CRATER)

def rain_streaks(buf, col):
    for i in (-1, 0, 1):
        x = CX + i * S * 0.34
        oline(buf, x, CY + S*0.28, x - S*0.12, CY + S*0.72, S*0.10, col)

def dots(buf, col, y=0.55):
    for i in (-1, 0, 1):
        odisc(buf, CX + i * S * 0.34, CY + S*y, S*0.09, col)

def bolt(buf):
    opoly(buf, [(CX+S*0.10, CY+S*0.10), (CX-S*0.20, CY+S*0.30), (CX+S*0.00, CY+S*0.36),
                (CX-S*0.14, CY+S*0.78), (CX+S*0.28, CY+S*0.18), (CX+S*0.06, CY+S*0.14)], BOLT)

# ── Clock-style face (alarm / timer) ─────────────────────────────────────────
def clock_face(buf, ring, face, hand1, hand2):
    odisc(buf, CX, CY + S*0.06, S*0.44, ring)
    disc(buf, CX, CY + S*0.06, S*0.34, face)
    line(buf, CX, CY+S*0.06, CX, CY - S*0.18, S*0.07, DARK)        # hour hand (up)
    line(buf, CX, CY+S*0.06, CX + S*0.20, CY + S*0.10, S*0.05, DARK)  # min hand

# ── Per-icon drawing ─────────────────────────────────────────────────────────
def i_clear(b):        sun(b, 0, 0, S*0.50)
def i_clear_night(b):  moon(b, 0, 0, S*0.48)
def i_partly(b):       sun(b, -S*0.30, -S*0.30, S*0.28); cloud(b, S*0.10, S*0.14, 1.0, CLOUD)
def i_partly_night(b): moon(b, -S*0.30, -S*0.28, S*0.26); cloud(b, S*0.10, S*0.14, 1.0, CLOUD)
def i_cloudy(b):       cloud(b, 0, 0, 1.12, CLOUDD)
def i_fog(b):
    cloud(b, 0, -S*0.18, 1.0, CLOUD)
    for i in range(3):
        y = CY + S*0.36 + i * S*0.22
        oline(b, CX - S*0.55, y, CX + S*0.55, y, S*0.09, FOG)
def i_drizzle(b):      cloud(b, 0, -S*0.15, 1.0, CLOUD); dots(b, RAIN)
def i_rain(b):         cloud(b, 0, -S*0.15, 1.0, CLOUD); rain_streaks(b, RAIN)
def i_snow(b):         cloud(b, 0, -S*0.15, 1.0, CLOUD); dots(b, SNOW)
def i_storm(b):        cloud(b, 0, -S*0.18, 1.0, CLOUDD); bolt(b)

def i_bell(b):
    odisc(b, CX, CY - S*0.50, S*0.09, AMBER)                       # top knob
    odisc(b, CX, CY - S*0.06, S*0.34, AMBER)                       # dome
    opoly(b, [(CX-S*0.34, CY-S*0.06), (CX+S*0.34, CY-S*0.06),
              (CX+S*0.48, CY+S*0.30), (CX-S*0.48, CY+S*0.30)], AMBER)  # flare
    rrect(b, CX-S*0.52, CY+S*0.28, S*1.04, S*0.12, S*0.06, AMBER)  # rim
    odisc(b, CX, CY + S*0.48, S*0.10, AMBER)                       # clapper

def i_alarm(b):
    odisc(b, CX - S*0.34, CY - S*0.40, S*0.16, RED)               # bell ears
    odisc(b, CX + S*0.34, CY - S*0.40, S*0.16, RED)
    oline(b, CX - S*0.30, CY + S*0.40, CX - S*0.42, CY + S*0.56, S*0.08, RED)  # legs
    oline(b, CX + S*0.30, CY + S*0.40, CX + S*0.42, CY + S*0.56, S*0.08, RED)
    clock_face(b, RED, FACE, DARK, DARK)

def i_timer(b):
    rrect(b, CX - S*0.09, CY - S*0.58, S*0.18, S*0.14, S*0.04, TEAL)  # crown button
    oline(b, CX, CY - S*0.46, CX, CY - S*0.34, S*0.06, TEAL)
    clock_face(b, TEAL, TEALF, DARK, DARK)

def i_message(b):
    rrect(b, CX-S*0.46-EDGE, CY-S*0.36-EDGE, S*0.92+2*EDGE, S*0.62+2*EDGE, S*0.16, EDGE_COL)
    poly(b, [(CX-S*0.22, CY+S*0.24), (CX-S*0.02, CY+S*0.24), (CX-S*0.30, CY+S*0.50)], EDGE_COL)
    rrect(b, CX-S*0.46, CY-S*0.36, S*0.92, S*0.62, S*0.16, CYAN)   # bubble
    poly(b, [(CX-S*0.20, CY+S*0.22), (CX-S*0.04, CY+S*0.22), (CX-S*0.26, CY+S*0.46)], CYAN)
    for dx in (-0.20, 0.0, 0.20):                                  # dots
        disc(b, CX + S*dx, CY - S*0.04, S*0.06, DARK)

# ── Status glyphs (white, mirror the C++ procedural set) ──────────────────────
def s_wifi(b):
    bx, by = CX, CY + S*0.30
    for i in (1, 2, 3):
        arc(b, bx, by, S*0.20*i, math.radians(-135), math.radians(-45), S*0.10, WHITE)
    disc(b, bx, by, S*0.07, WHITE)

def s_bt(b):
    w = S*0.30
    pts = [(CX+w, CY+S*0.28), (CX, CY-S*0.50), (CX, CY+S*0.50), (CX+w, CY-S*0.28)]
    for i in range(len(pts)-1):
        line(b, pts[i][0], pts[i][1], pts[i+1][0], pts[i+1][1], S*0.10, WHITE)

def s_gamepad(b):
    rrect_stroke(b, CX-S*0.46, CY-S*0.24, S*0.92, S*0.48, S*0.20, S*0.07, WHITE)
    dx = CX - S*0.22
    line(b, dx-S*0.12, CY, dx+S*0.12, CY, S*0.07, WHITE)
    line(b, dx, CY-S*0.12, dx, CY+S*0.12, S*0.07, WHITE)
    disc(b, CX+S*0.18, CY-S*0.06, S*0.06, WHITE)
    disc(b, CX+S*0.32, CY+S*0.06, S*0.06, WHITE)

def s_audio(b):
    poly(b, [(CX-S*0.40, CY-S*0.12), (CX-S*0.22, CY-S*0.12), (CX-S*0.02, CY-S*0.28),
             (CX-S*0.02, CY+S*0.28), (CX-S*0.22, CY+S*0.12), (CX-S*0.40, CY+S*0.12)], WHITE)
    for i in (1, 2):
        arc(b, CX-S*0.02, CY, S*0.16*i, math.radians(-40), math.radians(40), S*0.07, WHITE)

def s_ssh(b):
    rrect_stroke(b, CX-S*0.42, CY-S*0.34, S*0.84, S*0.68, S*0.10, S*0.06, WHITE)
    line(b, CX-S*0.22, CY-S*0.10, CX-S*0.08, CY+S*0.03, S*0.06, WHITE)  # chevron
    line(b, CX-S*0.08, CY+S*0.03, CX-S*0.22, CY+S*0.16, S*0.06, WHITE)
    line(b, CX+S*0.02, CY+S*0.16, CX+S*0.22, CY+S*0.16, S*0.06, WHITE)  # underscore

def s_lora(b):
    ny = CY - S*0.10
    line(b, CX, ny, CX, CY + S*0.40, S*0.07, WHITE)               # mast
    line(b, CX-S*0.16, CY+S*0.40, CX+S*0.16, CY+S*0.40, S*0.07, WHITE)  # base
    disc(b, CX, ny, S*0.07, WHITE)
    for side in (0, 1):
        a0 = math.radians(-34) if side else math.radians(146)
        a1 = math.radians(34) if side else math.radians(214)
        for i in (1, 2):
            arc(b, CX, ny, S*0.16*i, a0, a1, S*0.06, WHITE)

ICONS = {
    "wx-clear": i_clear, "wx-clear-night": i_clear_night,
    "wx-partly": i_partly, "wx-partly-night": i_partly_night,
    "wx-cloudy": i_cloudy, "wx-fog": i_fog, "wx-drizzle": i_drizzle,
    "wx-rain": i_rain, "wx-snow": i_snow, "wx-storm": i_storm,
    "alarm": i_alarm, "timer": i_timer, "message": i_message, "bell": i_bell,
    "status-wifi": s_wifi, "status-bt": s_bt, "status-gamepad": s_gamepad,
    "status-audio": s_audio, "status-ssh": s_ssh, "status-lora": s_lora,
}

# ── Downscale (premultiplied) + PNG encode ────────────────────────────────────
def downscale(buf):
    n = BASE
    out = bytearray(n * n * 4)
    for oy in range(n):
        for ox in range(n):
            sr = sg = sb = sa = 0
            for j in range(SS):
                row = (oy*SS + j) * B
                for k in range(SS):
                    i = (row + ox*SS + k) * 4
                    a = buf[i+3]
                    sr += buf[i]*a; sg += buf[i+1]*a; sb += buf[i+2]*a; sa += a
            oi = (oy*n + ox) * 4
            out[oi+3] = sa // (SS*SS)
            if sa:
                out[oi] = min(255, sr // sa); out[oi+1] = min(255, sg // sa); out[oi+2] = min(255, sb // sa)
    return out

def write_png(path, n, rgba):
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    raw = bytearray()
    for y in range(n):
        raw.append(0)
        raw += rgba[y*n*4:(y+1)*n*4]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))

def main():
    for name, fn in ICONS.items():
        buf = _new()
        fn(buf)
        write_png(os.path.join(OUT_DIR, name + ".png"), BASE, downscale(buf))
        print("wrote", name + ".png")
    print("done:", len(ICONS), "icons ->", OUT_DIR)

if __name__ == "__main__":
    main()
