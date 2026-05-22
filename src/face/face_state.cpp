#include "face_state.h"

#include <algorithm>
#include <chrono>

namespace face {

FaceState::FaceState(const FaceCfg& cfg, std::vector<std::string> expression_names)
    : expressions_(std::move(expression_names)),
      rng_(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())) {
    expression_      = expressions_.empty() ? "neutral" : expressions_.front();
    prev_expression_ = expression_;
    transition_t_    = 1.0;

    fade_speed_      = 1.0 / std::max(0.01, cfg.expression_fade);

    blink_duration_  = cfg.blink.duration;
    blink_min_       = cfg.blink.interval_min;
    blink_max_       = cfg.blink.interval_max;
    next_blink_      = rand_uniform(blink_min_, blink_max_);

    wiggle_          = cfg.wiggle;
}

double FaceState::rand_uniform(double lo, double hi) {
    std::uniform_real_distribution<double> d(lo, hi);
    return d(rng_);
}

// ── Expression control ──────────────────────────────────────────────────────

void FaceState::set_expression(const std::string& name) {
    if (name == expression_) return;
    prev_expression_ = expression_;
    expression_      = name;
    transition_t_    = 0.0;
}

void FaceState::set_expression_by_index(int idx) {
    if (idx >= 0 && idx < static_cast<int>(expressions_.size())) {
        expr_idx_ = idx;
        set_expression(expressions_[idx]);
    }
}

void FaceState::next_expression() {
    if (expressions_.empty()) return;
    expr_idx_ = (expr_idx_ + 1) % static_cast<int>(expressions_.size());
    set_expression(expressions_[expr_idx_]);
}

void FaceState::prev_expression() {
    if (expressions_.empty()) return;
    int n = static_cast<int>(expressions_.size());
    expr_idx_ = (expr_idx_ - 1 + n) % n;
    set_expression(expressions_[expr_idx_]);
}

void FaceState::trigger_boop(const std::string& expression, double duration) {
    if (boop_remaining_ <= 0.0) boop_prev_ = expression_;
    boop_remaining_ = duration;
    set_expression(expression);
}

void FaceState::trigger_blink() {
    if (blink_phase_ == BlinkPhase::Open) {
        blink_phase_ = BlinkPhase::Closing;
        blink_t_     = 0.0;
    }
}

// ── Per-frame update ─────────────────────────────────────────────────────────

void FaceState::update(double dt) {
    time_ += dt;

    if (transition_t_ < 1.0)
        transition_t_ = std::min(1.0, transition_t_ + dt * fade_speed_);

    if (boop_remaining_ > 0.0) {
        boop_remaining_ -= dt;
        if (boop_remaining_ <= 0.0) set_expression(boop_prev_);
    }

    update_blink(dt);
}

void FaceState::update_blink(double dt) {
    const double half = blink_duration_ / 2.0;

    switch (blink_phase_) {
    case BlinkPhase::Open:
        next_blink_ -= dt;
        if (next_blink_ <= 0.0) { blink_phase_ = BlinkPhase::Closing; blink_t_ = 0.0; }
        break;
    case BlinkPhase::Closing:
        blink_t_ += dt;
        blink_weight_ = std::min(1.0, blink_t_ / half);
        if (blink_t_ >= half) { blink_phase_ = BlinkPhase::Closed; blink_t_ = 0.0; }
        break;
    case BlinkPhase::Closed:
        blink_t_ += dt;
        if (blink_t_ >= 0.04) { blink_phase_ = BlinkPhase::Opening; blink_t_ = 0.0; }
        break;
    case BlinkPhase::Opening:
        blink_t_ += dt;
        blink_weight_ = std::max(0.0, 1.0 - blink_t_ / half);
        if (blink_t_ >= half) {
            blink_weight_ = 0.0;
            blink_phase_  = BlinkPhase::Open;
            next_blink_   = rand_uniform(blink_min_, blink_max_);
        }
        break;
    }
}

} // namespace face
