#pragma once
// ── head_lock.h ───────────────────────────────────────────────────────────────
// World-locking ("pin in space") math for HUD overlays, driven by the VITURE
// glasses' built-in head IMU (AppState::imu_pose). A point pinned to a forward
// direction (anchor_yaw, anchor_pitch) is re-projected to a screen-pixel offset
// each frame as the head turns, so the overlay appears fixed in space.
//
// Uses the same pinhole model as the timewarp reprojection: a ray at angle θ
// from the optical axis lands at fx·tan(θ) pixels. fx/fy are the display focal
// lengths in pixels (the timewarp CameraIntrinsics — fx = fy = 1920 on VITURE).

#include "../app_state.h"   // ImuPose, ImVec2 (via imgui.h)
#include <cmath>

namespace headlock {

// Wrap a degree delta into [-180, 180] so yaw wrap-around (359°→1°) is smooth.
inline float wrap180(float d) {
    d = std::fmod(d + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    return d - 180.f;
}

// Pixel offset (from screen centre) at which a point world-locked to
// (anchor_yaw, anchor_pitch) should be drawn for the current head pose.
// invert_* flips the axis if the IMU sign is mirrored relative to the display
// (tunable from the menu so it can be corrected without a recompile).
inline ImVec2 screen_offset(float cur_yaw,   float cur_pitch,
                            float anchor_yaw, float anchor_pitch,
                            float fx, float fy,
                            bool invert_yaw = false, bool invert_pitch = false) {
    float dyaw   = wrap180(anchor_yaw   - cur_yaw);
    float dpitch = wrap180(anchor_pitch - cur_pitch);
    if (invert_yaw)   dyaw   = -dyaw;
    if (invert_pitch) dpitch = -dpitch;

    // Clamp near ±90° so tan() can't explode when looking far off-axis.
    const float lim = 85.f;
    if (dyaw   >  lim) dyaw   =  lim; else if (dyaw   < -lim) dyaw   = -lim;
    if (dpitch >  lim) dpitch =  lim; else if (dpitch < -lim) dpitch = -lim;

    const float r = 3.14159265358979f / 180.f;
    return ImVec2( fx * std::tan(dyaw   * r),     // turn head right → point slides left
                  -fy * std::tan(dpitch * r) );   // look up → point slides down
}

}  // namespace headlock
