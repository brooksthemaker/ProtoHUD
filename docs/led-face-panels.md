# Custom LED face panels (APA102/SK9822, `backend: "led_panels"`)

Large custom-shaped face panels built from clocked addressable LEDs, driven
straight from the CM5's hardware SPI. Scale target: **up to ~2000 LEDs per
panel, 4 panels (8000 LEDs)** at 30–60+ fps.

## Why APA102/SK9822 (and not WS2812) at this scale

| | WS2812/NeoPixel | APA102/SK9822 (DotStar) |
|---|---|---|
| Wires | 1 (data) | 2 (data + clock) |
| Data rate | fixed 800 kbit/s | clocked — 8–24 MHz from spidev |
| 2000-LED refresh | **60 ms → 16 fps ceiling** | ~5 ms @ 12 MHz → 185 fps ceiling |
| Timing | strict (needs PIO/kernel) | none — plain userspace spidev |
| Extra | cheapest | ~3–4× the cost; 5-bit per-LED global brightness |

WS2812 stays supported for small 8×8-module builds (`backend: "rgb_matrix"`)
and the coprocessor accessory zone; the face at panel scale is APA102.

## Architecture

The renderer composites the SAME canvas as every backend — expressions,
water, frost, reactions, the face editor all work unchanged. `led_panels`
samples that canvas **per LED** through a mapping and streams APA102 frames:

```
canvas (RGB Mat) ──sample map──▶ APA102 wire frames ──spidev──▶ panels
```

- One chain = one panel = one spidev (APA102 has no CS — a bus drives one
  chain; chain two panels in series to share a bus: 4000 LEDs ≈ 11 ms ≈
  90 fps @ 12 MHz).
- 4 panels: SPI0 + SPI1 with two panels chained per bus, or enable more of
  the CM5/RP1's SPI buses via overlay for one panel each.

## Per-LED mapping (the "custom panel" part)

Panels are face-shaped, not grids. Each chain takes:

- `map_file` — JSON `{"leds": [[x, y], ...]}`: entry i = the canvas pixel LED
  i (chain order) samples. Author it from your panel CAD/layout.
- or `cols`/`rows`/`pitch`/`canvas_x`/`canvas_y`/`serpentine` — generated
  rectangular grid for prototypes.

Bump `protoface.canvas_w/h` if you want more sampling resolution — the face
art scales automatically.

## Power — read this twice

8000 RGB LEDs at full white ≈ **480 A at 5 V**. This is a wiring-design
problem, not a rendering detail:

- Every chain enforces `power_limit_a` in software: each frame's estimated
  current (≈20 mA per fully-lit channel, scaled by brightness and the APA102
  global) is computed, and the frame is dimmed to stay under the cap.
  **Set it to what your injection wiring actually supports.**
- Inject 5 V at multiple points per panel; a 2000-LED panel at even 4 A means
  proper bus wire, not signal wire.
- Realistic face content (mostly dark, ~15% lit at moderate brightness) runs
  an order of magnitude below white-max — the cap is the safety net, not the
  operating point.

## Config

```json
"protoface": {
  "backend": "led_panels",
  "led_panels": { "chains": [
    { "name": "muzzle_l", "spi_device": "/dev/spidev0.0", "speed_hz": 12000000,
      "map_file": "config/panels/muzzle_l.json",
      "brightness": 64, "global_5bit": 31, "power_limit_a": 8.0 },
    { "name": "muzzle_r", "spi_device": "/dev/spidev1.0",
      "cols": 50, "rows": 40, "pitch": 1, "canvas_x": 64, "canvas_y": 10 }
  ]}
}
```

Verified off-target: wire format (start frame / 0xE0|global B-G-R / end
clocks), serpentine grid mapping, and the power cap (a 120 A white frame
clamps to exactly the configured 8 A) — `scratchpad ledpanel_test`.
