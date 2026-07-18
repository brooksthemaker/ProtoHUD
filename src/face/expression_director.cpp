#include "expression_director.h"

#include <algorithm>

namespace face {

// Time base: advanced by tick(dt) on the render thread (60 Hz), read by the
// sensor-thread event entries under the mutex. Event stamps therefore have
// frame-level (~16 ms) resolution — plenty for multi-second recipe windows —
// and harnesses control time deterministically through tick().
double ExpressionDirector::now_s() const { return clock_s_; }

void ExpressionDirector::set_conditions(float roll_deg, float lux, bool moving) {
    std::lock_guard<std::mutex> lk(mtx_);
    roll_deg_ = roll_deg;
    lux_      = lux;
    moving_   = moving;
}

bool ExpressionDirector::conditions_hold_locked(const TriggerRecipe& r) const {
    switch (r.tilt) {
        case TriggerRecipe::Tilt::Left:  if (!(roll_deg_ < -r.tilt_deg)) return false; break;
        case TriggerRecipe::Tilt::Right: if (!(roll_deg_ >  r.tilt_deg)) return false; break;
        case TriggerRecipe::Tilt::Any:   break;
    }
    switch (r.light) {
        case TriggerRecipe::Light::Bright:
            if (!(lux_ >= 0.f && lux_ > r.light_lux)) return false;
            break;
        case TriggerRecipe::Light::Dark:
            if (!(lux_ >= 0.f && lux_ < r.light_lux)) return false;
            break;
        case TriggerRecipe::Light::Any: break;
    }
    switch (r.motion) {
        case TriggerRecipe::Motion::Moving: if (!moving_) return false; break;
        case TriggerRecipe::Motion::Still:  if (moving_)  return false; break;
        case TriggerRecipe::Motion::Any:    break;
    }
    return true;
}

void ExpressionDirector::fire_locked(const Rule& rule) {
    if (rule.is_eye_anim) {
        // Transient by nature: the animation takes over the panels for its
        // own duration and reverts by itself, so none of the activation /
        // restore-face bookkeeping below applies. A retrigger just replays.
        if (act_.play_eyes) act_.play_eyes(rule.eyes);
        if (act_.notify && !rule.name.empty()) act_.notify("Expression", rule.name);
        return;
    }
    if (rule.base_expression.empty() && rule.name.empty()) return;
    const bool retrigger = active_ && active_key_ == rule.key;
    if (!retrigger) {
        // Remember where to return to — but never a face we put up ourselves.
        if (!active_ && act_.current_face) restore_face_ = act_.current_face();
        active_     = true;
        active_key_ = rule.key;
        if (act_.set_face && !rule.base_expression.empty())
            act_.set_face(rule.base_expression);
        // Custom expressions carry their style in the override slot (applied
        // AFTER set_face — the face switch re-runs the style resolver).
        // Built-ins get their style from the resolver itself.
        if (rule.has_style) {
            if (act_.set_style_override) act_.set_style_override(rule.style);
        } else if (act_.clear_style_override) {
            act_.clear_style_override();
        }
        if (act_.notify && !rule.name.empty()) act_.notify("Expression", rule.name);
    }
    latched_   = (rule.hold_s <= 0.0);
    hold_left_ = rule.hold_s;
}

void ExpressionDirector::activate(const Rule& rule) {
    std::lock_guard<std::mutex> lk(mtx_);
    fire_locked(rule);
}

void ExpressionDirector::deactivate() {
    std::lock_guard<std::mutex> lk(mtx_);
    deactivate_locked();
}

void ExpressionDirector::deactivate_locked() {
    if (!active_) return;
    active_ = false;
    active_key_.clear();
    latched_ = false;
    hold_left_ = 0.0;
    if (act_.clear_style_override) act_.clear_style_override();
    if (!restore_face_.empty() && act_.set_face) act_.set_face(restore_face_);
    restore_face_.clear();
}

bool ExpressionDirector::feed_event_locked(
        const std::vector<Rule>& rules,
        const std::function<bool(const TriggerRecipe&)>& match) {
    const double now = now_s();
    const Rule* best_rule = nullptr;
    const TriggerRecipe* best_recipe = nullptr;
    int best_spec = -1;

    for (const auto& rule : rules) {
        for (size_t ri = 0; ri < rule.recipes.size(); ++ri) {
            const TriggerRecipe& r = rule.recipes[ri];
            if (!r.armed() || !match(r)) continue;
            auto& acc = accum_[rule.key + "#" + std::to_string(ri)];
            // Count this event, pruning stamps that fell out of the window.
            acc.stamps.push_back(now);
            const double w = std::max(0.1, static_cast<double>(r.window_s));
            acc.stamps.erase(
                std::remove_if(acc.stamps.begin(), acc.stamps.end(),
                               [&](double t) { return now - t > w; }),
                acc.stamps.end());
            if (static_cast<int>(acc.stamps.size()) >= r.count &&
                conditions_hold_locked(r) && r.specificity() > best_spec) {
                best_spec   = r.specificity();
                best_rule   = &rule;
                best_recipe = &r;
            }
        }
    }
    if (!best_rule) return false;
    // Consume the winner's count so the next fire needs fresh events.
    for (size_t ri = 0; ri < best_rule->recipes.size(); ++ri)
        if (&best_rule->recipes[ri] == best_recipe)
            accum_[best_rule->key + "#" + std::to_string(ri)].stamps.clear();
    fire_locked(*best_rule);
    return true;
}

bool ExpressionDirector::on_boop(int zone, const std::vector<Rule>& rules) {
    std::lock_guard<std::mutex> lk(mtx_);
    return feed_event_locked(rules, [zone](const TriggerRecipe& r) {
        return r.event == TriggerRecipe::Event::Boop && r.boop_zone == zone;
    });
}

void ExpressionDirector::on_gesture(const std::string& gesture,
                                    const std::vector<Rule>& rules) {
    if (gesture.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    feed_event_locked(rules, [&gesture](const TriggerRecipe& r) {
        return r.event == TriggerRecipe::Event::Gesture && r.gesture == gesture;
    });
}

void ExpressionDirector::on_shake(const std::vector<Rule>& rules) {
    std::lock_guard<std::mutex> lk(mtx_);
    feed_event_locked(rules, [](const TriggerRecipe& r) {
        return r.event == TriggerRecipe::Event::Shake;
    });
}

void ExpressionDirector::tick(double dt, const std::vector<Rule>& rules) {
    std::lock_guard<std::mutex> lk(mtx_);
    clock_s_ += std::max(0.0, dt);

    // Light edge events, derived from the fed lux with ±10% hysteresis so a
    // hovering reading doesn't machine-gun. Edges count like any event.
    if (lux_ >= 0.f) {
        for (const auto& rule : rules) {
            for (size_t ri = 0; ri < rule.recipes.size(); ++ri) {
                const TriggerRecipe& r = rule.recipes[ri];
                const bool bright_ev = (r.event == TriggerRecipe::Event::LightBright);
                if (!bright_ev && r.event != TriggerRecipe::Event::LightDark) continue;
                const std::string k = rule.key + "#" + std::to_string(ri);
                const float band = std::max(1.f, r.light_lux * 0.10f);
                int& side = light_side_[k];
                int  now_side = side;
                if      (lux_ > r.light_lux + band) now_side = +1;
                else if (lux_ < r.light_lux - band) now_side = -1;
                if (now_side == side) continue;
                const int prev = side;
                side = now_side;
                if (prev == 0) continue;               // first reading arms only
                const bool fired_edge = (bright_ev && now_side == +1) ||
                                        (!bright_ev && now_side == -1);
                if (!fired_edge) continue;
                // Route through the shared counting path (recipes may want
                // e.g. "gets dark ×2").
                const TriggerRecipe* target = &r;
                feed_event_locked(rules, [target](const TriggerRecipe& rr) {
                    return &rr == target;
                });
            }
        }
    }

    if (!active_ || latched_) return;
    hold_left_ -= dt;
    if (hold_left_ <= 0.0) deactivate_locked();
}

} // namespace face
