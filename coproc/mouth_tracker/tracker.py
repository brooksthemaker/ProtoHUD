"""MouthTracker — camera-agnostic MediaPipe FaceLandmarker → mouth blendshapes.

Shared by demo.py (CM5 preview) and the future serve.py (Pi Zero 2 W UART
service). Feed it RGB frames + timestamps; it returns smoothed blendshape
weights in contract order plus a tracking confidence and the raw landmarks
(for overlay drawing).
"""

from dataclasses import dataclass, field
from typing import List, Optional

import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision as mp_vision

from .mouth_blendshapes import MOUTH_BLENDSHAPES, COEFF_INDEX, mouth_blendshape_count
from .one_euro import OneEuroFilter


@dataclass
class TrackResult:
    weights: List[float]                 # contract order, [0,1], smoothed
    confidence: float                    # [0,1]
    landmarks: Optional[list] = None     # raw NormalizedLandmark list (or None)
    raw: dict = field(default_factory=dict)  # coeff name -> raw score (debug)


class MouthTracker:
    def __init__(self, model_path: str,
                 min_cutoff: float = 1.0, beta: float = 0.01,
                 gain: float = 1.0):
        base = mp_python.BaseOptions(model_asset_path=model_path)
        options = mp_vision.FaceLandmarkerOptions(
            base_options=base,
            output_face_blendshapes=True,
            output_facial_transformation_matrixes=False,
            num_faces=1,
            running_mode=mp_vision.RunningMode.VIDEO,
        )
        self._landmarker = mp_vision.FaceLandmarker.create_from_options(options)

        n = mouth_blendshape_count()
        self._filters = [OneEuroFilter(min_cutoff=min_cutoff, beta=beta)
                         for _ in range(n)]
        self._neutral = [0.0] * n     # per-coeff resting baseline (subtracted)
        self._pending_neutral = False
        self.gain = float(gain)
        self._conf = 0.0              # ramped presence confidence

    # ── Calibration ─────────────────────────────────────────────────────────
    def capture_neutral(self):
        """Snapshot the next frame's raw weights as the resting baseline."""
        self._pending_neutral = True

    def reset_neutral(self):
        self._neutral = [0.0] * len(self._neutral)

    # ── Per-frame ───────────────────────────────────────────────────────────
    def process(self, rgb, timestamp_ms: int) -> TrackResult:
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        result = self._landmarker.detect_for_video(mp_image, int(timestamp_ms))

        t = timestamp_ms / 1000.0
        n = len(self._filters)

        have_face = bool(result.face_blendshapes)
        # Presence-based confidence with a short ramp so brief dropouts don't
        # slam the mouth shut (mirrors the CM5 stale-timeout intent).
        target = 1.0 if have_face else 0.0
        self._conf += (target - self._conf) * (0.5 if have_face else 0.2)
        self._conf = max(0.0, min(1.0, self._conf))

        if not have_face:
            # Let filters relax toward 0 so the mouth eases closed.
            weights = [self._filters[i](0.0, t) for i in range(n)]
            return TrackResult(weights=weights, confidence=self._conf,
                               landmarks=None, raw={})

        cats = result.face_blendshapes[0]
        raw_by_name = {c.category_name: c.score for c in cats}

        # Snapshot neutral if requested (raw, pre-offset).
        if self._pending_neutral:
            self._neutral = [raw_by_name.get(d.coeff, 0.0)
                             for d in MOUTH_BLENDSHAPES]
            self._pending_neutral = False

        weights = []
        raw_out = {}
        for i, d in enumerate(MOUTH_BLENDSHAPES):
            raw = raw_by_name.get(d.coeff, 0.0)
            raw_out[d.coeff] = raw
            v = (raw - self._neutral[i]) * self.gain
            v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
            weights.append(self._filters[i](v, t))

        landmarks = result.face_landmarks[0] if result.face_landmarks else None
        return TrackResult(weights=weights, confidence=self._conf,
                           landmarks=landmarks, raw=raw_out)

    def close(self):
        self._landmarker.close()
