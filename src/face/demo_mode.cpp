// ── demo_mode.cpp ──────────────────────────────────────────────────────────────
// See demo_mode.h. The option tables below are deliberately curated subsets of
// everything the face supports (e.g. black/very-dark looks are left out so the
// demo never looks "off"), matching the indices the Face Display menu uses:
//   • palettes → set_menu_item(8, idx)  (see NativeFaceController::preset_material)
//   • effects  → set_effect(id)         (native/daemon effect_id map, 0..22)

#include "face/demo_mode.h"

#include <mutex>

#include <nlohmann/json.hpp>

#include "app_state.h"
#include "serial/face_controller.h"

namespace face {
namespace {

// Material indices (Protoface > Material Color). Skips Black (11) and the dark
// PNG patterns so every step stays vivid.
constexpr int kPalettes[] = {
    0,   // Teal
    6,   // Red
    2,   // Orange
    1,   // Yellow
    4,   // Green
    7,   // Blue
    5,   // Purple
    3,   // White
    8,   // Rainbow
    12,  // Sunset
    13,  // Ocean
    14,  // Forest
    15,  // Fire
    16,  // Aurora
    18,  // Galaxy
    20,  // Candy
    21,  // Toxic
    22,  // Pride Rainbow
};

// Effect ids (native/daemon map). Skips "none" (0) so a step always shows
// something; the snapshot/restore handles getting back to the user's effect.
constexpr int kEffects[] = {
    1,   // sparkle
    2,   // embers
    5,   // confetti
    6,   // rings
    7,   // fireflies
    8,   // fire
    9,   // aurora
    13,  // celebration
    14,  // galaxy
    16,  // clouds
    17,  // nebula
    18,  // starfield
    21,  // shooting_stars
};

// Brightness levels swept when the brightness track is on.
constexpr int kBrightness[] = { 220, 150, 90, 255 };

// Canonical emotions we probe for. Only the ones that actually exist in the
// loaded face folder are used (so the demo adapts to whatever face is set).
constexpr const char* kEmotions[] = {
    "neutral", "happy", "angry", "sad", "surprised", "squint",
    "smug", "tired", "blushing", "embarrassed", "crazy", "uncertain", "shocked",
};

// Curated combos for the Showcase sequence: an expression paired with an effect
// and a colour that read well together (mirrors the Expression Effects moods).
struct Combo { const char* expr; int effect; int palette; };
constexpr Combo kCombos[] = {
    { "happy",     13,  8 },   // celebration + rainbow
    { "angry",      8,  6 },   // fire        + red
    { "sad",        3, 13 },   // rain        + ocean
    { "surprised", 14, 18 },   // galaxy      + galaxy
    { "neutral",    9, 16 },   // aurora      + aurora
    { "happy",      5, 20 },   // confetti    + candy
    { "smug",       7,  5 },   // fireflies   + purple
    { "neutral",   17, 18 },   // nebula      + galaxy
    { "crazy",      1, 21 },   // sparkle     + toxic
    { "neutral",   18, 13 },   // starfield   + ocean
};

template <typename T, size_t N>
int pick(std::mt19937& rng, const T (&arr)[N]) {
    std::uniform_int_distribution<size_t> d(0, N - 1);
    return static_cast<int>(arr[d(rng)]);
}

} // namespace

DemoMode::DemoMode(IFaceController* face, AppState* state)
    : face_(face), state_(state) {}

const char* DemoMode::sequence_name(Sequence s) {
    switch (s) {
        case Sequence::Showcase: return "Showcase";
        case Sequence::Tour:     return "Tour";
        case Sequence::Shuffle:  return "Shuffle";
    }
    return "Showcase";
}

std::vector<std::string> DemoMode::available_expressions() const {
    std::vector<std::string> out;
    if (!face_) return out;
    for (const char* e : kEmotions)
        if (face_->face_image_exists(e)) out.emplace_back(e);
    return out;
}

void DemoMode::snapshot() {
    if (!face_) return;
    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        snap_palette_    = state_->face.material_color;
        snap_effect_     = state_->face.effect_id;
        snap_brightness_ = state_->face.brightness;
    }
    snap_expression_ = face_->current_expression();
    have_snapshot_   = true;
}

void DemoMode::restore() {
    if (!have_snapshot_ || !face_) return;
    if (ran_palettes_) face_->set_menu_item(8, static_cast<uint8_t>(snap_palette_));
    if (ran_effects_)  face_->set_effect(static_cast<uint8_t>(snap_effect_));
    if (ran_brightness_) face_->set_brightness(static_cast<uint8_t>(snap_brightness_));
    if (ran_expressions_ && !snap_expression_.empty())
        face_->set_face_by_name(snap_expression_);
    if (state_) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        if (ran_palettes_)   state_->face.material_color = static_cast<uint8_t>(snap_palette_);
        if (ran_effects_)    state_->face.effect_id      = static_cast<uint8_t>(snap_effect_);
        if (ran_brightness_) state_->face.brightness     = static_cast<uint8_t>(snap_brightness_);
    }
    have_snapshot_ = false;
}

void DemoMode::apply_scene(const Scene& s) {
    if (!face_) return;
    if (s.palette >= 0) {
        face_->set_menu_item(8, static_cast<uint8_t>(s.palette));
        if (state_) {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->face.material_color = static_cast<uint8_t>(s.palette);
        }
    }
    if (s.effect >= 0) {
        face_->set_effect(static_cast<uint8_t>(s.effect));
        if (state_) {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->face.effect_id = static_cast<uint8_t>(s.effect);
        }
    }
    if (s.brightness >= 0) {
        face_->set_brightness(static_cast<uint8_t>(s.brightness));
        if (state_) {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->face.brightness = static_cast<uint8_t>(s.brightness);
        }
    }
    if (s.next_expr)            face_->next_expression();
    else if (!s.expression.empty()) face_->set_face_by_name(s.expression);
}

void DemoMode::rebuild_scenes() {
    scenes_.clear();
    step_ = 0;
    const std::vector<std::string> exprs = available_expressions();

    if (cfg_.sequence == Sequence::Showcase) {
        for (const auto& c : kCombos) {
            Scene s;
            if (cfg_.cycle_palettes) s.palette = c.palette;
            if (cfg_.cycle_effects)  s.effect  = c.effect;
            if (cfg_.cycle_expressions) {
                if (face_ && face_->face_image_exists(c.expr)) s.expression = c.expr;
            }
            scenes_.push_back(std::move(s));
        }
        return;
    }

    if (cfg_.sequence == Sequence::Tour) {
        // One axis at a time, so each track gets its own full showcase.
        if (cfg_.cycle_palettes)
            for (int p : kPalettes) { Scene s; s.palette = p; scenes_.push_back(s); }
        if (cfg_.cycle_effects)
            for (int e : kEffects)  { Scene s; s.effect = e; scenes_.push_back(s); }
        if (cfg_.cycle_expressions) {
            if (!exprs.empty())
                for (const auto& e : exprs) { Scene s; s.expression = e; scenes_.push_back(s); }
            else
                for (int i = 0; i < 5; ++i) { Scene s; s.next_expr = true; scenes_.push_back(s); }
        }
        if (cfg_.cycle_brightness)
            for (int b : kBrightness) { Scene s; s.brightness = b; scenes_.push_back(s); }
        return;
    }
    // Shuffle builds scenes on the fly in advance(); nothing to prebuild.
}

DemoMode::Scene DemoMode::random_scene() {
    Scene s;
    if (cfg_.cycle_palettes) s.palette = pick(rng_, kPalettes);
    if (cfg_.cycle_effects)  s.effect  = pick(rng_, kEffects);
    if (cfg_.cycle_expressions) {
        const auto exprs = available_expressions();
        if (!exprs.empty()) {
            std::uniform_int_distribution<size_t> d(0, exprs.size() - 1);
            s.expression = exprs[d(rng_)];
        } else {
            s.next_expr = true;
        }
    }
    if (cfg_.cycle_brightness) s.brightness = pick(rng_, kBrightness);
    return s;
}

void DemoMode::advance() {
    if (cfg_.sequence == Sequence::Shuffle) {
        apply_scene(random_scene());
        return;
    }
    if (scenes_.empty()) return;
    apply_scene(scenes_[step_ % scenes_.size()]);
    ++step_;
}

void DemoMode::start_internal(bool attract) {
    if (running_ || !face_) return;
    snapshot();
    ran_palettes_    = cfg_.cycle_palettes;
    ran_effects_     = cfg_.cycle_effects;
    ran_expressions_ = cfg_.cycle_expressions;
    ran_brightness_  = cfg_.cycle_brightness;
    rebuild_scenes();
    running_         = true;
    attract_started_ = attract;
    dwell_t_         = 0.0;
    advance();   // show the first step immediately
}

void DemoMode::start()  { start_internal(false); }

void DemoMode::stop() {
    if (!running_) return;
    running_         = false;
    attract_started_ = false;
    restore();
}

void DemoMode::toggle() { if (running_) stop(); else start(); }

void DemoMode::reconfigure() {
    if (!running_) return;
    // Track anything newly enabled so restore() still covers it.
    ran_palettes_    |= cfg_.cycle_palettes;
    ran_effects_     |= cfg_.cycle_effects;
    ran_expressions_ |= cfg_.cycle_expressions;
    ran_brightness_  |= cfg_.cycle_brightness;
    rebuild_scenes();
    dwell_t_ = 0.0;
    advance();
}

void DemoMode::tick(double dt, bool user_active) {
    if (user_active) {
        idle_t_ = 0.0;
        if (running_ && attract_started_) stop();   // attract run yields to the user
    } else {
        idle_t_ += dt;
        if (cfg_.attract_enabled && !running_ && idle_t_ >= cfg_.attract_idle_s)
            start_internal(true);
    }
    if (running_) {
        dwell_t_ += dt;
        if (dwell_t_ >= cfg_.dwell_s) {
            dwell_t_ = 0.0;
            advance();
        }
    }
}

void DemoMode::load_config(const nlohmann::json& j) {
    if (!j.is_object()) return;
    cfg_.cycle_palettes    = j.value("cycle_palettes",    cfg_.cycle_palettes);
    cfg_.cycle_effects     = j.value("cycle_effects",     cfg_.cycle_effects);
    cfg_.cycle_expressions = j.value("cycle_expressions", cfg_.cycle_expressions);
    cfg_.cycle_brightness  = j.value("cycle_brightness",  cfg_.cycle_brightness);
    cfg_.dwell_s           = j.value("dwell_s",           cfg_.dwell_s);
    cfg_.attract_enabled   = j.value("attract_enabled",   cfg_.attract_enabled);
    cfg_.attract_idle_s    = j.value("attract_idle_s",    cfg_.attract_idle_s);
    const std::string seq  = j.value("sequence", std::string("showcase"));
    if      (seq == "tour")    cfg_.sequence = Sequence::Tour;
    else if (seq == "shuffle") cfg_.sequence = Sequence::Shuffle;
    else                       cfg_.sequence = Sequence::Showcase;
}

nlohmann::json DemoMode::save_config() const {
    nlohmann::json j;
    j["cycle_palettes"]    = cfg_.cycle_palettes;
    j["cycle_effects"]     = cfg_.cycle_effects;
    j["cycle_expressions"] = cfg_.cycle_expressions;
    j["cycle_brightness"]  = cfg_.cycle_brightness;
    j["dwell_s"]           = cfg_.dwell_s;
    j["attract_enabled"]   = cfg_.attract_enabled;
    j["attract_idle_s"]    = cfg_.attract_idle_s;
    switch (cfg_.sequence) {
        case Sequence::Tour:     j["sequence"] = "tour";     break;
        case Sequence::Shuffle:  j["sequence"] = "shuffle";  break;
        case Sequence::Showcase: j["sequence"] = "showcase"; break;
    }
    return j;
}

} // namespace face
