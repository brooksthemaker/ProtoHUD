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
║ ● Proot      ║                                         ║ FACE              ║
║ ● LoRa       ║                                         ║ Connected         ║
║ ● Interface  ║                                         ║ Effect: Idle      ║
║ ● Left Cam   ║                                         ║ Palette: #2       ║
║ ● Right Cam  ║         Live camera feed                ║  ██████░░  R0 G… ║
║ ○ Cam 1      ║         (NV12 → RGB, zero-copy)         ║ Brt: 75%          ║
║ ● Cam 2      ║                                         ╠═══════════════════╣
║ ● Audio      ║                                         ║ LORA NODES        ║
║ ● Android    ║                                         ║ Alpha      045° … ║
║              ║                                         ║   RSSI:-87 SNR:6  ║
╠══════════════╩═════════════════════════════════════════╩═══════════════════╣  ← y=1020
║░░░░░░░░░ N ░░░░░░░ NE ░░░░░░░ E ░░░░░░░ SE ░░░░░░░ S ░░░░░░░░░░░░░░░░░░ ║  ← compass (60 px)
╚═════════════════════════════════════════════════════════════════════════════╝  ← y=1080
```

**●** = subsystem OK (teal dot)  **○** = subsystem fault (red dot, label dimmed)

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

## Right Panels (320 px wide, right edge)

### Face Panel (upper half)

```
┌─────────────────────┐
│ FACE                │
│ Connected           │
│ Effect: Idle        │
│ Palette: #2         │
│ ████░░  R0 G220 B18 │
│ Brt: 75%            │
└─────────────────────┘
```

### LoRa Nodes Panel (lower half)

```
┌─────────────────────┐
│ LORA NODES          │
│ Alpha    045° 1.2km │
│   RSSI:-87 SNR:6 8s │
│ Bravo    230° 3.4km │
│   RSSI:-92 SNR:3 47s│
└─────────────────────┘
```

Node color by age: teal < 30 s · cyan 30–120 s · dim > 120 s

---

## Compass Tape (full width, 60 px, bottom)

Shows a 120° window (60° either side of current heading). Each degree
spans `screen_width / 120` pixels so the visible range scales with
display width.

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
