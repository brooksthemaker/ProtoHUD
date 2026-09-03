#pragma once
// ── accessory_profiles.h ─────────────────────────────────────────────────────
// Named snapshots of how the accessory LEDs LOOK, so a whole lighting mood can
// be saved once and then recalled — by hand from the menu, or automatically by a
// face expression (Faces and Expressions > [expression] > Actions).
//
// ⚠️ A profile carries the LOOK half of each zone and deliberately NOT the
// layout half. Shape, sections, count, start, position, rotation, mirror,
// reverse and serpentine all describe how the strip was physically BUILT — they
// are the same whatever mood the face is in, and changing them re-chains the
// strip. An expression flipping to "angry" must never do that, so those fields
// are simply not part of a profile and cannot be disturbed by one.
//
// Profiles are referenced BY NAME, not by index: expressions store the name, so
// reordering or deleting profiles can never silently rebind an expression to
// somebody else's lighting (the same hazard the eye-animation enum has, where
// indices are positional).

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accessory/accessory_leds.h"

namespace accessory {

// Everything that decides how one zone appears. Mirrors the corresponding
// ZoneConfig fields; defaults match ZoneConfig's so a partial profile loads sane.
struct ZoneLook {
    Pattern    pattern         = Pattern::Solid;
    uint8_t    r = 0, g = 220, b = 180;
    std::vector<std::array<uint8_t, 3>> stops;
    float      breathe_hz      = 0.5f;
    float      wave_speed      = 0.5f;
    bool       grad_spatial    = false;
    float      grad_angle      = 0.f;
    float      palette_drift   = 0.f;
    uint8_t    zone_brightness = 255;
    bool       follow_face     = false;
    LevelStyle level_style     = LevelStyle::Glow;
    float      min_level       = 0.f;
    bool       sound_trigger   = false;
    float      sound_threshold = 0.3f;
    float      sound_decay     = 2.0f;
    bool       sound_complete  = false;
};

// Pull the look out of a zone config (or a live zone snapshot — same type).
inline ZoneLook look_of(const ZoneConfig& z) {
    ZoneLook L;
    L.pattern         = z.pattern;
    L.r = z.r; L.g = z.g; L.b = z.b;
    L.stops           = z.stops;
    L.breathe_hz      = z.breathe_hz;
    L.wave_speed      = z.wave_speed;
    L.grad_spatial    = z.grad_spatial;
    L.grad_angle      = z.grad_angle;
    L.palette_drift   = z.palette_drift;
    L.zone_brightness = z.zone_brightness;
    L.follow_face     = z.follow_face;
    L.level_style     = z.level_style;
    L.min_level       = z.min_level;
    L.sound_trigger   = z.sound_trigger;
    L.sound_threshold = z.sound_threshold;
    L.sound_decay     = z.sound_decay;
    L.sound_complete  = z.sound_complete;
    return L;
}

// Write a look into a zone config, leaving every layout/wiring field untouched.
inline void apply_look(ZoneConfig& z, const ZoneLook& L) {
    z.pattern         = L.pattern;
    z.r = L.r; z.g = L.g; z.b = L.b;
    z.stops           = L.stops;
    z.breathe_hz      = L.breathe_hz;
    z.wave_speed      = L.wave_speed;
    z.grad_spatial    = L.grad_spatial;
    z.grad_angle      = L.grad_angle;
    z.palette_drift   = L.palette_drift;
    z.zone_brightness = L.zone_brightness;
    z.follow_face     = L.follow_face;
    z.level_style     = L.level_style;
    z.min_level       = L.min_level;
    z.sound_trigger   = L.sound_trigger;
    z.sound_threshold = L.sound_threshold;
    z.sound_decay     = L.sound_decay;
    z.sound_complete  = L.sound_complete;
}

// Chain-wide settings a profile also carries. These are NOT layout — they're
// look/behaviour that applies across every zone, so an emotion can dim the whole
// chain or link the cheeks as part of its mood.
struct ProfileGlobals {
    uint8_t global_brightness = 64;
    bool    sync_sides        = false;
    bool    link_areas        = false;
};

struct LedProfile {
    std::string                     name;
    std::array<ZoneLook, ZoneCount> zones{};
    ProfileGlobals                  globals;
};

// ── JSON ─────────────────────────────────────────────────────────────────────
inline nlohmann::json look_to_json(const ZoneLook& L) {
    nlohmann::json j;
    j["pattern"]         = static_cast<int>(L.pattern);
    j["color"]           = nlohmann::json::array({ L.r, L.g, L.b });
    if (!L.stops.empty()) {
        nlohmann::json js = nlohmann::json::array();
        for (const auto& s : L.stops)
            js.push_back(nlohmann::json::array({ s[0], s[1], s[2] }));
        j["stops"] = std::move(js);
    }
    j["breathe_hz"]      = L.breathe_hz;
    j["wave_speed"]      = L.wave_speed;
    j["grad_spatial"]    = L.grad_spatial;
    j["grad_angle"]      = L.grad_angle;
    j["palette_drift"]   = L.palette_drift;
    j["zone_brightness"] = L.zone_brightness;
    j["follow_face"]     = L.follow_face;
    j["level_style"]     = static_cast<int>(L.level_style);
    j["min_level"]       = L.min_level;
    j["sound_trigger"]   = L.sound_trigger;
    j["sound_threshold"] = L.sound_threshold;
    j["sound_decay"]     = L.sound_decay;
    j["sound_complete"]  = L.sound_complete;
    return j;
}

inline ZoneLook look_from_json(const nlohmann::json& j) {
    ZoneLook L;
    if (!j.is_object()) return L;
    auto num = [&](const char* k, auto def) {
        return (j.contains(k) && j[k].is_number()) ? j[k].get<decltype(def)>() : def;
    };
    auto boolean = [&](const char* k, bool def) {
        return (j.contains(k) && j[k].is_boolean()) ? j[k].get<bool>() : def;
    };
    L.pattern = static_cast<Pattern>(num("pattern", static_cast<int>(L.pattern)));
    if (j.contains("color") && j["color"].is_array() && j["color"].size() == 3) {
        L.r = j["color"][0].get<uint8_t>();
        L.g = j["color"][1].get<uint8_t>();
        L.b = j["color"][2].get<uint8_t>();
    }
    if (j.contains("stops") && j["stops"].is_array()) {
        for (const auto& js : j["stops"])
            if (js.is_array() && js.size() == 3)
                L.stops.push_back({ js[0].get<uint8_t>(),
                                    js[1].get<uint8_t>(),
                                    js[2].get<uint8_t>() });
    }
    L.breathe_hz      = num("breathe_hz",      L.breathe_hz);
    L.wave_speed      = num("wave_speed",      L.wave_speed);
    L.grad_spatial    = boolean("grad_spatial", L.grad_spatial);
    L.grad_angle      = num("grad_angle",      L.grad_angle);
    L.palette_drift   = num("palette_drift",   L.palette_drift);
    L.zone_brightness = static_cast<uint8_t>(
        std::clamp(num("zone_brightness", static_cast<int>(L.zone_brightness)), 0, 255));
    L.follow_face     = boolean("follow_face", L.follow_face);
    L.level_style     = static_cast<LevelStyle>(
        num("level_style", static_cast<int>(L.level_style)));
    L.min_level       = num("min_level",       L.min_level);
    L.sound_trigger   = boolean("sound_trigger", L.sound_trigger);
    L.sound_threshold = num("sound_threshold", L.sound_threshold);
    L.sound_decay     = num("sound_decay",     L.sound_decay);
    L.sound_complete  = boolean("sound_complete", L.sound_complete);
    return L;
}

// ── Apply ────────────────────────────────────────────────────────────────────
// Push a profile onto the LIVE strip. Only look setters are called, so nothing
// re-chains and the strip length can't change. This is what an expression
// action uses (temporary, reverted when the expression ends).
// One zone's look onto the live strip. The per-zone form matters for REVERT:
// restoring only the zones that were actually captured, without touching any
// other zone (a whole-profile write would blast the rest with defaults).
inline void apply_zone_look_live(AccessoryLeds& leds, Zone z, const ZoneLook& L) {
    leds.set_zone_pattern        (z, L.pattern);
    leds.set_zone_color          (z, L.r, L.g, L.b);
    leds.set_zone_stops          (z, L.stops);
    leds.set_zone_breathe_hz     (z, L.breathe_hz);
    leds.set_zone_wave_speed     (z, L.wave_speed);
    leds.set_zone_grad_spatial   (z, L.grad_spatial);
    leds.set_zone_grad_angle     (z, L.grad_angle);
    leds.set_zone_palette_drift  (z, L.palette_drift);
    leds.set_zone_brightness     (z, L.zone_brightness);
    leds.set_zone_follow_face    (z, L.follow_face);
    leds.set_zone_level_style    (z, L.level_style);
    leds.set_zone_min_level      (z, L.min_level);
    leds.set_zone_sound_trigger  (z, L.sound_trigger);
    leds.set_zone_sound_threshold(z, L.sound_threshold);
    leds.set_zone_sound_decay    (z, L.sound_decay);
    leds.set_zone_sound_complete (z, L.sound_complete);
}

inline void apply_globals_live(AccessoryLeds& leds, const ProfileGlobals& g) {
    leds.set_global_brightness(g.global_brightness);
    leds.set_sync_sides(g.sync_sides);
    leds.set_link_areas(g.link_areas);
}

inline void apply_profile_live(AccessoryLeds& leds, const LedProfile& p) {
    for (int i = 0; i < ZoneCount; ++i)
        apply_zone_look_live(leds, static_cast<Zone>(i), p.zones[i]);
    apply_globals_live(leds, p.globals);
}

// Write a profile into the EDITABLE config, so the zone editor, the preview and
// the next config save all agree with what the strip is showing. The menu's
// Apply does both this and apply_profile_live.
inline void apply_profile_config(AccessoryLeds::Config& cfg, const LedProfile& p) {
    for (int i = 0; i < ZoneCount; ++i) apply_look(cfg.zones[i], p.zones[i]);
    cfg.global_brightness = p.globals.global_brightness;
    cfg.sync_sides        = p.globals.sync_sides;
    cfg.link_areas        = p.globals.link_areas;
}

// Snapshot what a profile stores. Zone looks come from the EDITABLE config (the
// menu dual-writes those as you edit), but the globals are read from the LIVE
// controller when one is given — the master Brightness slider only ever writes
// to the live object, so cfg.global_brightness can be stale.
inline LedProfile capture_profile(const AccessoryLeds::Config& cfg,
                                  const AccessoryLeds* live,
                                  std::string name) {
    LedProfile p;
    p.name = std::move(name);
    for (int i = 0; i < ZoneCount; ++i) p.zones[i] = look_of(cfg.zones[i]);
    p.globals.global_brightness =
        live ? live->global_brightness() : cfg.global_brightness;
    p.globals.sync_sides = live ? live->sync_sides() : cfg.sync_sides;
    p.globals.link_areas = live ? live->link_areas() : cfg.link_areas;
    return p;
}

// ── Store ────────────────────────────────────────────────────────────────────
// Lives in the HUD config (not one file each, like the whole-system
// ProfileManager) because these are small, and because expressions reference
// them — keeping them beside the expression that names them means a config is
// self-contained and can't half-load.
class LedProfiles {
public:
    int count() const { return static_cast<int>(v_.size()); }
    const LedProfile* get(int i) const {
        return (i >= 0 && i < count()) ? &v_[i] : nullptr;
    }
    LedProfile* mutable_get(int i) {
        return (i >= 0 && i < count()) ? &v_[i] : nullptr;
    }
    std::vector<LedProfile>&       all()       { return v_; }
    const std::vector<LedProfile>& all() const { return v_; }

    int index_of(const std::string& name) const {
        for (int i = 0; i < count(); ++i) if (v_[i].name == name) return i;
        return -1;
    }
    const LedProfile* find(const std::string& name) const {
        const int i = index_of(name);
        return i >= 0 ? &v_[i] : nullptr;
    }

    // Add or overwrite by name, returning the index. Saving over an existing
    // name UPDATES it rather than making a duplicate, so any expression already
    // pointing at that name keeps working and picks up the new look.
    int put(const LedProfile& p) {
        const int i = index_of(p.name);
        if (i >= 0) { v_[i] = p; return i; }
        v_.push_back(p);
        return count() - 1;
    }
    void remove(int i) {
        if (i >= 0 && i < count()) v_.erase(v_.begin() + i);
    }
    // A name not already taken, for the "save as new" default.
    std::string unused_name(const std::string& stem = "Profile") const {
        for (int n = 1; n < 999; ++n) {
            std::string c = stem + " " + std::to_string(n);
            if (index_of(c) < 0) return c;
        }
        return stem;
    }

    nlohmann::json to_json() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : v_) {
            nlohmann::json j;
            j["name"] = p.name;
            nlohmann::json jz = nlohmann::json::array();
            for (const auto& L : p.zones) jz.push_back(look_to_json(L));
            j["zones"] = std::move(jz);
            j["global_brightness"] = p.globals.global_brightness;
            j["sync_sides"]        = p.globals.sync_sides;
            j["link_areas"]        = p.globals.link_areas;
            arr.push_back(std::move(j));
        }
        return arr;
    }

    void from_json(const nlohmann::json& arr) {
        v_.clear();
        if (!arr.is_array()) return;
        for (const auto& j : arr) {
            if (!j.is_object()) continue;
            LedProfile p;
            p.name = j.value("name", std::string());
            if (p.name.empty()) continue;              // unnamed = unreferencable
            if (j.contains("zones") && j["zones"].is_array()) {
                int i = 0;
                for (const auto& jz : j["zones"]) {
                    if (i >= ZoneCount) break;
                    p.zones[i++] = look_from_json(jz);
                }
            }
            // Absent in profiles saved before globals were carried — those keep
            // the struct defaults, which is the same as "don't change much".
            if (j.contains("global_brightness") && j["global_brightness"].is_number())
                p.globals.global_brightness = static_cast<uint8_t>(
                    std::clamp(j["global_brightness"].get<int>(), 0, 255));
            if (j.contains("sync_sides") && j["sync_sides"].is_boolean())
                p.globals.sync_sides = j["sync_sides"].get<bool>();
            if (j.contains("link_areas") && j["link_areas"].is_boolean())
                p.globals.link_areas = j["link_areas"].get<bool>();
            v_.push_back(std::move(p));
        }
    }

private:
    std::vector<LedProfile> v_;
};

}  // namespace accessory
