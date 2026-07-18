#pragma once
// ── eye_anim.h ─────────────────────────────────────────────────────────────────
// Lightweight (no-OpenCV) spec for the procedural "animated eye" reactions
// (e.g. a hypnotic spiral or pulsing hearts that take over the panels when a
// boop zone is rapidly triggered). Kept dependency-free so the face-controller
// interface and AppState can reference it without pulling in OpenCV. The actual
// pixel rendering lives in eye_animations.h/.cpp.

#include <cstdint>

namespace face {

enum class EyeAnim : int {
    Spiral = 0,   // rotating hypnotic spiral
    Rings,        // expanding concentric rings
    Hearts,       // pulsing heart
    Swirl,        // dizzy swirl
    Starburst,    // rotating rays from centre
    Glitch,       // random colour blocks
    Count
};

// Tunable parameters for one animated-eye reaction. Editable from the menu so
// users can re-skin the built-in animations (rate, scale, colour, hold time).
struct EyeAnimParams {
    EyeAnim type       = EyeAnim::Spiral;
    double  speed      = 1.0;          // animation-rate multiplier
    double  size       = 1.0;          // feature-scale multiplier
    uint8_t r = 0, g = 220, b = 180;   // primary colour (RGB)
    double  duration_s = 2.5;          // how long it plays before the face returns
    // Centre of the animation on each panel, panel-normalised (0..1) — each
    // panel draws its own copy, so eye-panel rigs shift both eyes together.
    // Glitch fills the whole panel and ignores this.
    double  cx = 0.5, cy = 0.5;
    // Horizontally mirror the animation (a spiral spins the other way). The
    // configured centre is kept — only the animation itself flips.
    bool    mirror = false;
};

} // namespace face
