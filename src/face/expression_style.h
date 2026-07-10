#pragma once
// ── expression_style.h ─────────────────────────────────────────────────────────
// Per-expression look override. Every expression — built-in slot or
// user-created — may carry its own material color, particle effect, and
// glitch config. Unset fields inherit the global default look, so an empty
// ExpressionStyle is exactly the legacy behavior. Resolution happens in
// NativeFaceController::apply_expression_style_locked (three layers, weakest
// to strongest: default look → per-expression style → the custom-expression
// override slot).

#include <string>

#include <nlohmann/json.hpp>

#include "glitch.h"

namespace face {

struct ExpressionStyle {
    std::string    material_spec;            // "" = inherit default material
    nlohmann::json effect_spec;              // null = inherit; else a particle
                                             // spec ({"preset":..}, {"layers":[..]}
                                             // or "none")
    bool           effect_overlay = false;   // true = layer effect_spec ON TOP of
                                             // the base/ambient effect instead of
                                             // replacing it (ignored when
                                             // effect_spec is null or "none")
    bool           has_glitch = false;       // false = inherit default glitch
    GlitchConfig   glitch;                   // used when has_glitch

    bool any() const {
        return !material_spec.empty() || !effect_spec.is_null() || has_glitch;
    }

    nlohmann::json to_json() const;
    static ExpressionStyle from_json(const nlohmann::json& j);
};

} // namespace face
