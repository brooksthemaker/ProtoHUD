#pragma once
// ── glitch.h ───────────────────────────────────────────────────────────────────
// CPU "glitch" post-effect for the native face. ONE overall effect whose
// individual looks are exposed as independent 0..1 variables, wrapped by a
// master `intensity` and a burst (on/off) envelope so the corruption stutters
// in bursts rather than running flat.
//
// Most components run frame-level on the composited RGB canvas (chromatic
// split, band tearing, block shuffle, bitcrush, dropout bars, datamosh smear,
// eyes/mouth region desync). One component — expression flicker — can't be
// faked from pixels, so tick() raises a flag the controller reads at the face
// stage to flash a different expression image for that frame.
//
// Threading: a single GlitchEffect instance is owned and driven by the
// NativeFaceController render thread; only its config is set from other
// threads (copied under the controller's mutex).

#include <cstdint>
#include <random>

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

namespace face {

struct GlitchConfig {
    bool   enabled    = false;
    double intensity  = 1.0;   // master scale over every component (0..2)

    // Burst timing. burst_rate <= 0 => always-on (constant glitch).
    double burst_rate = 0.4;   // average bursts per second
    double burst_min  = 0.08;  // burst length seconds
    double burst_max  = 0.30;

    // Per-component amounts (0 = off, 1 = full).
    double chromatic     = 0.5;  // RGB channel split / chromatic aberration
    double tearing       = 0.6;  // horizontal band displacement (VHS tracking)
    double blocks        = 0.3;  // rectangular block shuffle (macroblock rot)
    double bitcrush      = 0.0;  // colour-depth posterise
    double dropout       = 0.0;  // signal-loss bars (black / static)
    double datamosh      = 0.0;  // frame freeze / smear (ghost of prev frame)
    double region_desync = 0.3;  // eyes(top) vs mouth(bottom) band slip
    double expr_flicker  = 0.0;  // chance to flash a wrong expression

    nlohmann::json to_json() const;
    static GlitchConfig from_json(const nlohmann::json& j);
};

class GlitchEffect {
public:
    GlitchEffect();

    // Advance the burst envelope once per frame (call before the panel loop).
    void tick(double dt, const GlitchConfig& cfg);

    double envelope() const { return env_; }      // 0..1 current burst strength
    bool   active()   const { return env_ > 1e-3; }

    // True this frame when a wrong-expression flash should be shown (sampled in
    // tick from cfg.expr_flicker). flicker_pick() is a stable [0,1) selector so
    // every panel flashes the SAME alternate expression in a given frame.
    bool   flicker_expr() const { return flicker_expr_; }
    double flicker_pick() const { return flicker_pick_; }

    // Apply the frame-level glitch to an 8UC3 RGB image, in place.
    void apply(cv::Mat& rgb, const GlitchConfig& cfg);

private:
    double rnd(double lo, double hi);

    std::mt19937 rng_;
    double env_          = 0.0;   // current burst envelope 0..1
    double burst_left_   = 0.0;   // seconds remaining in the active burst
    double burst_len_    = 0.0;   // total length of the active burst
    double next_in_      = 0.0;   // seconds until the next burst starts
    bool   flicker_expr_ = false;
    double flicker_pick_ = 0.0;
    cv::Mat prev_;                // previous clean frame, for datamosh smear
};

} // namespace face
