#pragma once
// ── demo_mode.h ────────────────────────────────────────────────────────────────
// Protoface "demo" / attract mode. Takes whatever face is currently set and
// cycles it through colours (materials/palettes), particle effects and
// expressions to show off what the face can do, hands-free.
//
// It is a thin orchestrator on top of IFaceController: every step is just an
// existing set_menu_item / set_effect / set_brightness / set_face_by_name call,
// so there is no new rendering code and it works on both the native and daemon
// Protoface backends. The look that was set before the demo started is snapshot
// and restored when it stops.
//
// Three cycling styles (user-selectable):
//   • Showcase — curated expression+effect+colour combos that look good together
//     (leans on the same mood pairings as Expression Effects).
//   • Tour     — march through one axis at a time (every colour, then every
//     effect, then every expression). Methodical "show me everything".
//   • Shuffle  — randomised combos, no fixed order. Ambient background showing-off.
//
// Each axis ("track") can be enabled/disabled independently, so the user picks
// what gets cycled. An optional attract mode auto-starts the demo after a period
// of inactivity (off by default); a self-started attract run yields back to the
// user on the next interaction.

#include <random>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

class IFaceController;
struct AppState;

namespace face {

class DemoMode {
public:
    enum class Sequence { Showcase = 0, Tour, Shuffle };

    struct Config {
        bool     cycle_palettes    = true;   // colours / materials
        bool     cycle_effects     = true;   // particle effects
        bool     cycle_expressions = true;   // emotions
        bool     cycle_brightness  = false;  // sweep brightness too
        Sequence sequence          = Sequence::Showcase;
        double   dwell_s           = 3.5;    // hold time per step
        bool     attract_enabled   = false;  // auto-start when idle
        double   attract_idle_s    = 90.0;   // idle time before attract kicks in
    };

    DemoMode(IFaceController* face, AppState* state);

    // Manual control. start()/stop() are idempotent; toggle() flips.
    void start();
    void stop();
    void toggle();
    bool running() const { return running_; }

    // Per-frame pump from the render loop. user_active should be true when the
    // user interacted this frame (an input event was drained or the menu is
    // open) — it resets the attract idle timer and hands a self-started attract
    // run back to the user. A demo the user started manually keeps running until
    // it is toggled off.
    void tick(double dt, bool user_active);

    Config&       config()       { return cfg_; }
    const Config& config() const { return cfg_; }

    // Re-derive the step list from the live config while running (so toggling a
    // track / sequence in the menu takes effect immediately). No-op when idle.
    void reconfigure();

    void           load_config(const nlohmann::json& j);
    nlohmann::json save_config() const;

    static const char* sequence_name(Sequence s);

private:
    // A single demo step. Fields left at their sentinel are not touched, so a
    // Tour step can change just one axis while a Showcase step sets all three.
    struct Scene {
        int         palette    = -1;     // material index, -1 = leave as-is
        int         effect     = -1;     // effect id,      -1 = leave as-is
        int         brightness = -1;     // 0-255,          -1 = leave as-is
        std::string expression;          // set by name; empty = leave as-is
        bool        next_expr  = false;  // advance the loader's expression set
    };

    void  start_internal(bool attract);
    void  snapshot();
    void  restore();
    void  rebuild_scenes();
    void  advance();
    void  apply_scene(const Scene& s);
    Scene random_scene();
    std::vector<std::string> available_expressions() const;

    IFaceController* face_  = nullptr;
    AppState*        state_ = nullptr;

    Config cfg_;

    bool   running_         = false;
    bool   attract_started_ = false;  // active run was auto-started by attract mode
    // Which axes the active run actually cycles — captured at start so restore()
    // only touches what the demo changed (an untouched axis, e.g. a custom
    // layered effect, is left exactly as the user had it).
    bool   ran_palettes_    = false;
    bool   ran_effects_     = false;
    bool   ran_expressions_ = false;
    bool   ran_brightness_  = false;
    double dwell_t_         = 0.0;
    double idle_t_          = 0.0;
    size_t step_            = 0;
    std::vector<Scene> scenes_;
    std::mt19937 rng_{std::random_device{}()};

    // The look from before the demo started, restored on stop().
    bool        have_snapshot_   = false;
    int         snap_palette_    = 0;
    int         snap_effect_     = 0;
    int         snap_brightness_ = 200;
    std::string snap_expression_;
};

} // namespace face
