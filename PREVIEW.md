# ProtoHUD — Display Preview

Each eye receives a 1920×1080 image. The two eyes are presented side-by-side
on the VITURE Beast (3840×1080 SBS). All HUD elements below are composited on
top of the live camera feed.

---

## Full HUD Layout (single eye, 1920×1080)

```
╔═════════════════════════════════════════════════════════════════════════════╗  ← y=0
║                                                              AU → VITURE   ║  ← top bar (52 px)
║                            [MSG:3]                           X:0  ████░░  ║
╠═════════════════════════════════════════════════════════════════════════════╣  ← y=52
║                          ║                                  ║              ║
║                          ║                                  ║              ║
║   ●  Brt:75%   |  /      ║                                  ║      \  |   ●  LORA ●     ║
║  ●  R0G220B180 | /    ●  Audio      Live camera feed        ● Audio |\    ●  Alpha 045° ║
║ ●  Palette:#2  |/  ●  Interface     (NV12→RGB, zero-copy)  ●  I/F  | \   ●  Bravo 230° ║
║●  FACE ●      /  ●  LoRa                                   ●  LoRa |  \                ║
║              /  ●  Proot                                   ●  Proo |   \                ║
║             /                                                      |    \               ║
║════════════/══════════════════════════════════════════════════\════╝     \══════════════║
║           ║      NE          ENE      E  95°   ESE           ║                          ║
║           ╚══════════════════════════════════════════════════╝                          ║
╚═════════════════════════════════════════════════════════════════════════════════════════╝
```

Left-to-right at bottom baseline (x values for 1920-wide single eye):
- x≈120 — outer extension of FACE arm
- x≈260 — FACE arm diagonal anchor
- x≈410 — left separator diagonal (master divider)
- x≈560 — left health indicators diagonal
- compass tape (x≈640–1280)
- x≈1360 — right health indicators diagonal
- x≈1510 — right separator diagonal (master divider)
- x≈1660 — LoRa arm diagonal anchor
- x≈1800 — outer extension of LoRa arm

**●** = subsystem OK (orange dot + glow)  **○** = fault (red dot, label dimmed)

---

## Health Indicator Column (left edge, zoomed)

```
  x=10                   x=118
   │                        │
   ▼                        ▼
┌──────────────────────────┐
│  ●  Proot                │  ← Teensy face-LED controller
│  ●  LoRa                 │  ← LoRa mesh radio
│  ●  Interface            │  ← SmartKnob haptic controller
│  ●  Left Cam             │  ← OWLsight CSI left camera
│  ●  Right Cam            │  ← OWLsight CSI right camera
│  ○  Cam 1                │  ← USB camera 1 (PiP source) — FAULTED
│  ●  Cam 2                │  ← USB camera 2 (PiP source)
│  ●  Audio                │  ← ALSA audio engine
│  ●  Android              │  ← Android screen mirror
└──────────────────────────┘
 semi-transparent bg panel
 opacity: hud.health_panel_opacity
 (default 0.71)
```

Row layout (20 px per row):
```
 ←6px→ ●  ← 4px dot radius
        ←4px gap→ Label text (14 pt DejaVu Sans Mono)
```

Color key:

| State | Dot | Label text |
|-------|-----|------------|
| OK | teal `(0, 220, 180)` | bright `(200, 240, 230)` |
| Fault | red `(255, 60, 60)` | dim `(120, 160, 150)` |

---

## Top Bar (full width, 52 px)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         [MSG:3]                 AU → VITURE  X:0  ████░░░ │
└─────────────────────────────────────────────────────────────────────────────┘
```

- **Left**: empty (health column is now below the top bar on the left side)
- **Center**: unread LoRa message count badge (hidden when zero)
- **Right**: audio strip — output device, xrun count, CPU load bar

---

## LoRa Messages Panel (left side, shown only when messages exist)

The messages panel slides over the same left column as the health indicators.
Health dots are drawn on top of it (rendered after the messages panel) and
remain visible through their own background rect.

```
┌───────────────────────────────────┐
│ MESSAGES                          │ ─── right edge at min(panel_width, w/3)
│                                   │
│ [Alpha  12:34]                    │
│  Hello from sector 4              │
│                                   │
│ [Bravo  12:31]                    │
│  Copy, en route                   │
│                                   │
└───────────────────────────────────┘
```

---

## Indicator Arms (bottom edge, both sides)

The right-side panels have been replaced by indicator-style diagonal arms that
mirror the health indicator design. Three parallel diagonal lines at 130° appear
on each side of the compass tape.

### Left side — FACE arm + separator + health indicators

```
   ●  Brt: 75%     ╱    ╱            ●  Audio
  ●  R0 G220 B180 ╱    ╱           ●  Interface
 ●  Palette: #2  ╱    ╱           ●  LoRa
●  FACE ●       ╱    ╱           ●  Proot
               ╱    ╱
──────────────────────────────────────────────
x≈120  x≈260  x≈410           x≈560
[ext]  [FACE] [sep]            [health anchor]
```

- **FACE** dot: orange = connected, red = offline
- Items: FACE status · effect · GIF/palette · R G B · brightness
- Labels drawn to the LEFT of each dot

### Right side — health indicators + separator + LoRa arm

```
      ●  Audio  ╲    ╲          ●  LORA ●
   ●  Interface  ╲    ╲        ●  Alpha  045° 1.2k
      ●  L.Cam    ╲    ╲      ●  Bravo  230° 3.4k
      ●  R.Cam     ╲    ╲
                    ╲    ╲
──────────────────────────────────────────────────
       x≈1360       x≈1510  x≈1660      x≈1800
      [health]      [sep]  [LoRa]       [ext]
```

- **LORA** dot: ok/fail from `health.lora_ok`
- Node rows (up to 3): name (6 chars) + heading° + distance in km
- Node dot: orange if age < 120 s, red if stale or unseen
- Labels drawn to the RIGHT of each dot

---

## Compass Tape (center third, 60 px, floats above bottom edge)

Spans the middle 1/3 of screen width, horizontally centered. Shows a
120° window (60° either side of current heading). Each degree spans
`tape_width / 120` pixels. Vertical position is `compass_bottom_margin_px`
above the screen edge (default 20 px, set in `config.json → hud`).

```
      NE          ENE              E            ESE          SE
       |    80     |    90    95°  |    100      |    110     |
  ░░░░░|░░░░|░░░░░░|░░░░|░░░░░▲░░░|░░░░|░░░░░░░░|░░░░|░░░░░░|
                               ▲
                              95°
```

Current heading shown with a center triangle cursor and degree readout below.
Cardinal labels (N, NE, E, SE, S, SW, W, NW) appear when they fall within the
120° window. Major ticks every 10°, minor ticks every 5°.
