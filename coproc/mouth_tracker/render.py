"""FacePreview — Python port of the C++ FaceLoader blendshape mouth stack.

Composites a face folder's neutral base + blend_*.png layers with live weights,
matching src/face/face_loader.cpp::get_frame (alpha-over, per-side regions) so
the demo shows what the LED panels would show. Kept intentionally small; if the
C++ compositing changes, update this to match.
"""

import json
import os
from typing import Dict, List, Optional

import cv2
import numpy as np

from .mouth_blendshapes import MOUTH_BLENDSHAPES, MouthSide


class _Region:
    __slots__ = ("x", "y", "w", "h", "set")

    def __init__(self, x=0, y=0, w=0, h=0, is_set=False):
        self.x, self.y, self.w, self.h, self.set = x, y, w, h, is_set


def _load_rgba(path: str, w: int, h: int) -> Optional[np.ndarray]:
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        return None
    if img.ndim == 2:                       # grayscale -> BGRA
        img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGRA)
    elif img.shape[2] == 3:                 # BGR -> BGRA (opaque)
        img = cv2.cvtColor(img, cv2.COLOR_BGR2BGRA)
    if (img.shape[1], img.shape[0]) != (w, h):
        img = cv2.resize(img, (w, h), interpolation=cv2.INTER_NEAREST)
    return img                               # BGRA uint8


class FacePreview:
    """Loads a face folder and renders the mouth stack. valid() is False when
    the folder has no usable base/layers (demo then shows bars only)."""

    def __init__(self, face_dir: str, canvas_w: int, canvas_h: int):
        self.w, self.h = canvas_w, canvas_h
        self.base: Optional[np.ndarray] = None
        self.layers: Dict[str, np.ndarray] = {}
        self.mouth = _Region()
        self.mouth_left = _Region()
        self.mouth_right = _Region()
        if face_dir:
            self._load(face_dir)

    def valid(self) -> bool:
        return self.base is not None and len(self.layers) > 0

    def _load(self, face_dir: str):
        cfg = {}
        cfg_path = os.path.join(face_dir, "config.json")
        if os.path.isfile(cfg_path):
            try:
                with open(cfg_path) as f:
                    cfg = json.load(f)
            except Exception:
                cfg = {}

        # Base: prefer neutral.png, else first expression-ish png, else black.
        base = None
        for name in ("neutral.png", "idle.png"):
            p = os.path.join(face_dir, name)
            if os.path.isfile(p):
                base = _load_rgba(p, self.w, self.h)
                break
        if base is None:
            base = np.zeros((self.h, self.w, 4), np.uint8)
            base[:, :, 3] = 255
        self.base = base

        for d in MOUTH_BLENDSHAPES:
            p = os.path.join(face_dir, d.stem + ".png")
            if os.path.isfile(p):
                img = _load_rgba(p, self.w, self.h)
                if img is not None:
                    self.layers[d.stem] = img

        # Region scaling from draw_size, mirroring FaceLoader.
        sx = sy = 1.0
        ds = cfg.get("draw_size")
        if isinstance(ds, list) and len(ds) == 2 and ds[0] and ds[1]:
            sx, sy = self.w / ds[0], self.h / ds[1]

        def region(key):
            d = cfg.get(key)
            if not isinstance(d, dict):
                return _Region()
            return _Region(round(d.get("x", 0) * sx), round(d.get("y", 0) * sy),
                           max(1, round(d.get("w", 1) * sx)),
                           max(1, round(d.get("h", 1) * sy)), True)

        self.mouth = region("mouth")
        self.mouth_left = region("mouth_left")
        self.mouth_right = region("mouth_right")

    def _region_for_side(self, side: MouthSide) -> _Region:
        if side == MouthSide.LEFT and self.mouth_left.set:
            return self.mouth_left
        if side == MouthSide.RIGHT and self.mouth_right.set:
            return self.mouth_right
        return self.mouth

    def render(self, weights: List[float], conf: float) -> np.ndarray:
        """Return an (h, w, 3) BGR image of the composited mouth."""
        frame = self.base[:, :, :3].astype(np.float32).copy()
        conf = max(0.0, min(1.0, conf))
        for i, d in enumerate(MOUTH_BLENDSHAPES):
            layer = self.layers.get(d.stem)
            if layer is None or i >= len(weights):
                continue
            wv = max(0.0, min(1.0, weights[i])) * conf
            if wv <= 0.0:
                continue
            reg = self._region_for_side(d.side)
            x0, y0 = (reg.x, reg.y) if reg.set else (0, 0)
            x1 = min(reg.x + reg.w, self.w) if reg.set else self.w
            y1 = min(reg.y + reg.h, self.h) if reg.set else self.h
            x0, y0 = max(0, x0), max(0, y0)
            if x1 <= x0 or y1 <= y0:
                continue
            a = (layer[y0:y1, x0:x1, 3:4].astype(np.float32) / 255.0) * wv
            rgb = layer[y0:y1, x0:x1, :3].astype(np.float32)
            frame[y0:y1, x0:x1] = frame[y0:y1, x0:x1] * (1.0 - a) + rgb * a
        return np.clip(frame, 0, 255).astype(np.uint8)
