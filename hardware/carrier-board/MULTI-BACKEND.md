# Running multiple face backends at once (HUB75 + MAX7219 + custom)

The base design treats the face display as **"pick one"** (HUB75 *or* MAX7219 *or*
WS2812 `rgb_matrix`) because that's what the firmware does today. Running two or
three **simultaneously** — e.g. HUB75 main face + small MAX7219 brow/cheek
matrices + a custom WS2812 panel — is now a **firmware-only** problem.

The **two-brain carrier removes the old wiring constraint**: HUB75 lives on the
CM5, while MAX7219 and WS2812 live on the **separate RP2354B I/O coprocessor**
(see [`RP2354-IO.md`](RP2354-IO.md), [`PINMAP.md`](PINMAP.md),
[`BLOCK-DIAGRAM.md`](BLOCK-DIAGRAM.md)). They no longer share GPIO or SPI, so the
hardware can light all of them at once. What's left is the firmware: it still
builds a single output from a single backend string, and **that** is now the
gating dependency.

## Layer 1 — firmware (gating blocker)

`pf_build_panel_output()` (`src/main.cpp`) builds a **single** `PanelOutput` from
one `protoface.backend` string, and `NativeFaceController` holds a single
`output_`. There is **no composite output** — so today only one backend lights
at a time, even though the two-brain hardware could drive them together.

To run several at once you need a **`CompositePanelOutput`**: one object that
holds N child `PanelOutput`s, each with the canvas sub-rectangle it owns, and in
`show(canvas)` forwards (the relevant crop of) the frame to each child;
`covered_regions()` returns the union. Config would list backends instead of one
string, e.g.:

```jsonc
"protoface": {
  "backends": [
    // HUB75 main face — driven DIRECTLY by the CM5 (piomatter on RP1).
    { "type": "hub75",      "brain": "cm5",     "canvas_region": [0,   0, 128, 64] },
    // MAX7219 brow/cheek — driven by the RP2354B over USB-CDC (SPI0 on the MCU).
    { "type": "max7219",    "brain": "rp2354b", "transport": "usb-cdc",
      "canvas_region": [0,  64,  64, 16], "chains": [ ... ] },
    // WS2812 accent panel — also on the RP2354B over USB-CDC (PIO zones).
    { "type": "rgb_matrix", "brain": "rp2354b", "transport": "usb-cdc",
      "canvas_region": [64, 64,  64, 16], "chains": [ ... ] }
  ]
}
```

Each backend already maps its chains to canvas coordinates (`canvas_x/canvas_y`,
`module_positions`, `covered_regions()`), so the pieces tile one logical face.

### Two kinds of child output: direct vs. coprocessor-forwarded

A subtlety the composite must handle: the children don't all reach their panels
the same way.

- **Direct (HUB75):** the CM5 drives the panel itself through piomatter on the
  RP1 PIO — `show()` pushes pixels straight to the framebuffer/GPIO.
- **Coprocessor-forwarded (MAX7219, WS2812):** these backends live on the
  **RP2354B**. The CM5 doesn't touch their GPIO at all — it sends the cropped
  pixel/region data to the MCU over **USB-CDC** (`/dev/ttyACM*`, the v1 line
  protocol's `CM5 → MCU` direction: MAX7219 pixel data, LED frames/effects), and
  the RP2354B firmware clocks it out on SPI0 / PIO.

So `CompositePanelOutput` mixes a direct child (HUB75) with one or more
USB-CDC-forwarded children (MAX7219, WS2812). The crop-and-forward contract in
`show()` is identical; only the per-child transport differs.

*(Ask and I can implement `CompositePanelOutput` + the `backends` config.)*

## Layer 2 — wiring (no longer the constraint)

On the two-brain board the panels sit on **different brains and different
buses**, so there is no pin-budget puzzle to solve — each backend already has its
own pins and its own 5 V buffer to its own connector:

| Backend | Brain | Pins | Bus | Buffer → connector |
|---------|-------|------|-----|--------------------|
| **HUB75** | CM5 (RP1) | 14 GPIO (BCM 4–27) | RP1 PIO | U1/U2 74AHCT245 → **J2** |
| **MAX7219** | RP2354B | SCK GP2, DIN GP3, CS GP7–GP10 | SPI0 | U10 74AHCT245 → **J3** |
| **WS2812** (×4 zones) | RP2354B | `LED1..4_DAT` GP16–GP19 | PIO | U11 74AHCT125 → **J4** |

Because HUB75 is the **only** consumer of CM5 GPIO and everything else lives on
the RP2354B (48 GPIO + PIO + 24 PWM, ~30 used), the three backends can run
**simultaneously** with no contention — HUB75 directly on the CM5, MAX7219 and
WS2812 on the MCU and forwarded over USB-CDC. The "second SPI bus", "bit-bang the
MAX7219", and "spare-pin budget" gymnastics are gone; the buffers and connectors
are already separate per backend (see [`BLOCK-DIAGRAM.md`](BLOCK-DIAGRAM.md) and
[`PINMAP.md`](PINMAP.md)). Current budgeting still applies — each panel on its
own fused 5 V feed (HUB75 high-current rail; MAX7219 ≈ 80 mA/module; WS2812 ≈
20 mA/LED); see [`POWER.md`](POWER.md) and [`REQUIREMENTS.md`](REQUIREMENTS.md)
R2.3–R2.4.

### Historical: the old single-brain HUB75 pin crunch

Before the pivot, **all three backends competed for the same CM5 GPIO/SPI pins**,
and the wiring — not the firmware — was the hard wall. HUB75 (piomatter) claimed
**14 GPIO**, leaving only `{7, 8, 9, 10, 11, 14, 15, 18, 19, 25}` free; SPI1's
MOSI/SCLK (BCM 20/21) collided with HUB75 `D`/`STB` so **SPI1 was unusable**,
leaving exactly one hardware SPI bus (SPI0) to share between MAX7219 and the
timing-critical WS2812. Lighting MAX7219 + WS2812 alongside HUB75 forced WS2812
onto SPI0 and the MAX7219 onto a **bit-banged GPIO transport** (`Max7219GpioBus`),
with buttons/boop also fighting for `{14,15,18,19,25}`. The clean escape — even
back then — was to **offload the extra panels to a coprocessor over USB**, which
is precisely what the RP2354B brain now does by design. None of that pin
arithmetic constrains the current board.
