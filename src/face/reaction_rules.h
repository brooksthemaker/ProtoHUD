#pragma once
// ── reaction_rules.h ──────────────────────────────────────────────────────────
// Named environmental reactions for the face — the "Reactions" list under
// Face Display. Each reaction is a purpose-built trigger (move into bright /
// dark, dizzy from spinning, low battery, loud noise, upside-down …) with an
// enable flag, a tunable threshold, a hold time, and an OUTCOME that shows
// either an expression or a GIF.
//
// This complements two neighbours: the sleep state machine
// (ReactionEngine — Falling Asleep) and the per-expression trigger recipes
// (ExpressionDirector — "what activates THIS expression"). ReactionRules is
// the top-down list: "when X happens in the world, show Y".
//
// Signals (light lux, spin rate, roll, battery %, mic level) are fed once per
// render tick via set_signals(); tick() edge-detects each enabled reaction and
// fires its outcome, restoring the prior face when an expression outcome's
// hold expires (GIF outcomes auto-revert on their own timer). Actions are
// injected from main so the engine stays controller-free and testable.
//
// Threading: entry points may be called from the render thread and the menu
// thread, so the engine locks internally.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace face {

struct ReactionOutcome {
    enum class Kind : uint8_t { Expression = 0, Gif = 1 };
    Kind        kind = Kind::Expression;
    std::string expression = "surprised";   // Kind::Expression
    int         gif_slot   = 0;              // Kind::Gif (0-based slot)

    nlohmann::json to_json() const;
    static ReactionOutcome from_json(const nlohmann::json& j);
};

struct ReactionRule {
    bool           enabled   = false;
    float          threshold = 0.f;   // meaning depends on the reaction id
    double         hold_s    = 3.0;   // 0 = latch until another reaction/face
    ReactionOutcome outcome;

    nlohmann::json to_json() const;
    static ReactionRule from_json(const nlohmann::json& j);
};

class ReactionRules {
public:
    struct Signals {
        float lux         = -1.f;   // ambient light (<0 = no sensor)
        float spin_dps    = 0.f;    // |yaw rate|, deg/s
        float roll_deg    = 0.f;    // head roll (attitude)
        int   battery_pct = -1;     // <0 = unknown
        float mic         = 0.f;    // mic level 0..1
    };
    struct Actions {
        std::function<void(const std::string&)> set_face;      // persistent switch
        std::function<std::string()>            current_face;
        std::function<void(int)>                play_gif;      // 0-based slot
        std::function<void(const std::string&, const std::string&)> notify;
    };

    // Built-in reaction ids, in menu order, with labels + threshold metadata.
    struct Meta {
        const char* id;
        const char* label;
        const char* desc;
        float       def_threshold;
        float       thr_min, thr_max, thr_step;
        const char* thr_unit;              // slider suffix
        const char* def_expression;        // default outcome expression
    };
    static const std::vector<Meta>& metas();
    static const Meta* meta(const std::string& id);

    void set_actions(Actions a) { std::lock_guard<std::mutex> lk(mtx_); act_ = std::move(a); }
    void set_signals(const Signals& s);
    void tick(double dt);

    // Menu access (seeds any missing reaction from its meta defaults).
    ReactionRule rule(const std::string& id) const;
    void         set_rule(const std::string& id, const ReactionRule& r);
    void         test(const std::string& id);         // force-fire the outcome now
    void         deactivate();
    bool         active() const;
    std::string  active_id() const;

    nlohmann::json to_json() const;
    void           load_json(const nlohmann::json& j);

private:
    struct EdgeState { int side = 0; double spin_accum = 0.0; double cooldown = 0.0; };

    ReactionRule& rule_locked(const std::string& id);
    void fire_locked(const std::string& id, const ReactionRule& r);
    void deactivate_locked();
    double now_s() const { return clock_s_; }

    mutable std::mutex mtx_;
    Actions act_;
    Signals sig_;
    std::map<std::string, ReactionRule> rules_;
    std::map<std::string, EdgeState>    edge_;

    bool        active_ = false;        // an expression outcome is held
    std::string active_id_;
    double      hold_left_ = 0.0;
    bool        latched_ = false;
    std::string restore_face_;
    double      clock_s_ = 0.0;
};

}  // namespace face
