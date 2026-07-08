# Authoring blendshape mouth layers for a face

This is the art/config spec for the **blendshape mouth stack** added in Phase 1
of the optical mouth-tracking work (see
`docs/mouth-tracking-blendshape-design.md`). Follow it when creating the
reference faces that ship with ProtoHUD.

A face that provides these layers gains **asymmetric, multi-shape** mouth
animation driven by the optical tracker (or, until the Pi Zero is wired, by the
**Face Display ▸ Mouth Tracker (Test)** sliders). Faces *without* these layers
keep working exactly as before on the audio-driven `mouth_open` path — the
blendshape stack is purely additive and opt-in.

## Where layers live

Inside a face folder `faces/<name>/`, alongside the expression PNGs
(`neutral.png`, `happy.png`, …), `blink.png`, and the existing viseme overlays
(`mouth_open.png`, …):

```
faces/<name>/
  neutral.png            ← expression (full canvas, RGBA)
  happy.png
  blink.png
  mouth_open.png         ← legacy audio viseme overlays (still supported)
  blend_jawOpen.png      ← NEW blendshape layers (all optional)
  blend_mouthSmileLeft.png
  blend_mouthSmileRight.png
  ...
  config.json
```

## Layer files

Each layer is a **full-canvas RGBA PNG** sized like every other sprite (it gets
scaled to the panel canvas). Author it as an **additive delta from neutral**:
only the part of the mouth that this shape moves should be **opaque**; everything
else must be **fully transparent**. Layers are alpha-stacked, so transparent
pixels never disturb the base face or other layers.

The filename stems are fixed by the contract in
`src/face/mouth_blendshapes.h` (index order is shared with the coprocessor
protocol — do not rename). The full set, in order:

| Layer file | MediaPipe coeff | Clipped to region | Suggested look |
|---|---|---|---|
| `blend_jawOpen.png` | `jawOpen` | `mouth` | overall open mouth |
| `blend_mouthSmileLeft.png` | `mouthSmileLeft` | `mouth_left` | left corner up |
| `blend_mouthSmileRight.png` | `mouthSmileRight` | `mouth_right` | right corner up |
| `blend_mouthFrownLeft.png` | `mouthFrownLeft` | `mouth_left` | left corner down |
| `blend_mouthFrownRight.png` | `mouthFrownRight` | `mouth_right` | right corner down |
| `blend_mouthPucker.png` | `mouthPucker` | `mouth` | tight rounded "OO" |
| `blend_mouthFunnel.png` | `mouthFunnel` | `mouth` | open round "OH" |
| `blend_mouthLeft.png` | `mouthLeft` | `mouth` | mouth shifted left |
| `blend_mouthRight.png` | `mouthRight` | `mouth` | mouth shifted right |
| `blend_mouthUpperUpLeft.png` | `mouthUpperUpLeft` | `mouth_left` | left upper-lip raise |
| `blend_mouthUpperUpRight.png` | `mouthUpperUpRight` | `mouth_right` | right upper-lip raise |
| `blend_mouthClose.png` | `mouthClose` | `mouth` | pressed/sealed lips |

**Start small:** a strong first set is `blend_jawOpen` + the four
smile/frown L/R layers + `blend_mouthPucker` + `blend_mouthFunnel` (indices 0–6).
Add the rest later — missing layers simply contribute nothing.

## config.json regions

The compositor clips each layer to a mouth region. Reuse the existing `mouth`
box; optionally add `mouth_left` / `mouth_right` for sharper per-side clipping
(Left/Right layers fall back to `mouth` when these are absent):

```jsonc
{
  "draw_size": [64, 32],          // author-space size; regions scale to panel px
  "eye_left":  { "x": 8,  "y": 6,  "w": 16, "h": 12 },
  "eye_right": { "x": 40, "y": 6,  "w": 16, "h": 12 },
  "mouth":       { "x": 20, "y": 22, "w": 24, "h": 8 },
  "mouth_left":  { "x": 20, "y": 22, "w": 12, "h": 8 },   // optional
  "mouth_right": { "x": 32, "y": 22, "w": 12, "h": 8 }    // optional
}
```

If you omit `mouth` entirely, layers composite across the whole canvas — which is
fine because each layer's own alpha already masks it to the mouth area. Defining
the region just bounds the work and sharpens per-side asymmetry.

## How the stack composites (what to expect)

Per frame, after the expression crossfade and blink:

1. The renderer reads the tracker's weight per layer (`0..1`) × global
   confidence (`0..1`).
2. Each present layer is alpha-blended **over** the face at its region with
   `opacity = layer_alpha × weight × confidence`.
3. Base shapes (jawOpen, funnel/pucker) are listed first; corner/asymmetry
   layers stack on top.

Because it's alpha-over (not cross-fade), multiple layers combine cleanly —
e.g. `jawOpen=0.6` + `mouthSmileLeft=0.9` reads as an open mouth with the left
corner pulled up. Design the opaque regions so overlapping layers reinforce
rather than fight (keep each layer's ink mostly to its own area).

## Testing without the Pi Zero

1. Select the native Protoface backend and a face that has `blend_*` layers.
2. Open **Face Display ▸ Mouth Tracker (Test)**, turn **Enabled** on.
3. Push individual coefficient sliders (and **Confidence**) and watch the panels
   / in-HUD preview. Asymmetric combinations should be visible.
4. Turn **Enabled** off to release the mouth back to the audio (Voice) path.
