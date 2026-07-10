#pragma once
// ── particles.h ────────────────────────────────────────────────────────────────
// C++ port of protoface/particles.py — a multi-layer particle compositor. The
// effect classes and presets live in the .cpp (pImpl) so this header stays light.
// render() returns one composited RGBA layer + its blend mode, ready to drop into
// renderer.h composite() alongside the face.

#include <memory>
#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

#include "renderer.h"   // Blend

namespace face {

// Per-frame motion state fed in from the IMU so effects can react to head
// movement (drift with yaw, "gravity" with tilt, density with acceleration).
// Opt-in per layer via the cfg keys "direction_from"/"intensity_from".
struct MotionInput {
    double heading_deg = 0.0;   // absolute compass heading
    double yaw_rate    = 0.0;   // deg/s about vertical (+ = turning right)
    double pitch_deg   = 0.0;   // head tilt fwd(+)/back(-)
    double roll_deg    = 0.0;   // head tilt right(+)/left(-)
    double accel_g     = 1.0;   // total linear accel magnitude (≈1.0 at rest)
};

struct ParticleFrame {
    bool    has   = false;       // false when there are no active layers
    cv::Mat rgba;                // CV_8UC4 (RGBA), valid when has
    Blend   blend = Blend::Add;  // overall blend hint (add unless a layer is normal)
    // Max "refraction" hint across layers (water): how strongly the backdrop
    // face should glow back through the layer. 0 for ordinary effects.
    double  face_glow = 0.0;
};

// Combine two particle specs into one layered spec — `base`'s layers first,
// then `over`'s on top. Either arg may be any form set_effect understands
// (string, {"preset":..}, {"effect":..}, {"layers":[..]}); presets are
// resolved. Used by the per-expression "overlay" effect mode. Returns "none"
// when both resolve to nothing.
nlohmann::json merge_effect_specs(const nlohmann::json& base,
                                  const nlohmann::json& over);

class ParticleSystem {
public:
    // cfg accepts any form _resolve_cfg understands: a string effect name,
    // {"preset": "..."}, {"effect": ..., ...}, or {"layers": [...]}.
    ParticleSystem(int width, int height, const nlohmann::json& cfg);
    ~ParticleSystem();

    void set_effect(const nlohmann::json& cfg);   // replace all layers at runtime
    void set_motion(const MotionInput& m);        // latest IMU state for reactive layers
    void set_audio(double level);                 // mic level [0,1] for audio-reactive layers
    void set_humidity(double humidity01);         // rel humidity [0,1] for the water fill level (<0 = no reading)
    // Global motion coupling: when on, directional layers that don't set their
    // own "direction_from" default to the real-gravity mode — precipitation
    // leans with head roll and sweeps on quick turns. Toggled from the menu.
    void set_motion_reactive(bool on);
    // One-shot expanding ring (boop feedback), centred at canvas-normalised
    // coordinates so a multi-panel face stays continuous. Drawn over the
    // running effect (or on an empty layer when no effect is active).
    void trigger_ripple(double cx_norm, double cy_norm,
                        uint8_t r = 235, uint8_t g = 245, uint8_t b = 255);
    // Where this panel sits in the full logical canvas, so canvas-space effects
    // (e.g. water) render one continuous field across a multi-panel face. Local
    // pixel (lx,ly) maps to canvas (off_x+lx, off_y+ly). Defaults to per-panel.
    void set_canvas_geometry(int canvas_w, int canvas_h, int off_x, int off_y);
    void update(double dt);
    ParticleFrame render();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace face
