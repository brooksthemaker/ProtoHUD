#!/usr/bin/env python3
"""Pick-and-place CSV -> led_panels map_file JSON.

Turns a PCB pick-and-place (centroid) export into the per-LED sampling map
the `led_panels` face backend loads (`{"leds": [[x, y], ...]}`, one canvas
pixel per LED, in CHAIN ORDER). The chain order comes from the LED reference
designators: LED1 is the first LED on the data line, LED2 the second, and so
on — number your schematic designators along the actual wiring and the map
is correct by construction.

Works with the common centroid formats (KiCad .pos exported as CSV, Altium
Pick Place, JLC/EasyEDA): it looks for designator / mid-x / mid-y / layer
columns by name, ignores units suffixes, and skips non-LED rows.

Coordinates: PnP files are millimetres with Y increasing UP (board space);
the face canvas is pixels with Y increasing DOWN, so Y is flipped by default
(--no-flip-y if your export already is screen-oriented). A bottom-layer
assembly — the usual way to build the mirrored second panel from the same
board design — is auto-mirrored in X so the map matches what you see looking
AT the LEDs; override with --mirror-x/--no-mirror-x. Always eyeball the
--preview SVG before trusting orientation.

Scaling (pick one):
  --px-per-mm F     explicit scale
  --pitch-px N      N canvas pixels per LED pitch (pitch auto-detected from
                    nearest-neighbour spacing) — the natural choice: 1 gives
                    the most compact map, 2+ gives sub-LED sampling headroom
  --fit W H         uniform-scale the panel into a WxH pixel box

Placement: --origin X Y offsets the finished map on the canvas (default 0 0),
e.g. to put the right panel on the right half of a shared canvas.

Examples:
    # Left/top panel at 2 px per LED, placed at canvas origin
    pnp_to_ledmap.py PickPlace_Top.csv -o config/panels/face_l.json \
        --pitch-px 2 --preview face_l.svg

    # Mirrored bottom-assembly twin on the right half of a 160-wide canvas
    pnp_to_ledmap.py PickPlace_Bottom.csv -o config/panels/face_r.json \
        --pitch-px 2 --origin 80 0 --preview face_r.svg
"""
import argparse
import csv
import json
import math
import os
import re
import sys

# Column-name candidates, lowercased, punctuation stripped (order = priority).
DESIGNATOR_COLS = ("designator", "ref", "refdes", "reference", "component")
X_COLS = ("midx", "centerx", "centerxmm", "posx", "refx", "x", "xmm")
Y_COLS = ("midy", "centery", "centerymm", "posy", "refy", "y", "ymm")
LAYER_COLS = ("layer", "side", "tb")


def norm_header(h):
    return re.sub(r"[^a-z]", "", h.lower())


def pick_col(headers, candidates, what):
    normed = {norm_header(h): h for h in headers}
    for c in candidates:
        if c in normed:
            return normed[c]
    sys.exit(f"error: no {what} column found (headers: {headers})")


def parse_mm(s):
    """'129.6388', '129.64mm', '-1.5 mm' -> float millimetres."""
    m = re.search(r"-?\d+(?:\.\d+)?", s)
    if not m:
        raise ValueError(f"no number in {s!r}")
    return float(m.group(0))


def load_leds(path, led_regex):
    """-> (leds sorted by designator number [(n, x_mm, y_mm)], layer counts)."""
    pat = re.compile(led_regex, re.IGNORECASE)
    with open(path, newline="") as f:
        # Some exports lead with comment/title lines before the header row.
        lines = [ln for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    header_i = next(
        (i for i, ln in enumerate(lines)
         if any(norm_header(c) in DESIGNATOR_COLS
                for c in next(csv.reader([ln])))), None)
    if header_i is None:
        sys.exit(f"error: {path}: no header row with a designator column")
    rows = list(csv.DictReader(lines[header_i:]))
    headers = rows[0].keys() if rows else []
    dcol = pick_col(headers, DESIGNATOR_COLS, "designator")
    xcol = pick_col(headers, X_COLS, "X")
    ycol = pick_col(headers, Y_COLS, "Y")
    lcol = next((h for h in headers if norm_header(h) in LAYER_COLS), None)

    leds, layers, dups = [], {}, []
    seen = set()
    for r in rows:
        m = pat.fullmatch((r.get(dcol) or "").strip())
        if not m:
            continue
        n = int(m.group(1))
        if n in seen:
            dups.append(n)
            continue
        seen.add(n)
        leds.append((n, parse_mm(r[xcol]), parse_mm(r[ycol])))
        if lcol:
            lay = (r.get(lcol) or "").strip().lower()
            layers[lay] = layers.get(lay, 0) + 1
    leds.sort()
    if dups:
        print(f"warning: duplicate designator numbers ignored: {sorted(set(dups))[:10]}",
              file=sys.stderr)
    nums = [n for n, _, _ in leds]
    gaps = [n for a, b in zip(nums, nums[1:]) for n in range(a + 1, b)]
    if gaps:
        print(f"warning: designator gaps (chain order still = numeric order): "
              f"{gaps[:10]}{'...' if len(gaps) > 10 else ''}", file=sys.stderr)
    return leds, layers


def detect_pitch(pts):
    """Median nearest-neighbour distance in mm — the LED grid pitch."""
    if len(pts) < 2:
        return 1.0
    # Grid-bucket for O(n) neighbour search; panels are a few hundred LEDs so
    # even brute force would be fine, but stay snappy for 2000-LED panels.
    cell = {}
    guess = max(1e-3, (max(x for x, _ in pts) - min(x for x, _ in pts))
                / max(1, int(math.sqrt(len(pts)))))
    for i, (x, y) in enumerate(pts):
        cell.setdefault((int(x / guess), int(y / guess)), []).append(i)
    dists = []
    for i, (x, y) in enumerate(pts):
        cx, cy = int(x / guess), int(y / guess)
        best = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in cell.get((cx + dx, cy + dy), ()):
                    if j == i:
                        continue
                    d = math.hypot(pts[j][0] - x, pts[j][1] - y)
                    if best is None or d < best:
                        best = d
        if best is not None:
            dists.append(best)
    dists.sort()
    return dists[len(dists) // 2] if dists else 1.0


def write_svg(path, pts, w, h):
    """Chain-order preview: hue-gradient dots, wiring polyline, IN/OUT marks."""
    s = max(1.0, min(1200.0 / max(w, 1), 800.0 / max(h, 1)))
    r = max(1.5, s * 0.35)
    n = len(pts)

    def color(i):
        t = i / max(1, n - 1)
        # blue (start) -> red (end) through green, cheap HSV walk
        h6 = (1 - t) * 4.0  # 4=blue .. 0=red
        k = int(h6)
        f = h6 - k
        rgb = [(1, f, 0), (1 - f, 1, 0), (0, 1, f), (0, 1 - f, 1), (f, 0, 1)][min(k, 4)]
        return "#%02x%02x%02x" % tuple(int(c * 255) for c in rgb)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" '
           f'width="{w * s:.0f}" height="{h * s:.0f}" '
           f'viewBox="-0.5 -0.5 {w + 1} {h + 1}" style="background:#111">',
           '<rect x="-0.5" y="-0.5" width="{}" height="{}" fill="#111"/>'.format(w + 1, h + 1)]
    poly = " ".join(f"{x},{y}" for x, y in pts)
    out.append(f'<polyline points="{poly}" fill="none" stroke="#444" '
               f'stroke-width="{0.15}"/>')
    for i, (x, y) in enumerate(pts):
        out.append(f'<circle cx="{x}" cy="{y}" r="{r / s:.3f}" fill="{color(i)}"/>')
    if pts:
        for (x, y), c, label in ((pts[0], "#00ff00", "IN"), (pts[-1], "#ff0000", "OUT")):
            out.append(f'<circle cx="{x}" cy="{y}" r="{1.6 * r / s:.3f}" '
                       f'fill="none" stroke="{c}" stroke-width="0.2"/>')
            out.append(f'<text x="{x + 1}" y="{y - 1}" font-size="2" '
                       f'fill="{c}" font-family="monospace">{label}</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="pick-and-place / centroid CSV")
    ap.add_argument("-o", "--out", required=True, help="output map JSON")
    ap.add_argument("--name", default=None, help="panel name stored in the JSON "
                    "(default: output filename stem)")
    ap.add_argument("--led-regex", default=r"LED(\d+)",
                    help="designator pattern; group 1 = chain index (default LED(\\d+))")
    ap.add_argument("--reverse", action="store_true",
                    help="designators are numbered AGAINST the wiring: data "
                         "enters at the highest number and exits at LED1")
    scale = ap.add_mutually_exclusive_group()
    scale.add_argument("--px-per-mm", type=float, help="explicit scale")
    scale.add_argument("--pitch-px", type=float,
                       help="pixels per detected LED pitch (compact map: 1)")
    scale.add_argument("--fit", nargs=2, type=int, metavar=("W", "H"),
                       help="uniform-fit the panel into a WxH pixel box")
    ap.add_argument("--origin", nargs=2, type=int, default=(0, 0), metavar=("X", "Y"),
                    help="canvas offset for the finished map (default 0 0)")
    mir = ap.add_mutually_exclusive_group()
    mir.add_argument("--mirror-x", action="store_true", default=None,
                     help="mirror horizontally (default: auto — on for a "
                          "bottom-layer assembly)")
    mir.add_argument("--no-mirror-x", dest="mirror_x", action="store_false")
    ap.add_argument("--mirror-y", action="store_true",
                    help="additionally mirror vertically")
    ap.add_argument("--no-flip-y", dest="flip_y", action="store_false", default=True,
                    help="skip the PnP-Y-up -> canvas-Y-down flip")
    ap.add_argument("--preview", metavar="SVG",
                    help="write a chain-order preview SVG (look at this!)")
    args = ap.parse_args()

    leds, layers = load_leds(args.csv, args.led_regex)
    if not leds:
        sys.exit(f"error: no designators matching {args.led_regex!r} in {args.csv}")
    if args.reverse:
        leds.reverse()
    pts = [(x, y) for _, x, y in leds]
    pitch = detect_pitch(pts)

    x0, x1 = min(x for x, _ in pts), max(x for x, _ in pts)
    y0, y1 = min(y for _, y in pts), max(y for _, y in pts)
    span_x, span_y = max(x1 - x0, 1e-9), max(y1 - y0, 1e-9)

    if args.px_per_mm:
        ppm = args.px_per_mm
    elif args.pitch_px:
        ppm = args.pitch_px / pitch
    elif args.fit:
        ppm = min((args.fit[0] - 1) / span_x, (args.fit[1] - 1) / span_y)
    else:
        ppm = 1.0 / pitch  # default: compact, 1 px per LED pitch

    mirror_x = args.mirror_x
    if mirror_x is None:
        bottom = sum(c for l, c in layers.items() if l.startswith("b"))
        mirror_x = bottom > len(leds) / 2
        if mirror_x:
            print("note: bottom-layer assembly -> mirroring X "
                  "(--no-mirror-x to keep board coords)", file=sys.stderr)

    ox, oy = args.origin
    mapped = []
    for _, x, y in leds:
        u = (x1 - x) if mirror_x else (x - x0)
        v = (y - y0)
        if args.flip_y:
            v = span_y - v
        if args.mirror_y:
            v = span_y - v
        mapped.append([ox + round(u * ppm), oy + round(v * ppm)])

    w = max(p[0] for p in mapped) + 1
    h = max(p[1] for p in mapped) + 1
    collisions = len(mapped) - len({tuple(p) for p in mapped})
    if collisions:
        print(f"warning: {collisions} LEDs share a pixel with another LED "
              f"(increase --pitch-px / --px-per-mm for distinct samples)",
              file=sys.stderr)

    name = args.name or os.path.splitext(os.path.basename(args.out))[0]
    doc = {
        "name": name,
        "source": os.path.basename(args.csv),
        "count": len(mapped),
        "pitch_mm": round(pitch, 4),
        "px_per_mm": round(ppm, 6),
        "bounds_mm": [round(span_x, 3), round(span_y, 3)],
        "mirrored_x": mirror_x,
        "reversed": args.reverse,
        "canvas_extent": [w, h],
        "leds": mapped,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        # One LED per line keeps diffs reviewable without ballooning the file.
        f.write("{\n")
        for k, v in doc.items():
            if k != "leds":
                f.write(f' "{k}": {json.dumps(v)},\n')
        f.write(' "leds": [\n')
        f.write(",\n".join(f"  [{x}, {y}]" for x, y in mapped))
        f.write("\n ]\n}\n")

    if args.preview:
        write_svg(args.preview, [(p[0] - ox, p[1] - oy) for p in mapped], w - ox, h - oy)

    print(f"{name}: {len(mapped)} LEDs, pitch {pitch:.3f} mm, "
          f"{span_x:.1f}x{span_y:.1f} mm -> extent {w}x{h} px at origin "
          f"({ox},{oy}){' [X-mirrored]' if mirror_x else ''}")
    print(f"  map: {args.out}" + (f"   preview: {args.preview}" if args.preview else ""))
    print(f"  config: {{ \"map_file\": \"{args.out}\" }} — make sure "
          f"protoface canvas_w/h covers {w}x{h}")


if __name__ == "__main__":
    main()
