#include "reaction_engine.h"

#include <algorithm>
#include <cmath>

namespace face {

nlohmann::json ReactionEngine::Config::to_json() const {
    return {
        {"enabled", enabled},
        {"sleepy", {
            {"enabled",        sleepy_enabled},
            {"drowsy_after_s", drowsy_after_s},
            {"sleep_after_s",  sleep_after_s},
            {"calm_dps",       calm_dps},
            {"wake_dps",       wake_dps},
            {"wake_flash_s",   wake_flash_s},
        }},
    };
}

ReactionEngine::Config ReactionEngine::Config::from_json(const nlohmann::json& j) {
    Config c;
    c.enabled = j.value("enabled", c.enabled);
    if (j.contains("sleepy") && j["sleepy"].is_object()) {
        const auto& s = j["sleepy"];
        c.sleepy_enabled = s.value("enabled",        c.sleepy_enabled);
        c.drowsy_after_s = s.value("drowsy_after_s", c.drowsy_after_s);
        c.sleep_after_s  = s.value("sleep_after_s",  c.sleep_after_s);
        c.calm_dps       = s.value("calm_dps",       c.calm_dps);
        c.wake_dps       = s.value("wake_dps",       c.wake_dps);
        c.wake_flash_s   = s.value("wake_flash_s",   c.wake_flash_s);
    }
    return c;
}

void ReactionEngine::set_config(const Config& c) {
    cfg_ = c;
    if ((!cfg_.enabled || !cfg_.sleepy_enabled) &&
        activity_ != Activity::Awake)
        wake(false);   // disabling the feature restores the face quietly
}

void ReactionEngine::feed_motion(double gyro_mag_dps, double accel_dev_g) {
    // One scalar "how much is the head moving": gyro magnitude plus the
    // acceleration deviation from resting gravity, weighted so a firm nod
    // (~0.15 g) counts like a moderate turn (~35 deg/s).
    instant_ = std::fabs(gyro_mag_dps) + std::fabs(accel_dev_g) * 240.0;
}

const char* ReactionEngine::activity_name() const {
    switch (activity_) {
    case Activity::Drowsy: return "drowsy";
    case Activity::Asleep: return "asleep";
    default:               return "awake";
    }
}

void ReactionEngine::tick(double dt) {
    if (dt <= 0.0) return;
    dt = std::min(dt, 0.25);

    // Smoothed energy (tau ~2 s) for the stillness timer; the raw instant
    // value handles the wake spike so a sharp nudge snaps the face awake.
    energy_ += (instant_ - energy_) * std::min(1.0, dt / 2.0);

    // Edge-detected motion spike for the custom-expression director — fires
    // once per shake regardless of the sleepy state machine, re-arming after
    // the motion settles below half the spike threshold.
    if (instant_ > cfg_.wake_dps) {
        if (spike_armed_ && act_.motion_event) act_.motion_event("shake");
        spike_armed_ = false;
    } else if (instant_ < cfg_.wake_dps * 0.5) {
        spike_armed_ = true;
    }

    if (!cfg_.enabled || !cfg_.sleepy_enabled) return;

    if (instant_ > cfg_.wake_dps) {
        still_s_ = 0.0;
        if (activity_ != Activity::Awake) wake(true);
        return;
    }

    still_s_ = (energy_ < cfg_.calm_dps) ? still_s_ + dt : 0.0;

    switch (activity_) {
    case Activity::Awake:
        if (still_s_ >= cfg_.drowsy_after_s) enter_drowsy();
        break;
    case Activity::Drowsy:
        if (still_s_ >= cfg_.sleep_after_s) enter_asleep();
        else if (still_s_ <= 0.0) wake(false);   // gentle motion: no flash
        break;
    case Activity::Asleep:
        break;   // only the wake spike (above) leaves sleep
    }
}

void ReactionEngine::enter_drowsy() {
    activity_ = Activity::Drowsy;
    if (act_.motion_event) act_.motion_event("still");
    if (act_.current_expression) prev_expression_ = act_.current_expression();
    // Heavy lids: long, slow blinks.
    if (act_.set_blink) act_.set_blink(1.5, 3.0, 0.45);
    if (act_.face_exists && act_.set_expression &&
        act_.face_exists("sleepy"))
        act_.set_expression("sleepy");
}

void ReactionEngine::enter_asleep() {
    activity_ = Activity::Asleep;
    // Eyes closed: prefer a dedicated asleep face, fall back to sleepy.
    if (act_.face_exists && act_.set_expression) {
        if      (act_.face_exists("asleep")) act_.set_expression("asleep");
        else if (act_.face_exists("sleepy")) act_.set_expression("sleepy");
    }
    // Floating Z's over whatever the face shows, via the override slot.
    override_active_ = true;
    if (act_.set_ambient)
        act_.set_ambient({{"layers", {{
            {"effect", "snooze"}, {"count", 3}, {"blend", "add"},
        }}}});
    if (act_.notify) act_.notify("Face", "Zzz...");
}

void ReactionEngine::wake(bool flash) {
    const bool was_asleep = (activity_ == Activity::Asleep);
    activity_ = Activity::Awake;
    still_s_  = 0.0;
    if (act_.restore_blink) act_.restore_blink();
    if (override_active_) {
        override_active_ = false;
        if (act_.set_ambient) act_.set_ambient(base_ambient_);
    }
    if (act_.set_expression && !prev_expression_.empty())
        act_.set_expression(prev_expression_);
    prev_expression_.clear();
    if (flash && was_asleep && act_.flash_expression &&
        (!act_.face_exists || act_.face_exists("surprised")))
        act_.flash_expression("surprised", cfg_.wake_flash_s);
}

void ReactionEngine::set_base_ambient(const nlohmann::json& spec) {
    base_ambient_ = spec;
    if (!override_active_ && act_.set_ambient) act_.set_ambient(spec);
}

void ReactionEngine::force_sleepy() {
    if (activity_ == Activity::Awake) enter_drowsy();
    if (activity_ == Activity::Drowsy) enter_asleep();
}

void ReactionEngine::force_wake() {
    if (activity_ != Activity::Awake) wake(true);
}

}  // namespace face
