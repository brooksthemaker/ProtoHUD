// Headless check for the Temp Effects per-band custom effect (Face Display >
// Effects > Temp Effects > Frost / Heat / Mild > Custom Effect).
//
// The menu stores the NAME of a saved layered preset; main.cpp resolves that
// name against cfg["protoface"]["custom_effects"] at ambient-sync time. This
// exercises that resolution against the DEVICE'S REAL config.json and then
// renders what comes out, so a name that resolves to a spec the engine can't
// actually run still fails here rather than silently showing nothing on the
// panels.
//
// build: g++ -std=c++17 -O2 -I src scripts/face_diag/temp_band_check.cpp \
//            src/face/particles.cpp $(pkg-config --cflags --libs opencv4) \
//            -o /tmp/temp_band_check
#include "face/particles.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

static int fails = 0;
static void chk(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) ++fails;
}

// ⚠ This MUST mirror main.cpp's temp_band_spec lambda. If that changes, change
// this — a check that has drifted from the code under test is worse than none.
static json temp_band_spec(const json& cfg, const std::string& custom,
                           json builtin) {
    if (custom.empty()) return builtin;
    auto jpf = cfg.find("protoface");
    if (jpf != cfg.end() && jpf->is_object()) {
        auto ce = jpf->find("custom_effects");
        if (ce != jpf->end() && ce->is_object()) {
            auto hit = ce->find(custom);
            if (hit != ce->end()) return *hit;
        }
    }
    return builtin;
}

// Does this spec actually put pixels on the panel? Runs the real engine for a
// couple of seconds — some effects (breath, lightning) are event-shaped and
// emit nothing on frame 1, so a single frame would be a false negative.
static bool renders_something(const json& spec, int w, int h) {
    face::ParticleSystem ps(w, h, spec);
    for (int i = 0; i < 120; ++i) {          // 2 s at 60 fps
        ps.update(1.0 / 60.0);
        face::ParticleFrame f = ps.render();
        if (!f.has || f.rgba.empty()) continue;
        // Any non-transparent pixel counts.
        std::vector<cv::Mat> ch;
        cv::split(f.rgba, ch);
        if (cv::countNonZero(ch[3]) > 0) return true;
        if (!f.face_tint.empty() && cv::countNonZero(f.face_tint) > 0) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/config.json";
    std::ifstream in(path);
    if (!in) { std::printf("cannot open %s\n", path.c_str()); return 2; }
    json cfg; in >> cfg;

    const int W = 128, H = 64;               // this rig's framebuffer
    const json frost_builtin = {{"effect", "frost"}, {"count", 44},
                                {"fractal", true}, {"speed", 1.0},
                                {"blend", "add"}};
    const json heat_builtin  = {{"effect", "heatwave"}, {"count", 18},
                                {"heartbeat", true}, {"speed", 1.0},
                                {"blend", "add"}};

    std::printf("config: %s\n", path.c_str());

    // ── Defaults must not change today's behaviour ───────────────────────────
    std::printf("\nempty name -> the band's built-in:\n");
    chk(temp_band_spec(cfg, "", frost_builtin) == frost_builtin,
        "cold band, no custom  -> built-in frost");
    chk(temp_band_spec(cfg, "", heat_builtin) == heat_builtin,
        "hot band, no custom   -> built-in heatwave");
    chk(temp_band_spec(cfg, "", json()).is_null(),
        "mild band, no custom  -> null (nothing), as before");

    // ── A deleted preset falls back, it does not go silent ───────────────────
    std::printf("\nmissing name -> fall back, never silent:\n");
    chk(temp_band_spec(cfg, "no_such_effect_xyz", frost_builtin) == frost_builtin,
        "cold band, deleted preset -> built-in frost");
    chk(temp_band_spec(cfg, "no_such_effect_xyz", json()).is_null(),
        "mild band, deleted preset -> null");

    // ── Every saved preset resolves AND renders ──────────────────────────────
    std::vector<std::string> names;
    if (cfg.contains("protoface") &&
        cfg["protoface"].contains("custom_effects") &&
        cfg["protoface"]["custom_effects"].is_object())
        for (auto& [k, _] : cfg["protoface"]["custom_effects"].items())
            names.push_back(k);
    std::printf("\n%zu saved custom effect(s) — each must resolve and render:\n",
                names.size());
    chk(!names.empty(), "config has at least one saved custom effect");

    for (const std::string& nm : names) {
        const json got = temp_band_spec(cfg, nm, frost_builtin);
        chk(got != frost_builtin, "\"" + nm + "\" resolves (not the fallback)");
        chk(renders_something(got, W, H), "\"" + nm + "\" renders visible pixels");
    }

    // ── The mild band is the one with no built-in ────────────────────────────
    if (!names.empty()) {
        const json mild = temp_band_spec(cfg, names.front(), json());
        chk(!mild.is_null(), "mild band with a custom picked -> non-null spec");
        chk(renders_something(mild, W, H), "mild band custom actually renders");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
