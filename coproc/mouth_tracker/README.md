# Mouth tracker (coprocessor + CM5 demo)

MediaPipe-based mouth tracking that turns a camera feed into the mouth
**blendshape** weights ProtoHUD's LED face consumes.

Two entry points share one core (`tracker.py`):

* **`demo.py`** — a preview window you run **on the CM5** (or any desktop). Shows
  the live camera + face mesh, a bar chart of the mouth blendshape weights, and —
  when pointed at a face folder that has `blend_*.png` art — a live render of the
  actual protogen mouth reacting to your face. No LED panels or Pi Zero needed.
* **`serve.py`** *(Phase 3, not yet built)* — the headless service for the
  **Pi Zero 2 W** that streams the same weights to the CM5 over UART.

The blendshape contract (`mouth_blendshapes.py`) is a hand-kept mirror of the C++
`src/face/mouth_blendshapes.h` — the index order MUST match on both sides.

## Setup

Runs on aarch64 (CM5 and Pi Zero 2 W). **Not** the original Pi Zero 1.3 (ARMv6 —
MediaPipe is unavailable there).

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r coproc/mouth_tracker/requirements.txt

# One-time: fetch the FaceLandmarker model (includes blendshapes).
wget -O face_landmarker.task \
  https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task
```

For the CSI camera path you also need Picamera2 (`sudo apt install python3-picamera2`).

## Run the demo

```bash
# USB webcam (index 0), tracking-only (bars + mesh, no face render):
python -m coproc.mouth_tracker.demo --model face_landmarker.task --source 0

# ...plus the live protogen mouth from a face folder that has blend_*.png art:
python -m coproc.mouth_tracker.demo --model face_landmarker.task --source 0 \
    --face faces/main --canvas 64x32 --scale 10

# CSI camera instead of USB:
python -m coproc.mouth_tracker.demo --model face_landmarker.task --source csi
```

Keys: `q` quit · `n` capture neutral (zeroes the current face as the resting
baseline) · `r` reset neutral · `[` / `]` lower/raise gain.

See `docs/mouth-blendshape-faces.md` for how to author the `blend_*.png` layers,
and `docs/mouth-tracking-blendshape-design.md` for the overall design.
