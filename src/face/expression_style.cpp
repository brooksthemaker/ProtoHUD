#include "expression_style.h"

namespace face {

nlohmann::json ExpressionStyle::to_json() const {
    nlohmann::json j = nlohmann::json::object();
    if (!material_spec.empty())  j["material"] = material_spec;
    if (!effect_spec.is_null()) {
        j["effect"] = effect_spec;
        if (effect_overlay) j["effect_overlay"] = true;
    }
    if (has_glitch)              j["glitch"]   = glitch.to_json();
    return j;
}

ExpressionStyle ExpressionStyle::from_json(const nlohmann::json& j) {
    ExpressionStyle s;
    if (!j.is_object()) return s;
    if (j.contains("material") && j["material"].is_string())
        s.material_spec = j["material"].get<std::string>();
    if (j.contains("effect")) s.effect_spec = j["effect"];
    s.effect_overlay = j.value("effect_overlay", false);
    if (j.contains("glitch") && j["glitch"].is_object()) {
        s.has_glitch = true;
        s.glitch = GlitchConfig::from_json(j["glitch"]);
    }
    return s;
}

} // namespace face
