#include "expression_director.h"

#include <algorithm>

namespace face {

void ExpressionDirector::fire_locked(const CustomExpression& cx) {
    if (!cx.used || cx.name.empty()) return;
    // Re-trigger while already showing this expression just refreshes the
    // hold timer (debounce) instead of re-snapshotting the restore face —
    // otherwise a repeated trigger would "restore" to itself.
    if (!(active_ && active_name_ == cx.name)) {
        if (!active_ && act_.current_face) restore_face_ = act_.current_face();
        active_      = true;
        active_name_ = cx.name;
        // Order matters: set_face re-runs the controller's style resolver,
        // so the override must be applied after it to win.
        if (act_.set_face) act_.set_face(cx.base_expression);
        if (act_.set_style_override) act_.set_style_override(cx.style);
        if (act_.notify) act_.notify("Expression", cx.name);
    }
    latched_   = (cx.hold_s <= 0.0);
    hold_left_ = cx.hold_s;
}

void ExpressionDirector::activate(const CustomExpression& cx) {
    std::lock_guard<std::mutex> lk(mtx_);
    fire_locked(cx);
}

void ExpressionDirector::deactivate() {
    std::lock_guard<std::mutex> lk(mtx_);
    deactivate_locked();
}

void ExpressionDirector::deactivate_locked() {
    if (!active_) return;
    active_ = false;
    active_name_.clear();
    latched_ = false;
    hold_left_ = 0.0;
    if (act_.clear_style_override) act_.clear_style_override();
    if (!restore_face_.empty() && act_.set_face) act_.set_face(restore_face_);
    restore_face_.clear();
}

void ExpressionDirector::tick(double dt) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!active_ || latched_) return;
    hold_left_ -= dt;
    if (hold_left_ <= 0.0) deactivate_locked();
}

bool ExpressionDirector::on_boop(int zone, const std::vector<CustomExpression>& list) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Zones match the boop sensor's: 0 snout / 1 left / 2 right / 3 both
    // (fire_boop already coalesces simultaneous cheek touches into 3).
    for (const auto& cx : list) {
        if (!cx.used) continue;
        const CustomTrigger* t = cx.trigger(CustomTrigger::Type::Boop);
        if (t && t->boop_zone == zone) { fire_locked(cx); return true; }
    }
    return false;
}

void ExpressionDirector::on_gesture(const std::string& gesture,
                                    const std::vector<CustomExpression>& list) {
    if (gesture.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& cx : list) {
        if (!cx.used) continue;
        const CustomTrigger* t = cx.trigger(CustomTrigger::Type::Gesture);
        if (t && t->gesture == gesture) { fire_locked(cx); return; }
    }
}

void ExpressionDirector::on_motion(const std::string& event,
                                   const std::vector<CustomExpression>& list) {
    if (event.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& cx : list) {
        if (!cx.used) continue;
        const CustomTrigger* t = cx.trigger(CustomTrigger::Type::Motion);
        if (t && t->motion_event == event) { fire_locked(cx); return; }
    }
}

void ExpressionDirector::on_lux(float lux, const std::vector<CustomExpression>& list) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto side_of = [this](const std::string& name) -> int& {
        for (auto& [n, s] : light_side_)
            if (n == name) return s;
        light_side_.emplace_back(name, 0);
        return light_side_.back().second;
    };
    for (const auto& cx : list) {
        if (!cx.used) continue;
        const CustomTrigger* t = cx.trigger(CustomTrigger::Type::Light);
        if (!t || t->light_edge.empty()) continue;
        const float band = std::max(1.f, t->light_lux * 0.10f);   // ±10% hysteresis
        int& side = side_of(cx.name);
        int  now  = side;
        if      (lux > t->light_lux + band) now = +1;
        else if (lux < t->light_lux - band) now = -1;
        if (now == side) continue;
        const int prev = side;
        side = now;
        if (prev == 0) continue;                       // first reading arms only
        if (t->light_edge == "bright" && now == +1) fire_locked(cx);
        if (t->light_edge == "dark"   && now == -1) fire_locked(cx);
    }
}

} // namespace face
