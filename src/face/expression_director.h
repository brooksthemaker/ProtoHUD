#pragma once
// ── expression_director.h ─────────────────────────────────────────────────────
// Runtime activation for user-created custom expressions: watches the trigger
// sources (boop zones, APDS-9960 gestures, ReactionEngine motion events, the
// ambient light sensor, manual menu activation), and while a custom
// expression is active shows its base face + style override, restoring the
// prior face and default style when the hold expires (hold_s == 0 latches
// until deactivate()).
//
// Same pattern as ReactionEngine: actions are injected from main so the
// director stays free of controller headers and testable off-target. Entry
// points fire from several threads (boop/light sensor callbacks, the render
// loop's tick, menu activation), so the director locks internally. Trigger
// entry points take a snapshot of the custom-expression list so the caller
// controls locking of the live AppState vector.

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "custom_expression.h"

namespace face {

class ExpressionDirector {
public:
    struct Actions {
        std::function<void(const std::string&)>     set_face;            // persistent switch
        std::function<std::string()>                current_face;
        std::function<void(const ExpressionStyle&)> set_style_override;
        std::function<void()>                       clear_style_override;
        // Optional HUD toast (title, body).
        std::function<void(const std::string&, const std::string&)> notify;
    };

    void set_actions(Actions a) { act_ = std::move(a); }

    // Manual activation (menu row) — also the tail of every trigger.
    void activate(const CustomExpression& cx);
    // Clear the override and restore the remembered face.
    void deactivate();
    bool active() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_;
    }
    std::string active_name() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_name_;
    }

    // Hold-timer countdown; call once per render tick.
    void tick(double dt);

    // ── Trigger entry points ────────────────────────────────────────────────
    // Each scans the snapshot for a used expression with a matching trigger.
    // on_boop returns true when a custom expression claimed the zone — the
    // caller then suppresses the default boop reaction for that event.
    bool on_boop(int zone, const std::vector<CustomExpression>& list);
    void on_gesture(const std::string& gesture, const std::vector<CustomExpression>& list);
    void on_motion(const std::string& event, const std::vector<CustomExpression>& list);
    // Ambient light sample (lux). Edge-triggered per expression with ±10%
    // hysteresis around each trigger's threshold so a hovering reading
    // doesn't machine-gun activations.
    void on_lux(float lux, const std::vector<CustomExpression>& list);

private:
    void fire_locked(const CustomExpression& cx);
    void deactivate_locked();

    mutable std::mutex mtx_;
    Actions     act_;
    bool        active_ = false;
    std::string active_name_;
    double      hold_left_ = 0.0;      // <= 0 while latched (hold_s == 0)
    bool        latched_ = false;
    std::string restore_face_;

    // Light hysteresis: name → last side of the threshold (+1 above, -1
    // below, 0 unknown). Re-arms only after crossing back past the band.
    std::vector<std::pair<std::string, int>> light_side_;
};

} // namespace face
