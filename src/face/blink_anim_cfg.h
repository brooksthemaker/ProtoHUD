#pragma once
// Writers for the `blink_anim` block in a face folder's config.json.
//
// These live beside the reader (face_loader.cpp) and away from the controller
// on purpose: the write is the exact inverse of the parse, and when the two
// drifted apart the result was a silent data-loss bug — the face-wide setter
// REPLACED the whole `blink_anim` object, so touching the face-wide toggle or
// frame count wiped every per-expression override the wearer had set. Nothing
// errored; the settings just quietly went back to "Use Face Default".
//
// Both helpers are strict read-modify-write: they touch only the keys they
// own and leave the rest of `blink_anim` — and the rest of the face config —
// untouched.

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

namespace face {

// Frames past this are not worth drawing — see kBlinkAnimMaxFrames in
// face_loader.h, which this deliberately mirrors rather than includes, so the
// config writers stay independent of the renderer.
inline constexpr int kBlinkCfgMaxFrames = 8;

namespace detail {
inline nlohmann::json& blink_block(nlohmann::json& cfg) {
    if (!cfg.is_object()) cfg = nlohmann::json::object();
    if (!cfg.contains("blink_anim") || !cfg["blink_anim"].is_object())
        cfg["blink_anim"] = nlohmann::json::object();
    return cfg["blink_anim"];
}
}  // namespace detail

// Face-wide sequence. ⚠ Only enabled/frames/whole are written — `expressions`
// and anything else already in the block survive.
inline void blink_cfg_set_face(nlohmann::json& cfg, bool enabled, int frames,
                               bool whole) {
    auto& ja = detail::blink_block(cfg);
    ja["enabled"] = enabled;
    ja["frames"]  = std::clamp(frames, 0, kBlinkCfgMaxFrames);
    ja["whole"]   = whole;
}

// One expression's override. mode: 0 = inherit, 1 = own sequence, 2 = never
// animate. ⚠ INHERIT IS THE ABSENCE OF THE KEY, not a stored value — storing
// {"enabled":true} would pin the expression to whatever the face-wide frame
// count happened to be that day, so later changes to the face default would
// silently stop reaching it, and only for expressions that had ever been
// opened in the menu.
inline void blink_cfg_set_expr(nlohmann::json& cfg, const std::string& expr,
                               int mode, int frames, bool whole) {
    if (expr.empty()) return;
    auto& ja = detail::blink_block(cfg);
    if (!ja.contains("expressions") || !ja["expressions"].is_object())
        ja["expressions"] = nlohmann::json::object();
    if (mode == 0) {
        ja["expressions"].erase(expr);
    } else {
        ja["expressions"][expr] = {
            {"enabled", mode == 1},
            {"frames",  std::clamp(frames, 0, kBlinkCfgMaxFrames)},
            {"whole",   whole},
        };
    }
    if (ja["expressions"].empty()) ja.erase("expressions");
}

}  // namespace face
