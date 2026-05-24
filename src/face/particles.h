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
    void update(double dt);
    ParticleFrame render();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace face
