#pragma once
// ── face_state.h ───────────────────────────────────────────────────────────────
// C++ port of protoface/state.py — the per-panel animation state passed to the
// face compositor each tick. Owns expression crossfade, the blink state machine,
// the boop override, and the inputs (audio mouth-open, gyro offset, brightness).
//
// Threading: NativeFaceController serializes all access under its own mutex, so
// FaceState carries no internal locking.

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "face_config.h"

namespace face {

class FaceState {
public:
    FaceState(const FaceCfg& cfg, std::vector<std::string> expression_names);

    // ── Expression control ──────────────────────────────────────────────────
    void set_expression(const std::string& name);
    void set_expression_by_index(int idx);   // out-of-range = no-op
    void next_expression();
    void prev_expression();
    void trigger_boop(const std::string& expression, double duration);
    void trigger_blink();

    // ── Per-frame update ─────────────────────────────────────────────────────
    void update(double dt);

    // ── Inputs (set by the controller before render) ─────────────────────────
    void set_audio(double volume, double mouth_open) { audio_volume_ = volume; mouth_open_ = mouth_open; }
    void set_gyro(double dx, double dy)              { gyro_dx_ = dx; gyro_dy_ = dy; }
    void set_brightness(int b)                       { brightness_ = b < 0 ? 0 : (b > 255 ? 255 : b); }
    // Selects which viseme overlay the FaceLoader blends at the mouth region
    // when mouth_open > 0. Default "mouth_open" matches the pre-viseme path.
    void set_mouth_shape(const std::string& s)       { if (!s.empty()) mouth_shape_ = s; }

    // ── Animation tuning (live; no rebuild required) ─────────────────────────
    // Setting blink_enabled = false freezes the blink state-machine open
    // (no eyes closed). Re-enabling resumes the next-blink countdown from
    // the next call to update().
    void set_blink_enabled(bool en)                  { blink_enabled_ = en; if (!en) { blink_phase_ = BlinkPhase::Open; blink_weight_ = 0.0; } }
    // Hold the eyes fully shut (asleep). Pins the blink at full weight — the
    // closed-eye look comes from the blink art, so it works even without a
    // dedicated closed-eye expression PNG. Clearing resumes normal blinking.
    void set_eyes_closed(bool closed)                { eyes_closed_ = closed; if (closed) { blink_phase_ = BlinkPhase::Closed; blink_weight_ = 1.0; } }
    bool eyes_closed() const { return eyes_closed_; }
    void set_blink_timing(double min_s, double max_s, double duration_s) {
        blink_min_      = std::max(0.1, min_s);
        blink_max_      = std::max(blink_min_, max_s);
        blink_duration_ = std::max(0.02, duration_s);
    }
    void set_expression_fade(double seconds)         { fade_speed_ = 1.0 / std::max(0.01, seconds); }
    void set_wiggle(const WiggleCfg& w)              { wiggle_ = w; }
    bool blink_enabled()  const { return blink_enabled_; }
    double blink_min()    const { return blink_min_; }
    double blink_max()    const { return blink_max_; }
    double blink_duration() const { return blink_duration_; }
    double expression_fade_s() const { return fade_speed_ > 0 ? (1.0 / fade_speed_) : 0.3; }

    // ── Read accessors for the compositor (mirror face.py's state fields) ─────
    const std::string& expression()      const { return expression_; }
    const std::string& prev_expression() const { return prev_expression_; }
    double transition_t() const { return transition_t_; }
    double blink_weight() const { return blink_weight_; }
    double mouth_open()   const { return mouth_open_; }
    const std::string& mouth_shape() const { return mouth_shape_; }
    double gyro_dx()      const { return gyro_dx_; }
    double gyro_dy()      const { return gyro_dy_; }
    double time()         const { return time_; }
    int    brightness()   const { return brightness_; }
    const WiggleCfg& wiggle() const { return wiggle_; }
    const std::vector<std::string>& expression_names() const { return expressions_; }

private:
    void update_blink(double dt);
    double rand_uniform(double lo, double hi);

    std::vector<std::string> expressions_;
    int    expr_idx_ = 0;

    std::string expression_;
    std::string prev_expression_;
    double transition_t_ = 1.0;
    double fade_speed_   = 1.0 / 0.3;

    // Boop override
    double      boop_remaining_ = 0.0;
    std::string boop_prev_      = "neutral";

    // Blink state machine
    enum class BlinkPhase { Open, Closing, Closed, Opening };
    BlinkPhase blink_phase_ = BlinkPhase::Open;
    double blink_weight_   = 0.0;
    double blink_t_        = 0.0;
    double blink_duration_ = 0.15;
    double blink_min_      = 3.0;
    double blink_max_      = 7.0;
    double next_blink_     = 3.0;
    bool   blink_enabled_  = true;
    bool   eyes_closed_    = false;   // asleep: hold the blink fully shut

    WiggleCfg wiggle_;

    // Inputs
    double audio_volume_ = 0.0;
    double mouth_open_   = 0.0;
    std::string mouth_shape_ = "mouth_open";   // viseme overlay name
    double gyro_dx_      = 0.0;
    double gyro_dy_      = 0.0;

    double time_       = 0.0;
    int    brightness_ = 255;

    std::mt19937 rng_;
};

} // namespace face
