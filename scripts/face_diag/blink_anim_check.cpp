// Headless check for the animated blink (Files > Faces > Blink > Blink
// Animation). Builds throwaway face folders, runs the real FaceLoader +
// FaceState through whole blinks, and reads back WHICH frame landed on the
// panel each tick — so the frame-from-blink-weight mapping is verified by what
// comes out, not by re-deriving the arithmetic.
//
// Each frame PNG is a flat colour whose RED channel encodes its number
// (frame N = 10*N), which makes "which frame is showing" a single pixel read.
//
// build: g++ -std=c++17 -O2 -I src scripts/face_diag/blink_anim_check.cpp \
//            src/face/face_loader.cpp src/face/face_state.cpp \
//            $(pkg-config --cflags --libs opencv4) -o /tmp/blink_anim_check
#include "face/face_loader.h"
#include "face/face_state.h"
#include "face/face_config.h"
#include "face/blink_anim_cfg.h"
#include "face/json_atomic.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int fails = 0;
static void chk(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) ++fails;
}

static const int W = 32, H = 16;

static void write_png(const fs::path& p, int r, int g, int b) {
    fs::create_directories(p.parent_path());
    cv::Mat m(H, W, CV_8UC4, cv::Scalar(b, g, r, 255));   // imwrite wants BGRA
    cv::imwrite(p.string(), m);
}

// Build a face folder. frames_on_disk = how many blink/N.png to actually
// write; frames_in_cfg = what config.json claims. They differ on purpose in
// the "missing art" case.
static fs::path make_face(const std::string& name, bool anim_enabled,
                          int frames_in_cfg, int frames_on_disk,
                          bool skip_frame_2 = false,
                          const std::string& expr_cfg = "",
                          const std::string& expr_art = "",
                          int expr_art_frames = 0,
                          int expr_art_base = 100) {
    const fs::path dir = fs::temp_directory_path() / ("blinkchk_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    write_png(dir / "neutral.png", 200, 200, 200);
    write_png(dir / "blink.png",   255,   0,   0);        // red = the fallback
    for (int i = 1; i <= frames_on_disk; ++i) {
        if (skip_frame_2 && i == 2) continue;             // leave a hole
        write_png(dir / "blink" / (std::to_string(i) + ".png"), 10 * i, 0, 0);
    }
    // Per-expression art, e.g. blink/happy/1.png. Red = expr_art_base + 10*N,
    // so a frame from the per-expression sequence is distinguishable from the
    // face-wide one at a glance.
    for (int i = 1; i <= expr_art_frames; ++i)
        write_png(dir / "blink" / expr_art / (std::to_string(i) + ".png"),
                  expr_art_base + 10 * i, 0, 0);
    // Eye region covering the whole panel, so the region-blink path runs and
    // one sample pixel reports the blink art directly.
    std::ofstream(dir / "config.json") <<
        "{\"eye_left\":{\"points\":[[0,0],[" << (W - 1) << ",0],["
        << (W - 1) << "," << (H - 1) << "],[0," << (H - 1) << "]]},"
        "\"blink_anim\":{\"enabled\":" << (anim_enabled ? "true" : "false")
        << ",\"frames\":" << frames_in_cfg
        << (expr_cfg.empty() ? "" : ",\"expressions\":" + expr_cfg) << "}}";
    return dir;
}

// Run one full blink and return the red value seen on each tick where the eye
// was not fully open. Red 200 = the open face, 255 = the fallback blink.png,
// 10*N = animation frame N.
static std::vector<int> blink_trace(const fs::path& dir,
                                    const std::string& expr = "") {
    face::FaceLoader loader(dir.string(), W, H);
    face::FaceCfg fcfg;
    face::FaceState st(fcfg, loader.expression_names());
    if (!expr.empty()) st.set_expression(expr);
    st.set_blink_timing(3.0, 7.0, 0.15);
    st.trigger_blink();
    std::vector<int> seen;
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < 60; ++i) {                 // 1 s — a blink is ~0.19 s
        st.update(dt);
        if (st.blink_weight() <= 0.0) { if (!seen.empty()) break; continue; }
        cv::Mat f = loader.get_frame(st);
        seen.push_back(f.at<cv::Vec4b>(H / 2, W / 2)[0]);   // RGBA -> red
    }
    return seen;
}

static bool rises_then_falls(const std::vector<int>& v, int lo, int hi) {
    if (v.size() < 3) return false;
    if (v.front() != lo || v.back() != lo) return false;
    if (std::find(v.begin(), v.end(), hi) == v.end()) return false;
    size_t i = 1;
    while (i < v.size() && v[i] >= v[i - 1]) ++i;   // climbing
    while (i < v.size() && v[i] <= v[i - 1]) ++i;   // then descending
    return i == v.size();                           // never climbs twice
}

int main() {
    // Missing-art cases below deliberately imread() files that are not
    // there; the decoder warning is the expected path, not a problem.
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
    // ── 4 frames, all drawn ─────────────────────────────────────────────────
    {
        const auto dir = make_face("anim4", true, 4, 4);
        const auto v = blink_trace(dir);
        std::printf("\n4-frame animated blink — trace: ");
        for (int r : v) std::printf("%d ", r);
        std::printf("\n");
        chk(!v.empty(), "the blink produced frames at all");
        chk(std::set<int>(v.begin(), v.end()) == std::set<int>({10, 20, 30, 40}),
            "exactly frames 1-4 appear, and nothing else");
        chk(rises_then_falls(v, 10, 40),
            "plays DOWN to fully shut then back UP, never twice");
        chk(std::find(v.begin(), v.end(), 255) == v.end(),
            "the single blink.png never appears while animating");
        fs::remove_all(dir);
    }

    // ── Toggle off -> the original crossfade ────────────────────────────────
    {
        const auto dir = make_face("off", false, 4, 4);
        const auto v = blink_trace(dir);
        chk(!v.empty(), "disabled: the blink still happens");
        chk(std::find(v.begin(), v.end(), 10) == v.end() &&
            std::find(v.begin(), v.end(), 40) == v.end(),
            "disabled: no animation frame is ever used");
        fs::remove_all(dir);
    }

    // ── A single frame is not an animation ──────────────────────────────────
    {
        const auto dir = make_face("one", true, 1, 1);
        face::FaceLoader loader(dir.string(), W, H);
        chk(loader.blink_frame_count() == 0,
            "1 configured frame -> falls back (needs at least 2)");
        fs::remove_all(dir);
    }

    // ── Missing art: stop at the gap, don't skip it ─────────────────────────
    {
        const auto dir = make_face("gap", true, 4, 4, /*skip_frame_2=*/true);
        face::FaceLoader loader(dir.string(), W, H);
        chk(loader.blink_frame_count() == 0,
            "hole at frame 2 -> stops before it, too short, falls back");
        fs::remove_all(dir);
    }
    {
        const auto dir = make_face("short", true, 4, 3);   // 4 asked, 3 drawn
        face::FaceLoader loader(dir.string(), W, H);
        chk(loader.blink_frame_count() == 3,
            "4 configured but 3 drawn -> runs the 3 that exist");
        chk(loader.blink_anim_frames() == 4,
            "configured count is still reported as 4 (menu shows 3/4)");
        const auto v = blink_trace(dir);
        chk(rises_then_falls(v, 10, 30), "short sequence still plays down and up");
        fs::remove_all(dir);
    }

    // ── Asleep holds the LAST frame, not the first ──────────────────────────
    {
        const auto dir = make_face("asleep", true, 4, 4);
        face::FaceLoader loader(dir.string(), W, H);
        face::FaceCfg fcfg;
        face::FaceState st(fcfg, loader.expression_names());
        st.set_eyes_closed(true);
        st.update(1.0 / 60.0);
        cv::Mat f = loader.get_frame(st);
        chk(f.at<cv::Vec4b>(H / 2, W / 2)[0] == 40,
            "eyes_closed pins the fully-shut frame (4 of 4)");
        fs::remove_all(dir);
    }

    // ── No blink/ folder at all: unchanged behaviour ────────────────────────
    {
        const auto dir = make_face("none", true, 4, 0);
        face::FaceLoader loader(dir.string(), W, H);
        chk(loader.blink_frame_count() == 0, "no frames drawn -> plain crossfade");
        const auto v = blink_trace(dir);
        chk(!v.empty(), "no frames drawn: the blink still happens");
        fs::remove_all(dir);
    }

    // ── Per-expression sequences ────────────────────────────────────────────
    // "happy" has its own 3 frames (red 110/120/130); the face-wide default is
    // 4 frames (10/20/30/40). Every other expression must keep inheriting it.
    {
        const auto dir = make_face("perexpr", true, 4, 4, false,
                                   R"({"happy":{"enabled":true,"frames":3}})",
                                   "happy", 3, 100);
        write_png(dir / "happy.png", 190, 190, 190);
        face::FaceLoader probe(dir.string(), W, H);
        chk(probe.blink_frame_count_for("happy") == 3,
            "happy loaded its own 3 frames");
        chk(probe.blink_mode_for("happy") == face::FaceLoader::BlinkMode::Own,
            "happy reports mode Own");
        chk(probe.blink_mode_for("neutral") == face::FaceLoader::BlinkMode::Inherit,
            "neutral, unmentioned, reports mode Inherit");

        const auto vh = blink_trace(dir, "happy");
        std::printf("\nhappy (own 3 frames) — trace: ");
        for (int r : vh) std::printf("%d ", r);
        std::printf("\n");
        chk(std::set<int>(vh.begin(), vh.end()) == std::set<int>({110, 120, 130}),
            "happy blinks with ITS OWN frames, never the face-wide ones");
        chk(rises_then_falls(vh, 110, 130), "happy's own sequence plays down and up");

        const auto vn = blink_trace(dir, "neutral");
        chk(std::set<int>(vn.begin(), vn.end()) == std::set<int>({10, 20, 30, 40}),
            "neutral still inherits the face-wide 4 frames");
        fs::remove_all(dir);
    }

    // ── Per-expression "No Animation" ───────────────────────────────────────
    {
        const auto dir = make_face("exproff", true, 4, 4, false,
                                   R"({"angry":{"enabled":false}})");
        write_png(dir / "angry.png", 190, 190, 190);
        const auto va = blink_trace(dir, "angry");
        chk(!va.empty(), "angry still blinks");
        chk(std::find(va.begin(), va.end(), 10) == va.end() &&
            std::find(va.begin(), va.end(), 40) == va.end(),
            "angry set to No Animation uses no frame, despite the face default");
        const auto vn = blink_trace(dir, "neutral");
        chk(std::set<int>(vn.begin(), vn.end()) == std::set<int>({10, 20, 30, 40}),
            "...and neutral is unaffected by angry being off");
        fs::remove_all(dir);
    }

    // ── Enabled but no art yet -> falls through to the face-wide sequence ───
    {
        const auto dir = make_face("exprnoart", true, 4, 4, false,
                                   R"({"sad":{"enabled":true,"frames":3}})");
        write_png(dir / "sad.png", 190, 190, 190);
        const auto vs = blink_trace(dir, "sad");
        chk(std::set<int>(vs.begin(), vs.end()) == std::set<int>({10, 20, 30, 40}),
            "own-sequence with no art drawn yet falls back to the face default");
        fs::remove_all(dir);
    }

    // ── REGRESSION: the face-wide writer must not eat per-expression data ───
    // This is the bug the user hit: set_blink_anim assigned a fresh object to
    // cfg["blink_anim"], so touching the face-wide toggle or frame count wiped
    // every override. Silent, and it made the per-expression rows look
    // unselectable because they snapped back to "Use Face Default".
    {
        nlohmann::json cfg = nlohmann::json::object();
        face::blink_cfg_set_face(cfg, true, 4, false);
        face::blink_cfg_set_expr(cfg, "happy", 1, 3, true);
        chk(cfg["blink_anim"]["expressions"]["happy"]["enabled"] == true,
            "writer: happy override stored");
        face::blink_cfg_set_face(cfg, true, 6, false);          // the wipe case
        chk(cfg["blink_anim"].contains("expressions") &&
            cfg["blink_anim"]["expressions"].contains("happy"),
            "writer: face-wide write PRESERVES per-expression overrides");
        chk(cfg["blink_anim"]["expressions"]["happy"]["frames"] == 3,
            "writer: ...and preserves their frame counts");
        chk(cfg["blink_anim"]["frames"] == 6, "writer: face-wide frames updated");
        // Inherit erases, and drops the map once empty.
        face::blink_cfg_set_expr(cfg, "happy", 0, 3, true);
        chk(!cfg["blink_anim"].contains("expressions"),
            "writer: Inherit erases the entry (absence, not a stored value)");
        // Unrelated face-config keys must survive both writers.
        cfg["wiggle"] = {{"speed", 0.3}};
        face::blink_cfg_set_face(cfg, false, 2, true);
        face::blink_cfg_set_expr(cfg, "sad", 2, 4, false);
        chk(cfg.contains("wiggle"), "writer: unrelated face-config keys survive");
        chk(cfg["blink_anim"]["whole"] == true, "writer: face-wide whole stored");
    }

    // ── REGRESSION: per-expression settings readable with the master OFF ─────
    // Parsing used to be gated on blink_anim.enabled, so the menu could not
    // read back what it had just written and every option looked stuck.
    {
        const auto dir = make_face("masteroff", false, 4, 4, false,
                                   R"({"happy":{"enabled":true,"frames":3}})",
                                   "happy", 3, 100);
        write_png(dir / "happy.png", 190, 190, 190);
        face::FaceLoader probe(dir.string(), W, H);
        chk(probe.blink_mode_for("happy") == face::FaceLoader::BlinkMode::Own,
            "master OFF: happy still reports mode Own to the menu");
        const auto vh = blink_trace(dir, "happy");
        chk(std::find(vh.begin(), vh.end(), 110) == vh.end(),
            "master OFF: ...but nothing actually animates");
        fs::remove_all(dir);
    }

    // ── REGRESSION: the write must be COMPLETE before anyone reads it ───────
    // The bug: the setter wrote via an ofstream that was still open when the
    // loaders were told to re-read the file. The ofstream had truncated it on
    // open and was still holding the bytes, so the reload parsed an empty file,
    // the whole blink_anim block vanished, and every per-expression setting
    // read back as unset — the menu radio looked welded to "Use Face Default".
    {
        const fs::path dir = fs::temp_directory_path() / "blinkchk_atomic";
        fs::remove_all(dir);
        const fs::path f = dir / "config.json";
        nlohmann::json cfg = {{"wiggle", {{"speed", 0.3}}}};
        face::blink_cfg_set_face(cfg, true, 4, false);
        face::blink_cfg_set_expr(cfg, "happy", 1, 3, false);
        chk(face::write_json_atomic(f, cfg), "atomic write reports success");
        // Read it back the instant the call returns — this is exactly what
        // reload_active_face() does, and what used to see a truncated file.
        nlohmann::json back;
        { std::ifstream in(f); chk(bool(in), "file exists immediately after"); in >> back; }
        chk(back == cfg, "content is COMPLETE the moment the write returns");
        chk(back["blink_anim"]["expressions"]["happy"]["frames"] == 3,
            "...including the per-expression map that used to vanish");
        chk(!fs::exists(fs::path(f.string() + ".tmp")),
            "the temp file is renamed away, not left behind");
        // A path that cannot be created must FAIL, so the caller skips its
        // reload rather than loading stale data and believing it.
        chk(!face::write_json_atomic(fs::path("/proc/nope/deeper/x.json"), cfg),
            "unwritable path returns false (caller must not then reload)");
        fs::remove_all(dir);
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
