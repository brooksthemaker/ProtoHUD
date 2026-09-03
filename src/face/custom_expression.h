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
        // ── System events ───────────────────────────────────────────────────
        // Not sensor readings — these are fired from the app's own lifecycle
        // and state, through ExpressionDirector::on_system(). Appended after
        // the sensor events so existing saved values keep their meaning.
        Boot,           // once, when startup finishes
        Shutdown,       // once, when a quit is requested (before teardown)
        BatteryLow,     // battery falls to/below kBatteryLowPct
        PhoneCharging,  // paired phone started charging (edge). The device
                        // itself exposes no charger signal, so this is the
                        // phone's state and is labelled as such.
        WifiUp,         // Wi-Fi associated (edge)
        WifiDown,       // Wi-Fi association lost (edge)
        Overheat,       // CPU package temperature rises past kOverheatC
    };
    // Fixed thresholds for the level-based system events. Deliberately not
    // per-recipe fields: the recipe editor is shared verbatim with faces and
    // eye animations, and adding a row there would change every trigger UI.
    static constexpr int   kBatteryLowPct = 20;
    static constexpr float kOverheatC     = 75.f;
    // True for events fired by on_system() rather than by a sensor callback.
    static bool is_system(Event e) { return e >= Event::Boot; }
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
                                    "light_bright", "light_dark",
                                    "boot", "shutdown", "battery_low",
                                    "phone_charging", "wifi_up", "wifi_down",
                                    "overheat" };
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
        else if (ev == "boot")         r.event = Event::Boot;
        else if (ev == "shutdown")     r.event = Event::Shutdown;
        else if (ev == "battery_low")  r.event = Event::BatteryLow;
        else if (ev == "phone_charging") r.event = Event::PhoneCharging;
        else if (ev == "wifi_up")      r.event = Event::WifiUp;
        else if (ev == "wifi_down")    r.event = Event::WifiDown;
        else if (ev == "overheat")     r.event = Event::Overheat;
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
    enum class Kind : uint8_t {
        None = 0, Led = 1, Servo = 2, LedProfile = 3, LedOverlay = 4
    };
    Kind kind = Kind::None;

    // LedOverlay action — a colour layer composited OVER the chosen zones while
    // this expression is active, covering part of each zone so the rest of the
    // zone's own look still shows underneath (a blush rising up the cheeks).
    // Unlike the other actions this ANIMATES: it rises on activation, behaves per
    // ov_mode while held, and retracts when the expression ends.
    uint32_t ov_zones   = 0x1F;     // bit per accessory::Zone; default all five
    uint8_t  ov_r = 255, ov_g = 105, ov_b = 180;   // blush pink
    int      ov_shape   = 0;        // accessory::OverlayShape (0 Rise/1 Sweep/2 Bloom)
    int      ov_mode    = 0;        // accessory::OverlayMode  (0 RiseHold/1 Cycle/2 Once)
    int      ov_opacity = 85;       // percent at full coverage
    int      ov_angle   = 90;       // axis across the zone's shape, degrees
    int      ov_soft    = 18;       // edge feather, percent
    float    ov_rise_s  = 0.6f;
    float    ov_fall_s  = 0.9f;
    float    ov_cycle   = 0.5f;     // Cycle rate / Sweep travel, Hz

    // LedProfile action — recall a whole saved accessory-LED look (all zones at
    // once) while this expression is active, then restore what was there.
    // Stored BY NAME, not index: renaming/reordering/deleting profiles then can
    // never silently rebind an expression to somebody else's lighting. An
    // unknown name is simply a no-op (the profile was deleted).
    std::string led_profile;

    // Led action — which accessory::Zone (0..4) and what to show while active.
    int     led_zone      = 0;
    int     led_pattern   = 1;      // accessory::Pattern value; -1 = leave the zone's pattern
    uint8_t r = 0, g = 200, b = 80; // color applied when led_set_color
    bool    led_set_color = true;   // false = keep the zone's own color, only swap the pattern
    int     led_brightness = -1;    // 0..255 per-zone brightness while active; -1 = leave

    // Servo action. servo_ch is now an INDEX into the configured servos (Face
    // Display > Servo Settings), NOT a raw coprocessor channel — the servo's own
    // channel, travel limits, centre and slew speed come from its config, so a
    // trigger can't drive an ear past its mechanical stops. The field keeps its
    // old name (and its "ch" JSON key) so existing saved actions still load;
    // index 0 was channel 0 in the old raw scheme, so the common case survives.
    int servo_ch   = 0;
    int servo_deg  = 90;            // 0..180 while active (clamped to the limits)
    int servo_rest = 90;            // 0..180 return angle on revert; -1 = use its rest
    // Drive the servo's configured partner too: 0 None, 1 Copy (same angle),
    // 2 Mirror (reflected about the partner's centre). Ignored with no partner.
    int servo_pair = 0;

    bool armed() const { return kind != Kind::None; }

    nlohmann::json to_json() const {
        static const char* kn[] = { "none", "led", "servo", "led_profile", "led_overlay" };
        nlohmann::json j;
        j["kind"] = kn[static_cast<int>(kind)];
        if (kind == Kind::LedOverlay) {
            j["ov_zones"] = ov_zones;
            j["ov_color"] = nlohmann::json::array({ ov_r, ov_g, ov_b });
            j["ov_shape"] = ov_shape;
            j["ov_mode"]  = ov_mode;
            j["ov_op"]    = ov_opacity;
            j["ov_angle"] = ov_angle;
            j["ov_soft"]  = ov_soft;
            j["ov_rise"]  = ov_rise_s;
            j["ov_fall"]  = ov_fall_s;
            j["ov_cycle"] = ov_cycle;
        } else if (kind == Kind::LedProfile) {
            j["profile"] = led_profile;
        } else if (kind == Kind::Led) {
            j["zone"]       = led_zone;
            j["pattern"]    = led_pattern;
            j["set_color"]  = led_set_color;
            j["color"]      = nlohmann::json::array({ r, g, b });
            j["brightness"] = led_brightness;
        } else if (kind == Kind::Servo) {
            j["ch"]   = servo_ch;
            j["deg"]  = servo_deg;
            j["rest"] = servo_rest;
            j["pair"] = servo_pair;
        }
        return j;
    }
    static ExprAction from_json(const nlohmann::json& j) {
        ExprAction a;
        if (!j.is_object()) return a;
        const std::string k = j.value("kind", "none");
        if      (k == "led")         a.kind = Kind::Led;
        else if (k == "servo")       a.kind = Kind::Servo;
        else if (k == "led_profile") a.kind = Kind::LedProfile;
        else if (k == "led_overlay") a.kind = Kind::LedOverlay;
        if (a.kind == Kind::LedOverlay) {
            a.ov_zones   = j.value("ov_zones", 0x1Fu);
            if (j.contains("ov_color") && j["ov_color"].is_array() &&
                j["ov_color"].size() == 3) {
                a.ov_r = static_cast<uint8_t>(std::clamp(j["ov_color"][0].get<int>(), 0, 255));
                a.ov_g = static_cast<uint8_t>(std::clamp(j["ov_color"][1].get<int>(), 0, 255));
                a.ov_b = static_cast<uint8_t>(std::clamp(j["ov_color"][2].get<int>(), 0, 255));
            }
            a.ov_shape   = j.value("ov_shape", 0);
            a.ov_mode    = j.value("ov_mode",  0);
            a.ov_opacity = j.value("ov_op",    85);
            a.ov_angle   = j.value("ov_angle", 90);
            a.ov_soft    = j.value("ov_soft",  18);
            a.ov_rise_s  = j.value("ov_rise",  0.6f);
            a.ov_fall_s  = j.value("ov_fall",  0.9f);
            a.ov_cycle   = j.value("ov_cycle", 0.5f);
        } else if (a.kind == Kind::LedProfile) {
            a.led_profile = j.value("profile", std::string());
        } else if (a.kind == Kind::Led) {
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
            a.servo_pair = j.value("pair", 0);
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

// Reserved expression_triggers key for the diagnostics banner. It rides in the
// same map as the expression rules — so it gets the same recipe editor, the
// same event pipeline and the same persistence — but resolves to "raise the
// readout" rather than to a face. Prefixed so it can't collide with an
// expression stem or a custom slot key.
inline constexpr const char* kDiagTriggerKey = "__diag";

// Event-text slots. Each holds its own message and its own full set of banner
// properties, and rides the SAME trigger map as faces/eye-anims/diagnostics
// under the key "textev_<n>" — so the recipe editor, the event pipeline and
// the load/save path are all reused unchanged.
inline constexpr const char* kTextEventKeyPrefix = "textev_";
inline constexpr int         kTextEventSlots     = 8;

} // namespace face
