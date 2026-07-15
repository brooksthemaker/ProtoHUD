#pragma once
// ── custom_expression.h ────────────────────────────────────────────────────────
// User-created expressions + the trigger-recipe vocabulary shared by EVERY
// expression (built-in slots and custom ones alike).
//
// A custom expression borrows its art from one of the existing expression
// PNG slots (base_expression) and carries its own ExpressionStyle. Trigger
// recipes do NOT live here — they're keyed per expression in
// AppState::expression_triggers ("happy", or "custom_3" for slot 3) so
// built-ins get the same treatment; ExpressionDirector evaluates them.
//
// A TriggerRecipe is "event × count within a window, WHILE conditions hold":
//   nose boop ×5 (3 s)                      → angry
//   left cheek ×1 while head tilted left    → curious
// Conditions are checked at the moment the counting event fires.
//
// Persisted via the normal config save flow: protoface.custom_expressions
// (the slots) and protoface.expression_triggers (the recipe sets).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "expression_style.h"

namespace face {

struct TriggerRecipe {
    // The counting event.
    enum class Event : uint8_t {
        None = 0,
        Boop,           // boop_zone picks snout/left/right/both
        Gesture,        // gesture picks up/down/left/right (APDS-9960 swipe)
        Shake,          // head-motion spike (ReactionEngine wake_dps)
        LightBright,    // ambient light rising past light_lux
        LightDark,      // ambient light falling past light_lux
    };
    Event       event = Event::None;
    int         boop_zone = 0;          // 0 snout / 1 left / 2 right / 3 both
    std::string gesture;                // "up" / "down" / "left" / "right"
    int         count    = 1;           // events within window_s to fire
    float       window_s = 3.0f;

    // WHILE-conditions — all must hold when the final event lands.
    enum class Tilt   : uint8_t { Any = 0, Left, Right };
    enum class Light  : uint8_t { Any = 0, Bright, Dark };
    enum class Motion : uint8_t { Any = 0, Moving, Still };
    Tilt   tilt      = Tilt::Any;
    float  tilt_deg  = 15.f;            // |roll| beyond this counts as tilted
    Light  light     = Light::Any;
    float  light_lux = 800.f;           // threshold for the light condition/event
    Motion motion    = Motion::Any;

    bool armed() const { return event != Event::None; }
    // More specific recipes win when one event satisfies several (higher
    // count first, then more conditions).
    int specificity() const {
        return count * 10 + (tilt != Tilt::Any) + (light != Light::Any) +
               (motion != Motion::Any);
    }

    nlohmann::json to_json() const {
        static const char* ev[] = { "none", "boop", "gesture", "shake",
                                    "light_bright", "light_dark" };
        nlohmann::json j;
        j["event"] = ev[static_cast<int>(event)];
        if (event == Event::Boop)    j["zone"]    = boop_zone;
        if (event == Event::Gesture) j["gesture"] = gesture;
        j["count"]    = count;
        j["window_s"] = window_s;
        if (tilt != Tilt::Any)
            j["tilt"] = (tilt == Tilt::Left) ? "left" : "right";
        j["tilt_deg"] = tilt_deg;
        if (light != Light::Any)
            j["light"] = (light == Light::Bright) ? "bright" : "dark";
        j["light_lux"] = light_lux;
        if (motion != Motion::Any)
            j["motion"] = (motion == Motion::Moving) ? "moving" : "still";
        return j;
    }
    static TriggerRecipe from_json(const nlohmann::json& j) {
        TriggerRecipe r;
        if (!j.is_object()) return r;
        const std::string ev = j.value("event", "none");
        if      (ev == "boop")         { r.event = Event::Boop;
                                         r.boop_zone = j.value("zone", 0); }
        else if (ev == "gesture")      { r.event = Event::Gesture;
                                         r.gesture = j.value("gesture", ""); }
        else if (ev == "shake")        r.event = Event::Shake;
        else if (ev == "light_bright") r.event = Event::LightBright;
        else if (ev == "light_dark")   r.event = Event::LightDark;
        r.count    = std::max(1, j.value("count", 1));
        r.window_s = j.value("window_s", 3.0f);
        const std::string t = j.value("tilt", "");
        if (t == "left")  r.tilt = Tilt::Left;
        if (t == "right") r.tilt = Tilt::Right;
        r.tilt_deg = j.value("tilt_deg", 15.f);
        const std::string l = j.value("light", "");
        if (l == "bright") r.light = Light::Bright;
        if (l == "dark")   r.light = Light::Dark;
        r.light_lux = j.value("light_lux", 800.f);
        const std::string m = j.value("motion", "");
        if (m == "moving") r.motion = Motion::Moving;
        if (m == "still")  r.motion = Motion::Still;
        return r;
    }
};

// The per-expression trigger set (built-in stem or "custom_<slot>" key).
struct TriggerSet {
    double hold_s = 3.0;                      // 0 = latch until manual return
    std::vector<TriggerRecipe> recipes;       // menu pre-allocates kRecipeSlots

    bool any() const {
        for (const auto& r : recipes) if (r.armed()) return true;
        return false;
    }
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["hold_s"] = hold_s;
        nlohmann::json jr = nlohmann::json::array();
        for (const auto& r : recipes) jr.push_back(r.to_json());
        j["recipes"] = std::move(jr);
        return j;
    }
    static TriggerSet from_json(const nlohmann::json& j) {
        TriggerSet t;
        if (!j.is_object()) return t;
        t.hold_s = j.value("hold_s", 3.0);
        if (j.contains("recipes") && j["recipes"].is_array())
            for (const auto& jr : j["recipes"])
                t.recipes.push_back(TriggerRecipe::from_json(jr));
        return t;
    }
};

struct CustomExpression {
    bool            used = false;               // slot occupied (empty = seed)
    std::string     name;
    std::string     base_expression = "neutral"; // art = an existing PNG slot
    ExpressionStyle style;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["used"] = used;
        if (!used) return j;
        j["name"]            = name;
        j["base_expression"] = base_expression;
        j["style"]           = style.to_json();
        return j;
    }
    static CustomExpression from_json(const nlohmann::json& j) {
        CustomExpression c;
        if (!j.is_object()) return c;
        c.used = j.value("used", false);
        if (!c.used) return c;
        c.name            = j.value("name", "");
        c.base_expression = j.value("base_expression", "neutral");
        if (j.contains("style")) c.style = ExpressionStyle::from_json(j["style"]);
        return c;
    }
};

constexpr int kInitialCustomSlots   = 5;
constexpr int kMaxCustomExpressions = 24;   // menu placeholder-row cap
constexpr int kRecipeSlots          = 3;    // trigger recipes per expression

} // namespace face
