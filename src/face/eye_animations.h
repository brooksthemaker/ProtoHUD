#pragma once
// ── eye_animations.h ───────────────────────────────────────────────────────────
// Procedural "animated eye" renderers. render_eye_animation() returns an
// (h, w) fully-opaque RGBA cv::Mat (CV_8UC4) for time t seconds into the
// animation — the same layer format every other face_layer producer emits.
// Used by NativeFaceController when a boop zone is rapidly triggered.

#include <opencv2/core.hpp>

#include "face/eye_anim.h"

namespace face {

// Render one frame of the given animation, t seconds in, at panel size w×h.
cv::Mat render_eye_animation(const EyeAnimParams& p, double t, int w, int h);

// Display name + count for the menu picker.
const char* eye_anim_name(EyeAnim a);
int         eye_anim_count();

} // namespace face
