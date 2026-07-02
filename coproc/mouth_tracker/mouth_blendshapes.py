"""Mouth blendshape contract — Python mirror of src/face/mouth_blendshapes.h.

The index order is STABLE and APPEND-ONLY, and MUST match the C++ header
exactly: it is the positional contract shared by the coprocessor wire protocol,
the FaceLoader blendshape stack, and this tracker. If you edit one, edit both.
"""

from dataclasses import dataclass
from enum import IntEnum


class MouthSide(IntEnum):
    CENTER = 0
    LEFT = 1
    RIGHT = 2


@dataclass(frozen=True)
class MouthBlendshapeDef:
    coeff: str   # MediaPipe FaceLandmarker blendshape category name
    stem: str    # PNG layer stem in a face folder: <stem>.png
    side: MouthSide


# Order matches src/face/mouth_blendshapes.h::mouth_blendshapes().
MOUTH_BLENDSHAPES = (
    MouthBlendshapeDef("jawOpen",           "blend_jawOpen",           MouthSide.CENTER),
    MouthBlendshapeDef("mouthSmileLeft",    "blend_mouthSmileLeft",    MouthSide.LEFT),
    MouthBlendshapeDef("mouthSmileRight",   "blend_mouthSmileRight",   MouthSide.RIGHT),
    MouthBlendshapeDef("mouthFrownLeft",    "blend_mouthFrownLeft",    MouthSide.LEFT),
    MouthBlendshapeDef("mouthFrownRight",   "blend_mouthFrownRight",   MouthSide.RIGHT),
    MouthBlendshapeDef("mouthPucker",       "blend_mouthPucker",       MouthSide.CENTER),
    MouthBlendshapeDef("mouthFunnel",       "blend_mouthFunnel",       MouthSide.CENTER),
    MouthBlendshapeDef("mouthLeft",         "blend_mouthLeft",         MouthSide.CENTER),
    MouthBlendshapeDef("mouthRight",        "blend_mouthRight",        MouthSide.CENTER),
    MouthBlendshapeDef("mouthUpperUpLeft",  "blend_mouthUpperUpLeft",  MouthSide.LEFT),
    MouthBlendshapeDef("mouthUpperUpRight", "blend_mouthUpperUpRight", MouthSide.RIGHT),
    MouthBlendshapeDef("mouthClose",        "blend_mouthClose",        MouthSide.CENTER),
)


def mouth_blendshape_count() -> int:
    return len(MOUTH_BLENDSHAPES)


# coeff name -> contract index, for mapping MediaPipe's category list.
COEFF_INDEX = {d.coeff: i for i, d in enumerate(MOUTH_BLENDSHAPES)}
