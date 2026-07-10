#pragma once
// ── reaction_engine.h ─────────────────────────────────────────────────────────
// Environment/movement reactions for the face. v1 owns the activity state
// machine (Awake → Drowsy → Asleep on stillness, snap awake on motion) and
// the ambient-override ladder: weather/temp ambient specs route through
// set_base_ambient(), and while a reaction override (the snooze Z's) is
// active the base is held and restored afterwards — so reactions, Weather
// Sync and Temp Effects can't fight over the controller's single slot.
//
// Threading: everything (feed_motion, tick, set_base_ambient, menu edits,
// force_*) runs on the render thread. No internal locking.
//
// Actions are injected from main so the engine stays free of controller
// headers and testable off-target.

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace face {

struct ReactionActions {
    // Does the active face folder have this expression PNG?
    std::function<bool(const std::string&)> face_exists;
    // Persistent expression switch / read-back (set_face_by_name path).
    std::function<void(const std::string&)> set_expression;
    std::function<std::string()>            current_expression;
    // Transient flash that auto-restores (trigger_boop path).
    std::function<void(const std::string&, double)> flash_expression;
    // Blink cadence override + restore-to-user-config (drowsy = heavy lids).
    std::function<void(double min_s, double max_s, double duration_s)> set_blink;
    std::function<void()>                   restore_blink;
    // Hold the eyes fully shut (asleep) / release them on wake. Optional.
    std::function<void(bool)>               set_eyes_closed;
    // Final ambient sink (native_ctrl->set_ambient_effect).
    std::function<void(const nlohmann::json&)> set_ambient;
    // HUD toast.
    std::function<void(const std::string&, const std::string&)> notify;
    // Motion-event hook for the custom-expression director: "shake" on a
    // wake spike, "still" on entering drowsy. Optional.
    std::function<void(const char*)> motion_event;
};

class ReactionEngine {
public:
    struct Config {
        bool   enabled        = true;
        bool   sleepy_enabled = true;
        double drowsy_after_s = 120.0;  // stillness before heavy lids
        double sleep_after_s  = 300.0;  // stillness before eyes close + Z's
        // Convenience macro (0..1, 0.5 = neutral) scaling the two timers: a
        // higher value nods off sooner, lower keeps it awake longer. Applied
        // on top of the raw seconds above so the sliders still fine-tune.
        double sensitivity    = 0.5;
        double calm_dps       = 6.0;    // below this smoothed energy = "still"
        double wake_dps       = 45.0;   // instant energy spike that wakes
        double wake_flash_s   = 1.5;    // surprised flash on wake

        nlohmann::json to_json() const;
        static Config from_json(const nlohmann::json& j);
    };

    enum class Activity { Awake, Drowsy, Asleep };

    void set_actions(ReactionActions a) { act_ = std::move(a); }
    void set_config(const Config& c);
    const Config& config() const { return cfg_; }

    // Raw (unscaled) motion for this frame: gyro magnitude in deg/s and
    // |accel - 1 g| in g. Called from the IMU feed each frame.
    void feed_motion(double gyro_mag_dps, double accel_dev_g);
    // Advance timers / state machine; fires actions on transitions.
    void tick(double dt);

    // Weather Sync / Temp Effects route their ambient spec here instead of
    // the controller. Forwarded immediately unless a reaction override is
    // active, in which case it's stored and restored on wake.
    void set_base_ambient(const nlohmann::json& spec);

    // Menu: current state label ("awake" / "drowsy" / "asleep") + test hooks.
    Activity    activity() const { return activity_; }
    const char* activity_name() const;
    double      energy_dps() const { return energy_; }
    void        force_sleepy();   // jump straight to Asleep (test)
    void        force_wake();     // as if a motion spike arrived (test)

private:
    bool spike_armed_ = true;   // motion-spike edge detector (see tick)
    void enter_drowsy();
    void enter_asleep();
    void wake(bool flash);

    ReactionActions act_;
    Config          cfg_;

    double   energy_       = 0.0;    // EMA of motion energy (deg/s-equivalent)
    double   instant_      = 0.0;    // this frame's raw energy
    double   still_s_      = 0.0;    // continuous time below calm_dps
    Activity activity_     = Activity::Awake;

    std::string    prev_expression_;     // restored on wake
    nlohmann::json base_ambient_;        // weather/temp result
    bool           override_active_ = false;
};

}  // namespace face
