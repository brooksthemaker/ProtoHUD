#pragma once
// ── expression_director.h ─────────────────────────────────────────────────────
// Trigger-recipe runtime for expressions — built-in slots and custom ones
// alike. Rules arrive as snapshots built from AppState (the caller controls
// locking); each rule is "activate <expression> when one of its recipes
// fires". A recipe counts events (boops, swipes, shakes, light edges) inside
// a rolling window and checks its WHILE-conditions (head tilt, light level,
// moving/still) at the moment the final event lands — so "nose boop ×5 →
// angry" and "left cheek while tilted left → curious" are both one recipe.
//
// Activation shows the rule's base face; custom expressions additionally
// apply their style through the controller's override slot (built-ins get
// their style automatically from the per-expression style resolver). The
// prior face is restored when the hold expires (hold_s == 0 latches until
// deactivate()).
//
// Threading: entry points fire from several threads (boop/light sensor
// callbacks, the render loop's tick, menu activation) — the director locks
// internally. Condition state (roll / lux / moving) is fed once per render
// tick via set_conditions.

#include <cstdint>
#include <functional>
#include <map>
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

    // One evaluatable rule: the expression to activate + its recipes.
    struct Rule {
        std::string key;               // "happy" or "custom_3" — accumulator id
        std::string name;              // display name (toast)
        std::string base_expression;   // face PNG to show
        bool            has_style = false;  // custom: apply style via override
        ExpressionStyle style;
        double          hold_s = 3.0;       // 0 = latch
        std::vector<TriggerRecipe> recipes;
    };

    void set_actions(Actions a) { act_ = std::move(a); }

    // Condition state, fed once per render tick: attitude roll (deg, +right),
    // latest ambient lux (<0 = no sensor), and whether the wearer is moving.
    void set_conditions(float roll_deg, float lux, bool moving);

    // Manual activation (menu row).
    void activate(const Rule& rule);
    void deactivate();                 // clear override + restore remembered face
    bool active() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_;
    }
    std::string active_key() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_key_;
    }

    // Hold-timer countdown + light edge detection; call once per render tick.
    void tick(double dt, const std::vector<Rule>& rules);

    // ── Event entry points ──────────────────────────────────────────────────
    // Each counts the event into matching recipes and fires the best (most
    // specific) one whose count is reached and whose conditions hold.
    // on_boop returns true when a recipe FIRED on this event — the caller
    // then suppresses the default boop reaction (intermediate counting boops
    // return false, so the normal reaction still gives per-boop feedback).
    bool on_boop(int zone, const std::vector<Rule>& rules);
    void on_gesture(const std::string& gesture, const std::vector<Rule>& rules);
    void on_shake(const std::vector<Rule>& rules);

private:
    struct Accum {                      // per (rule, recipe) counting state
        std::vector<double> stamps;     // event times (monotonic seconds)
    };

    bool conditions_hold_locked(const TriggerRecipe& r) const;
    // Count `now` into every recipe matching `match`; fire the best. Returns
    // true if one fired.
    bool feed_event_locked(const std::vector<Rule>& rules,
                           const std::function<bool(const TriggerRecipe&)>& match);
    void fire_locked(const Rule& rule);
    void deactivate_locked();
    double now_s() const;

    mutable std::mutex mtx_;
    Actions     act_;
    bool        active_ = false;
    std::string active_key_;
    double      hold_left_ = 0.0;
    bool        latched_ = false;
    std::string restore_face_;

    // Condition state (set_conditions).
    float  roll_deg_ = 0.f;
    float  lux_      = -1.f;
    bool   moving_   = false;

    // Per-recipe event accumulators, keyed "<rule.key>#<recipe idx>".
    std::map<std::string, Accum> accum_;

    // Light edge detection per recipe key: last side of the threshold
    // (+1 above / -1 below / 0 unknown), with ±10% hysteresis.
    std::map<std::string, int> light_side_;

    double clock_s_ = 0.0;             // advanced by tick(dt)
};

} // namespace face
