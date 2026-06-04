# Running multiple face backends at once (HUB75 + MAX7219 + custom)

The base design treats the face display as **"pick one"** (HUB75 *or* MAX7219 *or*
WS2812 `rgb_matrix`) because that's what the firmware does today. Running two or
three **simultaneously** — e.g. HUB75 main face + small MAX7219 brow/cheek
matrices + a custom WS2812 panel — takes changes at **two layers**. The
firmware one is the gating dependency: the wiring alone won't do it.

## Layer 1 — firmware (gating blocker)

`pf_build_panel_output()` (`src/main.cpp`) builds a **single** `PanelOutput` from
one `protoface.backend` string, and `NativeFaceController` holds a single
`output_`. There is **no composite output** — so today only one backend lights
at a time.

To run several at once you need a **`CompositePanelOutput`**: one object that
holds N child `PanelOutput`s, each with the canvas sub-rectangle it owns, and in
`show(canvas)` forwards (the relevant crop of) the frame to each child;
`covered_regions()` returns the union. Config would list backends instead of one
string, e.g.:

```jsonc
"protoface": {
  "backends": [
    { "type": "hub75",      "canvas_region": [0,   0, 128, 64] },
    { "type": "max7219",    "canvas_region": [0,  64,  64, 16], "chains": [ ... ] },
    { "type": "rgb_matrix", "canvas_region": [64, 64,  64, 16], "chains": [ ... ] }
  ]
}
```

Each backend already maps its chains to canvas coordinates (`canvas_x/canvas_y`,
`module_positions`, `covered_regions()`), so the pieces tile one logical face.
*(Ask and I can implement `CompositePanelOutput` + the `backends` config.)*

## Layer 2 — wiring / bus budget (the hard constraint)

HUB75 (piomatter on RP1 PIO) claims **14 GPIO**, which dominates the budget:

| Claimed by | BCM pins |
|------------|----------|
| HUB75 | 4 (OE), 5 (R1), 6 (B1), 12 (R2), 13 (G1), 16 (G2), 17 (CLK), 20 (D), 21 (STB), 22 (A), 23 (B2), 24 (E), 26 (B), 27 (C) |
| I²C bus 1 (sensors) | 2 (SDA), 3 (SCL) |
| **Free** | **7, 8, 9, 10, 11, 14, 15, 18, 19, 25** |

Two facts fall out of that free list:

- ✅ **SPI0 is completely free** with HUB75 active — CE1 = 7, CE0 = 8, MISO = 9,
  **MOSI = 10**, **SCLK = 11**.
- ❌ **SPI1 is unusable** — its MOSI/SCLK (BCM 20/21) *are* HUB75 `D`/`STB`. So
  the `rgb_matrix` default of putting the right chain on `spidev1.0` **must be
  overridden** to a single `spidev0` chain (or bit-bang) in a HUB75 build.

There is therefore exactly **one hardware SPI bus** available next to HUB75, and
WS2812 timing is strict, so it wins SPI0 when present:

| Want, alongside HUB75 | Second/third panel goes on | Pins | 5 V buffer |
|-----------------------|----------------------------|------|------------|
| HUB75 + **MAX7219** | **hardware SPI0** | DIN=10, CLK=11, CS=8 (CE0), 7 (CE1), +25… | 74AHCT245 |
| HUB75 + **WS2812 custom** | **hardware SPI0 MOSI** (timing-critical) | DIN=10 | 74AHCT1G125 |
| HUB75 + **MAX7219 + WS2812** | WS2812 → SPI0 (10); **MAX7219 → bit-bang GPIO** | DIN/CLK e.g. 14/15, CS e.g. 8/7/25 | 74AHCT245 |

The MAX7219 driver already supports a **bit-banged GPIO transport**
(`Max7219GpioBus`: shared DIN+CLK on any two GPIO, one CS per chain) — the code
comment calls out exactly this case ("available when HUB75 owns SPI1 and WS2812
owns SPI0"). It's slower than hardware SPI but fine for small accent matrices.

> ⚠️ Spare pins are scarce: GPIO buttons/boop also draw from `{14,15,18,19,25}`.
> Budget them together. If you run out, **offload the second panel to a
> co-processor** (the RP2350 helmet board or a Pico) over USB/SPI and sidestep
> the Pi pin crunch entirely — the clean way to scale past two panels.

## How the carrier wiring changes vs. "pick one"

In the base [block diagram](BLOCK-DIAGRAM.md), `JP1` routes the **shared**
74AHCT245 to *either* J2 (HUB75) *or* J3 (MAX7219). For simultaneous operation
you instead **populate a dedicated buffer + connector per active panel**, each on
its own pin group:

- **HUB75** → buffer U1/U2 (74AHCT245) → **J2**, on the 14 PIO pins. *(unchanged)*
- **MAX7219** → its own 74AHCT245 → **J3** (DIN/CLK/CS×n), **sourced from SPI0
  *or* bit-bang GPIO** — `JP1` is repurposed as that *source* selector, not a
  "which panel" selector.
- **WS2812 custom panel** → 74AHCT125 → **J4**, on SPI0 MOSI (BCM 10).
- **Common ground**; each panel on its **own fused 5 V feed**, current-budgeted
  separately (HUB75 high-current rail; MAX7219 ≈ 80 mA/module; WS2812 ≈
  20 mA/LED). See [`REQUIREMENTS.md`](REQUIREMENTS.md) R2.3–R2.4.

### Net: what to change in the BOM/requirements for a multi-backend board
- Don't share one face buffer behind JP1 — fit **separate '245 buffers** for
  HUB75 and MAX7219 (BOM items 12 + a second '245), each to its own connector.
- Bring the MAX7219 DIN/CLK to **both** a SPI0 tap and a spare-GPIO tap with
  0 Ω/jumper select, so it can run hardware-SPI (no WS2812) or bit-bang
  (with WS2812).
- Confirm `{14,15,18,19,25}` aren't all consumed by buttons before committing
  the MAX7219 bit-bang pins.
