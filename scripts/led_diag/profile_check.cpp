// Headless check of accessory-LED profiles: that a profile captures the LOOK and
// never the LAYOUT, that save/load round-trips, and that name-keyed lookup
// behaves when profiles are renamed, reordered or deleted.
#include <cstdio>
#include <string>

#include "accessory/accessory_profiles.h"

using namespace accessory;

static bool fail = false;
static void expect(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) fail = true;
}

// A zone with a distinctive look AND a distinctive layout, so a profile leaking
// layout fields is immediately visible.
static ZoneConfig make_zone(int seed) {
    ZoneConfig z;
    z.name = "z" + std::to_string(seed);
    z.pattern = Pattern::Wave;
    z.r = (uint8_t)(10 + seed); z.g = (uint8_t)(20 + seed); z.b = (uint8_t)(30 + seed);
    z.stops = { {1,2,3}, {4,5,6} };
    z.breathe_hz = 1.5f + seed;
    z.wave_speed = -0.75f;
    z.grad_spatial = true;
    z.grad_angle = 42.f;
    z.zone_brightness = 111;
    z.follow_face = true;
    z.level_style = LevelStyle::Peak;
    z.min_level = 0.25f;
    z.sound_trigger = true;
    z.sound_threshold = 0.6f;
    z.sound_decay = 3.5f;
    z.sound_complete = true;
    // Layout half — a profile must never carry or overwrite any of this.
    z.start = 100 + seed;
    z.count = 55 + seed;
    z.shape = Shape::Rings;
    z.sections = { 7, 8, 9 };
    z.pos_x = -12; z.pos_y = 34;
    z.rotation = 15.f;
    z.mirror = true;
    z.reverse = true;
    z.serpentine = true;
    z.scale = 2.f;
    return z;
}

int main() {
    std::array<ZoneConfig, ZoneCount> zones{};
    for (int i = 0; i < ZoneCount; ++i) zones[i] = make_zone(i);

    // ── capture / apply ──────────────────────────────────────────────────────
    std::printf("capture + apply\n");
    AccessoryLeds::Config cfg;
    cfg.zones = zones;
    cfg.global_brightness = 77;
    cfg.sync_sides = true;
    cfg.link_areas = true;
    const LedProfile p = capture_profile(cfg, nullptr, "Angry");
    expect(p.name == "Angry", "profile keeps its name");
    expect(p.zones[2].pattern == Pattern::Wave &&
           p.zones[2].zone_brightness == 111 &&
           p.zones[2].stops.size() == 2 &&
           p.zones[2].sound_complete,
           "look fields are captured");

    // Apply onto DIFFERENT zones whose layout must survive untouched.
    std::array<ZoneConfig, ZoneCount> target{};
    for (int i = 0; i < ZoneCount; ++i) {
        target[i].start = 900 + i;  target[i].count = 7 + i;
        target[i].shape = Shape::Lines;
        target[i].sections = { 1, 1 };
        target[i].pos_x = 5; target[i].pos_y = 6;
        target[i].rotation = -3.f; target[i].mirror = false;
        target[i].reverse = false; target[i].serpentine = false;
        target[i].name = "keepme";
        target[i].pattern = Pattern::Off;    // look: should be overwritten
    }
    AccessoryLeds::Config tcfg;
    tcfg.zones = target;
    tcfg.global_brightness = 1; tcfg.sync_sides = false; tcfg.link_areas = false;
    apply_profile_config(tcfg, p);
    target = tcfg.zones;
    expect(target[3].pattern == Pattern::Wave && target[3].zone_brightness == 111,
           "apply overwrites the LOOK");
    bool layout_intact = true;
    for (int i = 0; i < ZoneCount; ++i) {
        const auto& t = target[i];
        if (t.start != 900 + i || t.count != 7 + i || t.shape != Shape::Lines ||
            t.sections.size() != 2 || t.pos_x != 5 || t.pos_y != 6 ||
            t.rotation != -3.f || t.mirror || t.reverse || t.serpentine ||
            t.name != "keepme")
            layout_intact = false;
    }
    expect(layout_intact,
           "apply leaves EVERY layout/wiring field untouched");

    expect(p.globals.global_brightness == 77 && p.globals.sync_sides &&
           p.globals.link_areas, "chain-wide globals are captured");
    expect(tcfg.global_brightness == 77 && tcfg.sync_sides && tcfg.link_areas,
           "apply writes the globals into the config");

    // ── json round-trip ──────────────────────────────────────────────────────
    std::printf("\njson round-trip\n");
    LedProfiles store;
    store.put(p);
    store.put(capture_profile(cfg, nullptr, "Happy"));
    const auto j = store.to_json();
    LedProfiles loaded;
    loaded.from_json(j);
    expect(loaded.count() == 2, "both profiles survive save+load");
    const auto* q = loaded.find("Angry");
    expect(q != nullptr, "lookup by name after load");
    if (q) {
        const auto& a = p.zones[1];
        const auto& b = q->zones[1];
        expect(a.pattern == b.pattern && a.r == b.r && a.g == b.g && a.b == b.b &&
               a.stops == b.stops && a.breathe_hz == b.breathe_hz &&
               a.wave_speed == b.wave_speed && a.grad_spatial == b.grad_spatial &&
               a.grad_angle == b.grad_angle &&
               a.zone_brightness == b.zone_brightness &&
               a.follow_face == b.follow_face && a.level_style == b.level_style &&
               a.min_level == b.min_level && a.sound_trigger == b.sound_trigger &&
               a.sound_threshold == b.sound_threshold &&
               a.sound_decay == b.sound_decay &&
               a.sound_complete == b.sound_complete,
               "every look field round-trips exactly");
    }

    // ── name-keyed binding ───────────────────────────────────────────────────
    std::printf("\nname-keyed binding (the anti-rebind property)\n");
    {
        LedProfiles s;
        s.put(capture_profile(cfg, nullptr, "A"));
        s.put(capture_profile(cfg, nullptr, "B"));
        s.put(capture_profile(cfg, nullptr, "C"));
        // An expression holds the NAME "C". Deleting "A" shifts every index down,
        // which is exactly what would rebind an index-keyed reference.
        const int c_before = s.index_of("C");
        s.remove(s.index_of("A"));
        const int c_after = s.index_of("C");
        expect(c_before == 2 && c_after == 1, "indices DO shift on delete");
        expect(s.find("C") != nullptr, "the name still resolves to the same profile");
        // Saving over an existing name updates in place rather than duplicating.
        const int n_before = s.count();
        s.put(capture_profile(cfg, nullptr, "B"));
        expect(s.count() == n_before, "re-saving a name updates, not duplicates");
        // A deleted profile's name simply stops resolving (action becomes a no-op).
        s.remove(s.index_of("B"));
        expect(s.find("B") == nullptr, "a deleted name resolves to nothing");
    }

    // ── malformed input ──────────────────────────────────────────────────────
    std::printf("\nmalformed config\n");
    {
        LedProfiles s;
        s.from_json(nlohmann::json::parse(
            R"([{"name":"ok","zones":[{"pattern":3}]},{"zones":[]},{"name":""},17])"));
        // No globals key: a profile saved before globals existed must still load.
        expect(s.find("ok") && s.find("ok")->globals.global_brightness == 64,
               "a pre-globals profile loads with sane defaults");
        expect(s.count() == 1, "unnamed / non-object entries are dropped");
        expect(s.find("ok") != nullptr, "the valid entry survives");
        const auto* r = s.find("ok");
        // Only one zone was given; the rest must still be present at defaults.
        expect(r && r->zones.size() == (size_t)ZoneCount,
               "a short zones array still yields a full profile");
    }
    {
        LedProfiles s;
        s.from_json(nlohmann::json("not an array"));
        expect(s.count() == 0, "a non-array profiles value is ignored");
    }

    std::printf("\n%s\n", fail ? "*** SOME CHECKS FAILED ***" : "ALL CHECKS PASSED");
    return fail ? 1 : 0;
}
