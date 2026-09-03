#pragma once
// ── eye_lid.h ──────────────────────────────────────────────────────────────────
// Per-column lower edge of a face's blink eye regions — the "closed lid line".
//
// The blink itself draws no geometry: it alpha-crossfades blink.png inside the
// eye polygons authored in the face editor (config.json eye_left / eye_right).
// Those polygons ARE real geometry though, and their LOWER edge is exactly where
// a tear would leave the eye — so FaceLoader derives this profile from the same
// stencil it builds for the blink, and the Crying animation hangs its droplets
// off it. That way the tears track whatever the user actually drew.
//
// Kept dependency-free (no OpenCV) like eye_anim.h, so both the loader and the
// renderer can hold it without dragging headers around. Deliberately NOT part of
// EyeAnimParams — this is derived per-face state, not a persisted user setting.

#include <cstdint>
#include <vector>

namespace face {

struct EyeLidLine {
    int width  = 0;                  // panel columns this profile covers
    int height = 0;                  // panel rows (sanity-checked against the render size)
    // size() == width. The LOWEST lit stencil row in that column, or -1 when the
    // column contains no eye at all (e.g. the bridge of the nose between them).
    std::vector<int16_t> bottom;

    bool empty() const {
        return width <= 0 || static_cast<int>(bottom.size()) != width;
    }
};

} // namespace face
