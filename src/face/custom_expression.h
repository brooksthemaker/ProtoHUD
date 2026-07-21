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

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "expression_style.h"

namespace face {

struct TriggerRecipe {
    // The counting event.
    enum class Event : uint8_t {
        None = 0,
        Boop,           // boop_zone picks snout/cheeks/both/head/mouth
        Gesture,        // gesture picks up/down/left/right (APDS-9960 swipe)
        Shake,          // head-motion spike (ReactionEngine wake_dps)
        LightBright,    // ambient light rising past light_lux
        LightDark,      // ambient light falling past light_lux
    };
    Event       event = Event::None;
    int         boop_zone = 0;          // sensor::BoopSensor::Zone value: 0 snout / 1 left /
                                        // 2 right / 3 both / 4 head / 5 mouth top / 6 mouth bottom
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

// A non-face side-effect an expression fires when it ACTIVATES and undoes when
// it ends — the LED/servo analogue of switching the face. Two kinds today:
//   Led   → drive an accessory-LED zone's pattern/color while active.
//   Servo → move a coprocessor servo channel to an angle while active.
// The revert target for LEDs is the zone's config snapshotted at apply time;
// for servos (no read-back) it's a configured rest angle. Actions attach to
// the expression's TriggerSet, so they fire on ANY of its recipes.
struct ExprAction {
    enum class Kind : uint8_t { None = 0, Led = 1, Servo = 2 };
    Kind kind = Kind::None;

    // Led action — which accessory::Zone (0..4) and what to show while active.
    int     led_zone      = 0;
    int     led_pattern   = 1;      // accessory::Pattern value; -1 = leave the zone's pattern
    uint8_t r = 0, g = 200, b = 80; // color applied when led_set_color
    bool    led_set_color = true;   // false = keep the zone's own color, only swap the pattern
    int     led_brightness = -1;    // 0..255 per-zone brightness while active; -1 = leave

    // Servo action — raw coprocessor channel 0..3 (same addressing as the
    // Peripheral Test servo sliders).
    int servo_ch   = 0;
    int servo_deg  = 90;            // 0..180 while active
    int servo_rest = 90;            // 0..180 return angle on revert; -1 = detach

    bool armed() const { return kind != Kind::None; }

    nlohmann::json to_json() const {
        static const char* kn[] = { "none", "led", "servo" };
        nlohmann::json j;
        j["kind"] = kn[static_cast<int>(kind)];
        if (kind == Kind::Led) {
            j["zone"]       = led_zone;
            j["pattern"]    = led_pattern;
            j["set_color"]  = led_set_color;
            j["color"]      = nlohmann::json::array({ r, g, b });
            j["brightness"] = led_brightness;
        } else if (kind == Kind::Servo) {
            j["ch"]   = servo_ch;
            j["deg"]  = servo_deg;
            j["rest"] = servo_rest;
        }
        return j;
    }
    static ExprAction from_json(const nlohmann::json& j) {
        ExprAction a;
        if (!j.is_object()) return a;
        const std::string k = j.value("kind", "none");
        if      (k == "led")   a.kind = Kind::Led;
        else if (k == "servo") a.kind = Kind::Servo;
        if (a.kind == Kind::Led) {
            a.led_zone      = j.value("zone", 0);
            a.led_pattern   = j.value("pattern", 1);
            a.led_set_color = j.value("set_color", true);
            if (j.contains("color") && j["color"].is_array() && j["color"].size() == 3) {
                a.r = static_cast<uint8_t>(std::clamp(j["color"][0].get<int>(), 0, 255));
                a.g = static_cast<uint8_t>(std::clamp(j["color"][1].get<int>(), 0, 255));
                a.b = static_cast<uint8_t>(std::clamp(j["color"][2].get<int>(), 0, 255));
            }
            a.led_brightness = j.value("brightness", -1);
        } else if (a.kind == Kind::Servo) {
            a.servo_ch   = j.value("ch", 0);
            a.servo_deg  = j.value("deg", 90);
            a.servo_rest = j.value("rest", 90);
        }
        return a;
    }
};

// The per-expression trigger set (built-in stem or "custom_<slot>" key).
struct TriggerSet {
    double hold_s = 3.0;                      // 0 = latch until manual return
    std::vector<TriggerRecipe> recipes;       // menu pre-allocates kRecipeSlots
    std::vector<ExprAction>    actions;       // extra side-effects on activation (kActionSlots)

    bool any() const {
        for (const auto& r : recipes) if (r.armed()) return true;
        for (const auto& a : actions) if (a.armed()) return true;
        return false;
    }
    nlohmann::json to_json() const {
        nlohmann::json j;
        j["hold_s"] = hold_s;
        nlohmann::json jr = nlohmann::json::array();
        for (const auto& r : recipes) jr.push_back(r.to_json());
        j["recipes"] = std::move(jr);
        nlohmann::json ja = nlohmann::json::array();
        for (const auto& a : actions) if (a.armed()) ja.push_back(a.to_json());
        if (!ja.empty()) j["actions"] = std::move(ja);
        return j;
    }
    static TriggerSet from_json(const nlohmann::json& j) {
        TriggerSet t;
        if (!j.is_object()) return t;
        t.hold_s = j.value("hold_s", 3.0);
        if (j.contains("recipes") && j["recipes"].is_array())
            for (const auto& jr : j["recipes"])
                t.recipes.push_back(TriggerRecipe::from_json(jr));
        if (j.contains("actions") && j["actions"].is_array())
            for (const auto& ja : j["actions"])
                t.actions.push_back(ExprAction::from_json(ja));
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
constexpr int kActionSlots          = 3;    // extra (LED/servo) actions per expression

} // namespace face
