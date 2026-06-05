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
};

class ParticleSystem {
public:
    // cfg accepts any form _resolve_cfg understands: a string effect name,
    // {"preset": "..."}, {"effect": ..., ...}, or {"layers": [...]}.
    ParticleSystem(int width, int height, const nlohmann::json& cfg);
    ~ParticleSystem();

    void set_effect(const nlohmann::json& cfg);   // replace all layers at runtime
    void set_motion(const MotionInput& m);        // latest IMU state for reactive layers
    void set_audio(double level);                 // mic level [0,1] for audio-reactive layers
    void update(double dt);
    ParticleFrame render();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace face
