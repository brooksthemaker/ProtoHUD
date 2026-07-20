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
    Glitch,       // random colour blocks (fills the panel)
    XEyes,        // cartoon K.O. cross
    Radar,        // rotating sweep with afterglow trail + range rings
    Fire,         // rising flame flicker (fills the panel)
    Rain,         // falling streaks (fills the panel)
    Sparkle,      // twinkling star field (fills the panel)
    Heartbeat,    // monitor-sweep ECG trace, redrawn left→right each pass
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
    // Draw the animation once per HALF of the panel — a right copy and a
    // horizontally-mirrored left copy, like a pair of eyes (directional
    // animations radiate outward from the centre) — instead of one instance
    // across the whole panel. cx/cy then position within each half.
    bool    mirror = false;
    // Composite the animation over the live face (which keeps rendering —
    // blinks, GIFs, effects and all) instead of taking over the panel.
    bool    overlay = false;
    // With overlay: black out the face's blink eye regions (config.json
    // eye_left / eye_right) while the animation plays, so an eye-positioned
    // animation replaces the eyes instead of glowing through them. Panels
    // whose face defines no eye regions are left untouched.
    bool    blackout_eyes = false;
};

} // namespace face
