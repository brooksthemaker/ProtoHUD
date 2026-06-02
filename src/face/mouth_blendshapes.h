#pragma once
// ── mouth_blendshapes.h ────────────────────────────────────────────────────────
// Single source of truth for the mouth blendshape contract shared by:
//   • the coprocessor wire protocol (Pi Zero 2 W → CM5),
//   • the FaceLoader blendshape-stack compositor,
//   • the menu test injector,
//   • (future) the MouthTracker UART receiver.
//
// The index order is STABLE and APPEND-ONLY — both ends of the link address
// coefficients positionally, never by name. Each entry maps a MediaPipe
// FaceLandmarker blendshape coefficient to the PNG layer stem the FaceLoader
// looks for in a face folder (<stem>.png), and to which mouth sub-region clips
// the layer when the face provides per-side regions.
//
// See docs/mouth-tracking-blendshape-design.md.

#include <cstddef>
#include <vector>

namespace face {

// Which mouth region a layer is clipped to. Center uses the single "mouth"
// region; Left/Right prefer "mouth_left"/"mouth_right" when the face folder
// defines them, falling back to "mouth" otherwise.
enum class MouthSide { Center, Left, Right };

struct MouthBlendshapeDef {
    const char* coeff;   // MediaPipe coefficient name (contract label)
    const char* stem;    // PNG layer file stem: faces/<face>/<stem>.png
    MouthSide   side;    // region that clips this layer
};

// Fixed, index-stable ordering. Append new entries only at the end so existing
// coprocessor builds stay wire-compatible.
inline const std::vector<MouthBlendshapeDef>& mouth_blendshapes() {
    static const std::vector<MouthBlendshapeDef> defs = {
        { "jawOpen",           "blend_jawOpen",           MouthSide::Center },
        { "mouthSmileLeft",    "blend_mouthSmileLeft",    MouthSide::Left   },
        { "mouthSmileRight",   "blend_mouthSmileRight",   MouthSide::Right  },
        { "mouthFrownLeft",    "blend_mouthFrownLeft",    MouthSide::Left   },
        { "mouthFrownRight",   "blend_mouthFrownRight",   MouthSide::Right  },
        { "mouthPucker",       "blend_mouthPucker",       MouthSide::Center },
        { "mouthFunnel",       "blend_mouthFunnel",       MouthSide::Center },
        { "mouthLeft",         "blend_mouthLeft",         MouthSide::Center },
        { "mouthRight",        "blend_mouthRight",        MouthSide::Center },
        { "mouthUpperUpLeft",  "blend_mouthUpperUpLeft",  MouthSide::Left   },
        { "mouthUpperUpRight", "blend_mouthUpperUpRight", MouthSide::Right  },
        { "mouthClose",        "blend_mouthClose",        MouthSide::Center },
    };
    return defs;
}

inline std::size_t mouth_blendshape_count() { return mouth_blendshapes().size(); }

} // namespace face
