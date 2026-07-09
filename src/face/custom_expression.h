#pragma once
// ── custom_expression.h ────────────────────────────────────────────────────────
// User-created expressions. A custom expression borrows its art from one of
// the existing expression PNG slots (base_expression), carries its own
// ExpressionStyle (material / effect / glitch), and lists the triggers that
// activate it. The system seeds kInitialCustomSlots empty slots; "Add
// Another..." in the menu appends more up to kMaxCustomExpressions (the menu
// pre-allocates that many placeholder rows — see item_factories.h on why the
// tree must not grow while open).
//
// Persisted as the protoface.custom_expressions array inside the normal
// config save flow. Runtime activation/restore lives in ExpressionDirector.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "expression_style.h"

namespace face {

struct CustomTrigger {
    enum class Type : uint8_t { Manual = 0, Boop, Gesture, Motion, Light };
    Type        type = Type::Manual;
    int         boop_zone = -1;      // 0 snout / 1 left / 2 right / 3 both
    std::string gesture;             // "up" / "down" / "left" / "right" (APDS-9960)
    std::string motion_event;        // "shake" (wake spike) / "still" (drowsy)
    std::string light_edge;          // "bright" / "dark"
    float       light_lux = 800.f;   // threshold for the light edge

    nlohmann::json to_json() const {
        nlohmann::json j;
        switch (type) {
            case Type::Manual:  j["type"] = "manual"; break;
            case Type::Boop:    j["type"] = "boop";    j["zone"] = boop_zone; break;
            case Type::Gesture: j["type"] = "gesture"; j["gesture"] = gesture; break;
            case Type::Motion:  j["type"] = "motion";  j["event"] = motion_event; break;
            case Type::Light:   j["type"] = "light";   j["edge"] = light_edge;
                                j["lux"] = light_lux; break;
        }
        return j;
    }
    static CustomTrigger from_json(const nlohmann::json& j) {
        CustomTrigger t;
        const std::string ty = j.is_object() ? j.value("type", "manual") : "manual";
        if      (ty == "boop")    { t.type = Type::Boop;    t.boop_zone = j.value("zone", -1); }
        else if (ty == "gesture") { t.type = Type::Gesture; t.gesture = j.value("gesture", ""); }
        else if (ty == "motion")  { t.type = Type::Motion;  t.motion_event = j.value("event", ""); }
        else if (ty == "light")   { t.type = Type::Light;   t.light_edge = j.value("edge", "");
                                    t.light_lux = j.value("lux", 800.f); }
        else                      t.type = Type::Manual;
        return t;
    }
};

struct CustomExpression {
    bool            used = false;               // slot occupied (empty slots are seeds)
    std::string     name;
    std::string     base_expression = "neutral"; // art = an existing PNG slot
    ExpressionStyle style;
    double          hold_s = 3.0;                // 0 = latch until manual return
    std::vector<CustomTrigger> triggers;

    const CustomTrigger* trigger(CustomTrigger::Type ty) const {
        for (const auto& t : triggers) if (t.type == ty) return &t;
        return nullptr;
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["used"] = used;
        if (!used) return j;
        j["name"]            = name;
        j["base_expression"] = base_expression;
        j["style"]           = style.to_json();
        j["hold_s"]          = hold_s;
        nlohmann::json jt = nlohmann::json::array();
        for (const auto& t : triggers) jt.push_back(t.to_json());
        j["triggers"] = std::move(jt);
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
        c.hold_s          = j.value("hold_s", 3.0);
        if (j.contains("triggers") && j["triggers"].is_array())
            for (const auto& jt : j["triggers"])
                c.triggers.push_back(CustomTrigger::from_json(jt));
        return c;
    }
};

constexpr int kInitialCustomSlots   = 5;
constexpr int kMaxCustomExpressions = 24;   // menu placeholder-row cap

} // namespace face
