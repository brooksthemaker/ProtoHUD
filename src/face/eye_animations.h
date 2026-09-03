#pragma once
// ── eye_animations.h ───────────────────────────────────────────────────────────
// Procedural "animated eye" renderers. render_eye_animation() returns an
// (h, w) fully-opaque RGBA cv::Mat (CV_8UC4) for time t seconds into the
// animation — the same layer format every other face_layer producer emits.
// Used by NativeFaceController when a boop zone is rapidly triggered.

#include <opencv2/core.hpp>

#include "face/eye_anim.h"
#include "face/eye_lid.h"

namespace face {

// Render one frame of the given animation, t seconds in, at panel size w×h.
//
// `lid` (optional) is the face's per-column closed-lid line, used by Crying so
// its tears fall from the eye's own lower edge. `lid_x0` is the profile column
// this render's x=0 corresponds to — non-zero on the Mirror path, which renders
// a half-width frame covering the panel's RIGHT half. A null or mismatched
// profile falls back to a synthetic flat lid at Position Y, so callers with no
// face context (the menu preview) keep working unchanged.
// `gravity_deg` is the head's roll angle from the IMU: 0 = upright (tears fall
// straight down), positive/negative leans the fall so they run downhill and pool
// on the low side. Defaults to upright for callers with no IMU (menu preview).
cv::Mat render_eye_animation(const EyeAnimParams& p, double t, int w, int h,
                             const EyeLidLine* lid = nullptr, int lid_x0 = 0,
                             float gravity_deg = 0.f);

// Display name + count for the menu picker.
const char* eye_anim_name(EyeAnim a);
int         eye_anim_count();

} // namespace face
