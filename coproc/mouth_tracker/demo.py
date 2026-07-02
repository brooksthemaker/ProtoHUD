"""Camera -> mouth-shape preview window (run on the CM5, no LED panels needed).

Left  : live camera + face-mesh overlay + tracking confidence.
Right : blendshape weight bars, plus — when --face points at a folder with
        blend_*.png art — a live render of the actual protogen mouth.

    python -m coproc.mouth_tracker.demo --model face_landmarker.task --source 0
    python -m coproc.mouth_tracker.demo --model face_landmarker.task \
        --source 0 --face faces/main --canvas 64x32 --scale 10

Keys: q quit · n capture neutral · r reset neutral · [ / ] gain down/up
"""

import argparse
import time

import cv2
import numpy as np

from .camera import open_camera
from .mouth_blendshapes import MOUTH_BLENDSHAPES
from .render import FacePreview
from .tracker import MouthTracker

# Lip landmark indices (MediaPipe face mesh) for a light overlay. Robust across
# versions; we just draw dots rather than depending on connection tables.
try:
    import mediapipe as _mp
    _LIPS = sorted({i for pair in _mp.solutions.face_mesh.FACEMESH_LIPS
                    for i in pair})
except Exception:
    _LIPS = []

PANEL_W = 360   # right-hand info panel width


def parse_canvas(s: str):
    w, h = s.lower().split("x")
    return int(w), int(h)


def draw_mesh(rgb, landmarks):
    """Draw all landmarks faint + lips highlighted onto an RGB frame."""
    if not landmarks:
        return
    h, w = rgb.shape[:2]
    for lm in landmarks:
        x, y = int(lm.x * w), int(lm.y * h)
        cv2.circle(rgb, (x, y), 1, (60, 90, 60), -1)
    for i in _LIPS:
        if i < len(landmarks):
            lm = landmarks[i]
            cv2.circle(rgb, (int(lm.x * w), int(lm.y * h)), 2, (0, 230, 0), -1)


def build_info_panel(height, weights, conf, gain, face_img, scale):
    """Right-hand BGR panel: rendered face (optional) atop weight bars."""
    panel = np.full((height, PANEL_W, 3), 24, np.uint8)
    y = 12

    if face_img is not None:
        fh, fw = face_img.shape[:2]
        big = cv2.resize(face_img, (fw * scale, fh * scale),
                         interpolation=cv2.INTER_NEAREST)
        bh, bw = big.shape[:2]
        x = max(0, (PANEL_W - bw) // 2)
        big = big[:, :min(bw, PANEL_W - x)]
        panel[y:y + min(bh, height - y), x:x + big.shape[1]] = \
            big[:min(bh, height - y)]
        y += min(bh, height - y) + 10
    else:
        cv2.putText(panel, "no blend_* art", (10, y + 12),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (120, 120, 120), 1)
        cv2.putText(panel, "(--face folder)", (10, y + 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (120, 120, 120), 1)
        y += 44

    cv2.putText(panel, f"conf {conf:4.2f}  gain {gain:4.2f}", (10, y),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 200, 200), 1)
    y += 16

    bar_h, gap, x0, bar_max = 11, 3, 150, PANEL_W - 150 - 10
    for i, d in enumerate(MOUTH_BLENDSHAPES):
        if y + bar_h > height:
            break
        wv = weights[i] if i < len(weights) else 0.0
        cv2.putText(panel, d.coeff, (8, y + bar_h - 2),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.38, (180, 180, 180), 1)
        cv2.rectangle(panel, (x0, y), (x0 + bar_max, y + bar_h), (60, 60, 60), 1)
        fill = int(bar_max * max(0.0, min(1.0, wv)))
        if fill > 0:
            cv2.rectangle(panel, (x0, y), (x0 + fill, y + bar_h),
                          (0, 200, 120), -1)
        y += bar_h + gap
    return panel


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True, help="face_landmarker.task path")
    ap.add_argument("--source", default="0", help="USB index or 'csi'")
    ap.add_argument("--face", default="", help="face folder with blend_*.png")
    ap.add_argument("--canvas", default="64x32", help="LED canvas WxH")
    ap.add_argument("--scale", type=int, default=10, help="face preview zoom")
    ap.add_argument("--cap-width", type=int, default=640)
    ap.add_argument("--cap-height", type=int, default=480)
    ap.add_argument("--min-cutoff", type=float, default=1.0)
    ap.add_argument("--beta", type=float, default=0.01)
    ap.add_argument("--gain", type=float, default=1.0)
    args = ap.parse_args()

    canvas_w, canvas_h = parse_canvas(args.canvas)
    cam = open_camera(args.source, args.cap_width, args.cap_height)
    tracker = MouthTracker(args.model, min_cutoff=args.min_cutoff,
                           beta=args.beta, gain=args.gain)
    preview = FacePreview(args.face, canvas_w, canvas_h) if args.face else None
    if preview is not None and not preview.valid():
        print("[demo] note: --face folder has no usable blend_* art; "
              "showing bars only.")
        preview = None

    win = "ProtoHUD mouth tracker demo"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    t0 = time.monotonic()
    last_ts = -1
    fps_ema = 0.0
    print("[demo] running — q quit, n neutral, r reset, [ / ] gain")

    try:
        while True:
            t_frame = time.monotonic()
            rgb = cam.read()
            if rgb is None:
                continue
            ts = int((t_frame - t0) * 1000)
            if ts <= last_ts:          # VIDEO mode needs strictly increasing ts
                ts = last_ts + 1
            last_ts = ts

            res = tracker.process(rgb, ts)
            draw_mesh(rgb, res.landmarks)

            bgr = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
            face_img = (preview.render(res.weights, res.confidence)
                        if preview is not None else None)

            dt = time.monotonic() - t_frame
            if dt > 0:
                fps_ema = 0.9 * fps_ema + 0.1 * (1.0 / dt)
            cv2.putText(bgr, f"{fps_ema:4.1f} fps  conf {res.confidence:4.2f}",
                        (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)

            panel = build_info_panel(bgr.shape[0], res.weights, res.confidence,
                                     tracker.gain, face_img, args.scale)
            combined = np.hstack([bgr, panel])
            cv2.imshow(win, combined)

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            elif key == ord("n"):
                tracker.capture_neutral()
                print("[demo] neutral captured")
            elif key == ord("r"):
                tracker.reset_neutral()
                print("[demo] neutral reset")
            elif key == ord("]"):
                tracker.gain = min(8.0, tracker.gain + 0.1)
            elif key == ord("["):
                tracker.gain = max(0.1, tracker.gain - 0.1)
    finally:
        cam.close()
        tracker.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
