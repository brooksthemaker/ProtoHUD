# Optical Mouth Tracking → HUB75 Blendshape Mouth Driver

**Status:** Phases 0–1 implemented (render path + test injector); Phases 2–4 pending
**Target branch:** `claude/mouth-tracking-hub75-BRRTn`
**Author context:** design doc, then incremental implementation

> **Implemented so far (Phases 0–1):** the `set_mouth_blendshapes` seam through
> `IFaceController`/`FaceProxy`/`NativeFaceController`/`FaceState`, the weighted
> blendshape **stack** in `FaceLoader` (alpha-over `blend_*` layers with optional
> per-side `mouth_left`/`mouth_right` regions), the shared contract in
> `src/face/mouth_blendshapes.h`, and a **Face Display ▸ Mouth Tracker (Test)**
> menu that drives the stack with no hardware. Authoring guide:
> `docs/mouth-blendshape-faces.md`. Still pending: the UART `MouthTracker`
> receiver (Phase 2), the Pi Zero 2 W service (Phase 3), and polish (Phase 4).

---

## 1. Goal & locked decisions

Drive the HUB75 LED face's mouth from the wearer's **actual mouth**, with enough
fidelity for **asymmetrical** shapes (smirk, one-sided "O", lopsided smile),
while leaving the CM5's CPU/GPU headroom free for camera compositing + HUD.

Decisions already made in design discussion:

| Decision | Choice | Rationale |
|---|---|---|
| Signal source | Optical (camera on the real mouth) | True 1:1 tracking, works when silent |
| Topology | **Satellite coprocessor** at front of head, CM5 on back | Offload vision; single thin cable run |
| Coprocessor | **Raspberry Pi Zero 2 W** | Quad A53 can run MediaPipe FaceLandmarker at low res; small/low power |
| Vision model | **MediaPipe FaceLandmarker** w/ blendshape coefficients | Emits ARKit-style mouth/jaw weights directly; pose-normalized |
| Render model | **Blendshape stack** on the CM5 | Asymmetry, keeps PNG art pipeline, plugs 1:1 into tracker output |
| Link | **UART** (primary) or USB-serial gadget | Tiny payload; reuses existing serial framing |
| Audio role | **Fallback** when optical confidence is low / tracker offline | Mouth still moves if camera drops out |

The key architectural insight that justifies "blendshape stack": MediaPipe's
blendshape coefficients are **per-expression weights that are already largely
decoupled from head pose**, so the tracker output *is* the render input — no
landmark-to-screen mapping or heavy per-user calibration required to get usable
asymmetry.

---

## 2. End-to-end architecture

```
┌──────────────────────── FRONT OF HEAD (snout) ───────────────────────┐
│ Raspberry Pi Zero 2 W                                                 │
│                                                                       │
│  NoIR CSI camera ──► MediaPipe FaceLandmarker (Tasks API)             │
│  + IR LEDs           │  • 478 landmarks + 52 blendshape coeffs        │
│  (mouth-lit)         ▼                                                 │
│              select mouth/jaw subset (~10–12 coeffs)                   │
│                      │                                                 │
│                      ▼                                                 │
│              one-euro smoothing + neutral offset + gain               │
│                      │                                                 │
│                      ▼                                                 │
│              frame: {seq, conf, w[0..N]}  (binary, ~30 B)             │
└──────────────────────────────────┬───────────────────────────────────┘
                                    │  UART (3 wire) @ 115200–460800
                                    │  or USB-serial gadget
┌──────────────────────────────────┼──── BACK OF HEAD ──────────────────┐
│ Raspberry Pi CM5                  ▼                                     │
│   MouthTracker (new)  ── parses frames on its own thread               │
│        │                                                               │
│        │  face_proxy.set_mouth_blendshapes(weights, confidence)        │
│        ▼                                                               │
│   FaceProxy ──► NativeFaceController ──► FaceState (per panel)         │
│        ▲                                     │                          │
│   VoiceAnalyzer ─ set_audio_drive (fallback) │                         │
│                                              ▼                          │
│                                   FaceLoader::get_frame                 │
│                                   • expression crossfade               │
│                                   • blink                              │
│                                   • MOUTH: weighted blendshape stack   │ ◄── new
│                                              │                          │
│                                              ▼                          │
│                          ShmPusherOutput → /dev/shm/protoface_frame    │
└──────────────────────────────────┬───────────────────────────────────┘
                                    ▼
                       scripts/panel_driver.py (Piomatter) ──► HUB75
```

Nothing downstream of `FaceLoader` changes: the canvas still lands in
`/dev/shm/protoface_frame` and `panel_driver.py` still bridges to Piomatter.

---

## 3. Coprocessor (Pi Zero 2 W) software

A standalone Python service, e.g. `coproc/mouth_tracker_zero/tracker.py`, kept
in this repo so it ships and versions with ProtoHUD.

### 3.1 Camera + illumination
- **NoIR CSI camera** (e.g. Pi Camera v2 NoIR / IMX219). The snout interior is
  dark; IR removes dependence on ambient light and gives the landmarker a
  consistent image.
- **2–4 IR LEDs (850 nm)** aimed at the mouth, diffused to avoid hotspots. Keep
  them off-axis from the eyes.
- Capture at **low resolution** (e.g. 320×240 or a mouth-region crop). The
  landmarker downscales internally anyway; lower res = higher fps on the Zero 2 W.

### 3.2 Inference
- **MediaPipe Tasks `FaceLandmarker`** with `output_face_blendshapes=True`,
  `num_faces=1`, running in `LIVE_STREAM` (or `VIDEO`) mode.
- Realistic throughput on a Zero 2 W: **~10–15 fps** at small input sizes. This
  is acceptable because (a) the mouth is heavily smoothed and (b) the CM5 render
  loop interpolates between updates. If 15 fps proves too coarse, drop input
  resolution / crop to the mouth ROI before inference.
- Extract only the mouth/jaw subset of the 52 coefficients (see §5.2).

### 3.3 Conditioning (on the Zero, before sending)
- **Neutral offset:** subtract a captured "resting face" baseline per coefficient
  so a closed, relaxed mouth maps to ~0.
- **Per-coefficient gain + clamp** to [0,1]; lets you exaggerate subtle motion to
  read on a 64-px stylized mouth.
- **One-euro filter** per coefficient: low latency *and* low jitter, adaptive to
  motion speed. Strongly preferred over a fixed EMA here.
- **Confidence:** derive from FaceLandmarker presence/tracking (and a simple
  "face detected this frame" flag). Sent with every frame so the CM5 can
  arbitrate vs. audio.

### 3.4 Service behavior
- systemd unit on the Zero; auto-start, auto-restart.
- If no face is detected for N frames, keep streaming `conf=0` (don't go silent)
  so the CM5 can deterministically fall back to audio.
- Boots independently of the CM5; the link is hot-pluggable.

---

## 4. The link (Zero 2 W ↔ CM5)

Payload is tiny (~30 B/frame at ≤30 Hz ≈ <1 kB/s), so choose for reliability and
code fit, not bandwidth.

### Primary: UART (recommended)
- 3 wires (TX, RX, GND) from the Zero's UART to a CM5 UART. RX-on-CM5 is the only
  strictly required direction; keep TX→Zero for future commands (e.g. "recapture
  neutral").
- **115200** is ample; **460800** gives headroom. Level-matched (both 3.3 V).
- Reuses ProtoHUD's existing framing in `src/serial/serial_port.{h,cpp}` (the
  same infra behind `smartknob` and `lora_radio`).

### Alternate: USB-serial gadget
- Zero in OTG peripheral mode presents `/dev/ttyACMx` (or g_ether) to the CM5.
- One USB cable carries **power + data** — attractive for a single front-cable run.
- Slightly more setup; same parsing code on the CM5 side.

**Recommendation:** UART for the cleanest fit with existing serial code; switch to
USB-gadget if you'd rather power the Zero down the same cable.

---

## 5. Wire protocol

A small, versioned, framed binary message. Reuse `SerialPort`'s framing +
checksum; the body below is the payload.

### 5.1 Frame layout
```
magic   : u8  = 0xMF
version : u8  = 0x01
seq     : u8           // wraps; CM5 detects drops
conf    : u8           // 0..255 tracking confidence
count   : u8  = N      // number of blendshape weights
w[0..N] : u8 each      // each coefficient quantized 0..255  (→ /255 on CM5)
crc16   : u16          // over the payload (or rely on SerialPort framing CRC)
```
~6 + N bytes. With N≈12 → ~18 B body. Quantizing to u8 is plenty for a 64-px
mouth; switch to f16/f32 only if banding ever shows.

### 5.2 Blendshape subset (the contract)
Order is fixed by index so both sides agree without sending names. Practical
mouth/jaw set for a stylized protogen:

| idx | MediaPipe coeff | Drives |
|----:|-----------------|--------|
| 0 | `jawOpen` | overall openness (replaces today's `mouth_open` scalar) |
| 1 | `mouthSmileLeft` | left corner up |
| 2 | `mouthSmileRight` | right corner up |
| 3 | `mouthFrownLeft` | left corner down |
| 4 | `mouthFrownRight` | right corner down |
| 5 | `mouthPucker` | rounded / "OOH" |
| 6 | `mouthFunnel` | open-round / "OH" |
| 7 | `mouthLeft` | mouth slid left |
| 8 | `mouthRight` | mouth slid right |
| 9 | `mouthUpperUpLeft` | upper-lip raise L (snarl) |
| 10 | `mouthUpperUpRight` | upper-lip raise R |
| 11 | `mouthClose` | press/seal (optional) |

Start with 0–6 (openness + L/R smile + L/R frown + pucker + funnel) for a strong
asymmetric MVP; add the rest as art is authored.

---

## 6. CM5 receiver — `MouthTracker` module

New files: `src/serial/mouth_tracker.h` / `.cpp` (mirrors `lora_radio` /
`smartknob`).

- Owns a `SerialPort`, runs a reader thread, parses §5 frames.
- On each valid frame: dequantize to `float w[N]`, normalize confidence, and call
  the new seam (below) on the `FaceProxy`.
- Detects `seq` gaps (log/metric only) and a staleness timeout: if no frame for
  `T_stale` (e.g. 250 ms), report `confidence = 0` so the renderer falls back.
- Config-driven device path, baud, N, and the index→meaning map (so the contract
  can evolve without code edits).
- Wired up in `main.cpp` next to the other serial devices; pushed into the same
  `face_proxy` already used by the audio callback (`main.cpp:9539`).

### 6.1 New seam on `IFaceController`
Add alongside `set_audio_drive` / `set_mouth_shape` in
`src/serial/face_controller.h`:

```cpp
// Optical mouth tracker: weighted blendshape coefficients in [0,1], indexed
// by the agreed mouth-blendshape order, plus tracking confidence in [0,1].
// Native Protoface forwards to each panel's FaceState; non-native backends
// no-op (they run their own / no mouth tracking).
virtual void set_mouth_blendshapes(const std::vector<float>& weights,
                                   float confidence) {}
```
- Add the matching override to `FaceProxy` (forward to `*active_`).
- Implement in `NativeFaceController` (forward to each non-mirror panel's
  `FaceState`).
- Default no-op keeps Teensy/daemon backends compiling and behaving unchanged.

---

## 7. Render-side upgrade (the bulk of the work)

Today the mouth is **one** region, **one** selected overlay, **one** scalar
(`face_loader.cpp:145-153`, `face_state.h:104-106`). That cannot express
asymmetry. The blendshape stack replaces "pick one overlay" with "blend N
weighted overlays."

### 7.1 `FaceState` additions
- New field: `std::vector<float> mouth_weights_;` and `float mouth_conf_ = 0.f;`.
- New setter `set_mouth_blendshapes(const std::vector<float>&, float conf)`.
- Read accessors for the compositor.
- **Backward compatibility:** keep `mouth_open_` / `mouth_shape_`. When optical
  confidence is high, the stack drives the mouth; when low, fall back to the
  legacy audio path (see §8). The old viseme path remains fully functional for
  backends/users without a tracker.

### 7.2 Asset / naming convention
Per face folder `faces/<name>/`, add optional blendshape layer PNGs (RGBA, sized
to the panel like existing overlays):

```
blend_jawOpen.png
blend_mouthSmileLeft.png
blend_mouthSmileRight.png
blend_mouthFrownLeft.png
blend_mouthFrownRight.png
blend_mouthPucker.png
blend_mouthFunnel.png
...
```
- All optional. A missing layer = that coefficient contributes nothing (graceful
  degradation; you can ship openness-only first).
- `config.json` mouth region(s): keep the single `mouth` region for symmetric
  layers; optionally add `mouth_left` / `mouth_right` regions so corner layers
  are clipped to their side (sharper asymmetry on a small grid).
- These import through the **same Files > Faces flow** (`import_face_image` /
  `face_image_path`), extending the canonical stem list in
  `face_loader.cpp:71` to include the `blend_*` stems.

### 7.3 Compositing math (`FaceLoader::get_frame`)
Replace step 3 ("Mouth open") with a stack:

1. Start from the post-blink `frame`.
2. For each loaded blendshape layer `i` with weight `w_i = state.mouth_weights_[i]`
   (× any per-layer gain), composite it **over** the frame at its region with
   opacity `clamp(w_i, 0, 1)`.
3. Order matters: composite base-shape layers (jawOpen, funnel/pucker) first,
   then corner/asymmetry layers (smile/frown L/R) on top.

**Avoid double-darkening:** the current `blend_region` uses `cv::addWeighted`
(`face_loader.cpp:109`), which cross-fades base↔overlay. Stacking several of
those sequentially washes the base out. For the stack, composite each layer with
**alpha-over using the layer's own RGBA alpha × weight** (premultiplied), so
transparent areas of a layer never touch the base. This is a small, well-contained
new helper (`blend_over_region`) next to `blend_region`.

- Layers should be authored as **additive deltas from neutral** (only the moving
  region is opaque), which makes "blend several at once" behave intuitively.
- The legacy single-overlay path stays as a fallback branch when no `blend_*`
  layers exist (so existing faces keep working untouched).

### 7.4 Resolution realism
At ~24–40 px mouth width the effective DOF is a handful of control points, so a
**12-layer set is plenty** — fidelity is bounded by the panel and the stylized
art, not the tracker. Author layers chunky and palette-aware (they go through the
existing material colorizer downstream).

---

## 8. Optical ↔ audio arbitration

Both producers feed `FaceState`. Policy (in `FaceState::update` or the
controller, single source of truth):

```
if mouth_conf >= conf_hi:        use blendshape stack fully
elif mouth_conf <= conf_lo:      use audio fallback (legacy mouth_open + viseme)
else:                            crossfade between the two by confidence
```
- `jawOpen` and the audio `mouth_open` scalar are directly comparable, so the
  crossfade is smooth.
- When falling back to audio, the viseme classifier (`VoiceAnalyzer::mouth_shape`)
  still picks round/open/small/smile as it does today.
- Thresholds + hysteresis exposed in config/menu.

This *is* the "hybrid" graceful-degradation behavior, achieved for free once both
inputs exist.

---

## 9. Calibration

Minimal, because MediaPipe blendshapes are pose-decoupled:
- **Neutral capture** (on the Zero): a "hold a relaxed face" button/command
  records the baseline offset. Triggerable over the link's CM5→Zero direction or
  a physical button on the daughterboard.
- **Per-coefficient gain** (Zero or CM5): tune how far real motion pushes the
  stylized mouth. Live-adjustable from the ProtoHUD menu (sent back over UART, or
  applied CM5-side before compositing).
- Optional **per-user profiles** saved on the Zero.

---

## 10. Configuration additions

`config.json` (consumed in `main.cpp`):
```jsonc
"mouth_tracker": {
  "enabled": true,
  "device": "/dev/ttyAMA0",      // or /dev/ttyACM0 for USB gadget
  "baud": 460800,
  "blendshape_count": 7,
  "conf_hi": 0.6,
  "conf_lo": 0.2,
  "stale_timeout_ms": 250,
  "layer_gain": { "mouthSmileLeft": 1.4, "mouthSmileRight": 1.4 }
}
```
Plus optional `mouth_left` / `mouth_right` regions in each face's `config.json`.
No change to `RenderConfig` canvas/output fields.

---

## 11. Menu / UI

Under the existing Face/Audio menu tree (`src/menu/menu_system.*`):
- Toggle: optical tracking on/off (falls back to audio when off).
- Confidence thresholds + hysteresis sliders.
- Per-layer gain sliders.
- "Recapture neutral" action (sends command to the Zero).
- Live readout: current confidence + per-coefficient weights (debugging aid).
- The in-HUD face preview (`latest_frame`) already shows the result.

---

## 12. Latency budget

| Stage | Approx |
|---|---|
| Camera exposure + read | ~33 ms @30fps capture |
| FaceLandmarker on Zero 2 W | ~66–100 ms (10–15 fps) |
| One-euro smoothing | adds ~0 (chosen for low lag) |
| UART frame | <1 ms |
| CM5 render tick | ~33 ms (30 fps) |
| panel_driver poll + HUB75 refresh | ~16–33 ms |
| **End-to-end** | **~150–200 ms** |

Costume-acceptable, but mitigations if it feels laggy: crop-to-mouth before
inference (raises fps), tighten one-euro `min_cutoff`, and let the CM5 interpolate
weights between tracker updates.

---

## 13. Failure modes

| Failure | Behavior |
|---|---|
| Zero offline / cable unplugged | stale timeout → `conf=0` → audio fallback |
| Face not detected (fogged lens, look-away) | Zero streams `conf=0` → audio fallback |
| Dropped/corrupt frames | `SerialPort` CRC rejects; seq-gap logged; last-good held briefly then stale |
| Missing `blend_*` art | that layer contributes nothing; openness still works |
| Tracker enabled but no audio either | mouth rests at neutral (safe) |

---

## 14. Phased implementation plan

**Phase 0 — Plumbing (no behavior change, no hardware)** ✅ done
1. ✅ Add `set_mouth_blendshapes` to `IFaceController` + `FaceProxy` (no-op default).
2. ✅ Implement forward in `NativeFaceController` → new `FaceState` fields/setter.
3. ✅ Add a dev/test injector (**Face Display ▸ Mouth Tracker (Test)** sliders) so
   the render path is testable on the CM5 **with no Zero attached**.

**Phase 1 — Render: blendshape stack** ✅ done (art pending)
4. ✅ `blend_over_region` helper + stacked compositing in `FaceLoader::get_frame`.
5. ✅ Shared contract `mouth_blendshapes.h`; `blend_*` layers loaded + excluded
   from the scanned expression list; optional per-side regions.
6. ⏳ Author a starter layer set for one face (openness + L/R smile + L/R frown +
   pucker) — **owner: user**. Verify asymmetry on panels via the slider injector.

**Phase 2 — Link + receiver**
7. `MouthTracker` module (`SerialPort` reader, parse, push to `face_proxy`).
8. Protocol encode/decode shared constants; wire into `main.cpp`.
9. Arbitration + stale timeout + menu toggles.

**Phase 3 — Coprocessor**
10. Zero 2 W service: camera + FaceLandmarker + subset + one-euro + neutral +
    framed UART output. systemd unit. NoIR + IR LED bring-up.
11. Calibration UX (recapture neutral, gains).

**Phase 4 — Polish**
12. Per-side mouth regions, more layers, latency tuning, profiles, docs.

Each phase is independently testable; Phases 0–1 need no new hardware.

---

## 15. Testing & validation

- **No-hardware render test:** manual blendshape sliders / replay file → confirm
  asymmetric shapes on panels (or the in-HUD preview).
- **Loopback link test:** a host script emits synthetic §5 frames over a serial
  pair → exercises `MouthTracker` parsing, seq-gap, stale timeout, arbitration.
- **Coproc bench test:** run the Zero service against recorded IR video; verify
  coefficient stability + latency before mounting in the helmet.
- **Integration:** end-to-end with audio fallback forced (cover the lens) to
  confirm graceful degradation.

---

## 16. File-by-file change list

| File | Change |
|---|---|
| `src/serial/face_controller.h` | + `set_mouth_blendshapes()` on `IFaceController` and `FaceProxy` |
| `src/face/native_face_controller.{h,cpp}` | implement forward to per-panel `FaceState` |
| `src/face/face_state.{h,cpp}` | mouth weights + confidence fields, setter, accessors |
| `src/face/face_loader.{h,cpp}` | load `blend_*` layers; `blend_over_region`; stacked compositing; keep legacy fallback |
| `src/serial/mouth_tracker.{h,cpp}` | **new** — UART receiver, parse, push to `face_proxy` |
| `src/serial/serial_port.{h,cpp}` | reuse as-is (framing/CRC) |
| `src/main.cpp` | instantiate `MouthTracker`; arbitration alongside audio cb (`~:9539`); config load |
| `src/menu/menu_system.*` | toggles, thresholds, gains, recapture-neutral |
| `src/app_state.h` | `MouthTrackerConfig` struct |
| `config/config.example.json` | `mouth_tracker` section + per-face region notes |
| `coproc/mouth_tracker_zero/` | **new** — Zero 2 W Python service + systemd unit + README |
| `docs/mouth-tracking-blendshape-design.md` | this document |

---

## 17. Open questions

1. **Power for the Zero:** down the UART cable run separately, or commit to the
   USB-serial gadget so one cable does power+data?
2. **Per-side regions now or later?** Single `mouth` region (corner layers
   overlap) is simpler; `mouth_left`/`mouth_right` clipping is sharper. Start
   single, add if needed.
3. **Quantization:** u8 per coefficient assumed sufficient; revisit if banding
   appears on slow opens.
4. **Layer authoring owner:** who draws the `blend_*` set, and do we ship a
   reference set for the default face?
5. **IR safety/comfort:** LED placement/intensity validated against eye exposure
   and lens fogging.
