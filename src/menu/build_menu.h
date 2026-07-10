#pragma once
// ── build_menu.h ──────────────────────────────────────────────────────────────
// The deep-menu builder, extracted from main.cpp. MenuBuildContext carries
// exactly the parameters build_menu() used to take (same names — the tab
// builders open with an alias block so the moved bodies compile unchanged).
// The context only needs to outlive the build call itself; the produced
// MenuItem tree owns (by value / shared_ptr) everything it needs afterwards.

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <GLES2/gl2.h>
#include <nlohmann/json.hpp>

#include "app_state.h"
#include "face/shm_pusher_output.h"
#include "menu/menu_system.h"

// Forward declarations — the context only holds pointers to these.
namespace cv { class Mat; }
class IFaceController;
namespace face { class ExpressionDirector; }
class XRDisplay;
class CameraManager;
class LoRaRadio;
class SmartKnob;
class AudioEngine;
class AndroidMirror;
class Mpu9250;
class Bno055;
class Bno08x;
class BtMonitor;
class ProfileManager;
class BackgroundLibrary;
struct HudColors;
struct HudConfig;
enum class EyeSource;
namespace sensor       { class BoopSensor; }
namespace audio        { class VoiceAnalyzer; }
namespace accessory    { class AccessoryLeds; }
namespace sys          { class FanController; }
namespace input        { struct GpioPinCfg; struct CoprocConfig; }
namespace integrations { class KdeConnectBridge; }
namespace face         { struct GlitchConfig; struct ScrollTextConfig; class ReactionEngine; class ReactionRules; }

// HUB75 panel layout state. Lives here (not main.cpp) because both the menu
// (HUB75 Layout editor) and main's renderer-rebuild path use it.
struct PfHub75Layout {
    // Default size applied to every panel slot. Per-slot overrides in
    // panel_size_per (empty string = "use default") let a build mix
    // sizes — e.g. two 64x32 eye panels plus a 64x64 mouth.
    std::string panel_size      = "64x32";
    std::string arrangement     = "horizontal"; // horizontal / vertical / grid2x2
    int         panel_count     = 1;            // 1..4
    // HUB75 bonnet wiring → piomatter pinout + geometry (scripts/panel_driver.py).
    // "adafruit_bonnet" = the single-connector Adafruit RGB Matrix Bonnet/HAT
    // (default; plain serpentine-chained geometry). "active3" = the triple-
    // connector Active-3 board (multilane mapper). *_bgr variants swap R/B.
    std::string pinout          = "adafruit_bonnet";
    // Panel color-channel order ("auto" = the pinout's default; or one of
    // rgb/rbg/grb/gbr/brg/bgr to match oddly-wired panels — red/green
    // swapped is usually fixed by "grb", red/blue by "bgr"). Passed to
    // panel_driver.py as --order; applied on driver (re)launch.
    std::string color_order     = "auto";
    // Camera-friendly mode (piomatter): drives the panel with extra temporal
    // dithering / bit planes so the face reads cleanly on video (less banding).
    // A "tune and test" knob — piomatter's PIO refresh is already stable.
    bool        camera_mode            = false;
    int         camera_planes          = 10;  // PWM bit planes in camera mode
    int         camera_temporal_planes = 8;   // temporal-dither planes in camera mode
    std::string panel_size_per[4] = {"", "", "", ""};
    // Nudge stores each panel's CENTRE as an offset from the canvas centre
    // (in canvas pixels). Default = auto-placed by apply_defaults() per
    // arrangement; e.g. for two 64×32 panels in a horizontal chain the
    // defaults are dx[0] = -32, dx[1] = +32 (P1 left of centre, P2 right).
    int         nudge_dx[4]     = {0, 0, 0, 0};
    int         nudge_dy[4]     = {0, 0, 0, 0};
    // Per-panel orientation flips to match physical mounting/wiring. flip_x
    // mirrors left-right, flip_y top-bottom (both = 180°). Independent of the
    // nudge geometry — applied to each panel's composited region.
    bool        flip_x[4]       = {false, false, false, false};
    bool        flip_y[4]       = {false, false, false, false};
    // First-run flag — until apply_defaults has run we treat all-zero
    // nudges as "uninitialised" and populate them.
    bool        defaults_applied = false;
};

// ── MAX7219 panel layout (editor state) ──────────────────────────────────────
// Friendly front-end to Max7219Chain's module_positions: a ragged grid of 8×8
// modules the user builds in Face Display > MAX7219 Layout. rows[r] = how many
// panels sit in row r (so rows.size() = row count and each row's width is
// independent). Applied by rebuilding cfg["protoface"]["max7219"] and hot-
// swapping the panel output. Driven over the coprocessor (transport "coproc").
struct PfMax7219Layout {
    std::vector<int> rows { 4 };                // panels per row, in order top→bottom
    std::string chain_order = "serpentine";     // serpentine | row_major (DIN→DOUT walk)
    std::string module_type = "fc16";           // fc16 | generic1088
    std::string mode        = "section";        // section (beside HUB75) | main (the face)
    std::string content     = "face";           // section content: face (mirror) | symbols
    int         coproc_cs   = 0;                 // → firmware kMaxCsPins[coproc_cs]
    int         intensity   = 6;                 // 0..15
    int         canvas_x    = 0;                 // top-left of the block on the canvas
    int         canvas_y    = 0;
    bool        enabled     = false;
};

// Walk the ragged grid into per-module {x, y} canvas origins in DIN→DOUT (daisy)
// order — exactly what Max7219Chain::Config::module_positions wants, and what
// the wiring diagram numbers. Serpentine reverses every other row (boustrophedon)
// so the physical return wire on each row is short.
inline std::vector<std::array<int, 2>> pf_max7219_modules(const PfMax7219Layout& L) {
    std::vector<std::array<int, 2>> mods;
    for (int r = 0; r < static_cast<int>(L.rows.size()); ++r) {
        const int n = std::clamp(L.rows[r], 0, 32);
        for (int c = 0; c < n; ++c) {
            const int col = (L.chain_order == "serpentine" && (r & 1)) ? (n - 1 - c) : c;
            mods.push_back({L.canvas_x + col * 8, L.canvas_y + r * 8});
        }
    }
    return mods;
}

// ── Custom multi-colour gradient material ───────────────────────────────────
// Editor state for Protoface > Material Color > Custom Gradient. Up to 6 colour
// stops laid out along the face, smoothly blended or hard-banded, optionally
// scrolling so the colours flow behind the face. Serialised to
// cfg["protoface"]["gradient"]; rendered via face::GradientMaterial (the
// "gradient:…" material spec built by pf_gradient_spec below).
struct PfGradient {
    int count = 3;                                   // active stops, 2..6
    std::array<std::array<int, 3>, 6> colors {{
        {{0, 220, 180}}, {{0, 100, 255}}, {{180, 30, 220}},
        {{255, 80, 0}},  {{30, 220, 60}}, {{255, 220, 0}},
    }};
    bool        smooth    = true;                    // blend vs hard bands
    int         angle     = 0;                        // rotation, degrees (0 = →, 90 = ↓)
    int         speed     = 0;                        // px/s, 0 = static
    bool        mirror    = true;                     // reflect the ramp about the axis centre
};

// Build the "gradient:<dir>:<mode>:<speed>:RRGGBB-…" spec face::load_material
// parses. Clamped to 2..6 stops. The direction is an angle token "a<deg>", with
// a trailing 'm' to mirror the ramp about the centre.
inline std::string pf_gradient_spec(const PfGradient& g) {
    std::string s = "gradient:a";
    s += std::to_string(((g.angle % 360) + 360) % 360);
    if (g.mirror) s += 'm';
    s += ':';
    s += g.smooth ? 's' : 'b';
    s += ':';
    s += std::to_string(g.speed);
    s += ':';
    const int n = std::clamp(g.count, 2, 6);
    char buf[8];
    for (int i = 0; i < n; ++i) {
        if (i) s += '-';
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X",
                      g.colors[i][0] & 0xFF, g.colors[i][1] & 0xFF,
                      g.colors[i][2] & 0xFF);
        s += buf;
    }
    return s;
}

// ── Shared helpers defined in main.cpp ───────────────────────────────────────
// Used by both the menu builders and main's render loop / startup path.
void apply_hud_dock(AppState& s);
std::vector<face::ShmPusherOutput::Panel> pf_hub75_panels(const PfHub75Layout& L);
void pf_hub75_canvas(const PfHub75Layout& L, int& cw, int& ch);
void pf_hub75_apply_defaults(PfHub75Layout& L);
void qr_write_index(const std::string& qr_dir, const QrCaptureLog& log);

// ── Face version helpers ──────────────────────────────────────────────────────
// Saved versions of an expression's PNG live in hidden sibling dirs so the
// FaceLoader (which scans top-level *.png) never lists them as expressions:
//   <folder>/.versions/<expr>/<name>.png   named, kept until deleted
//   <folder>/.history/<expr>/<ts>.png      auto-backups, ring-buffered
namespace fvers {
namespace ffs = std::filesystem;
inline ffs::path vdir(const std::string& expr_png, const char* kind) {
    ffs::path p(expr_png);
    return p.parent_path() / kind / p.stem();
}
inline std::string stamp() {
    char b[32]; time_t t = time(nullptr);
    strftime(b, sizeof(b), "%Y%m%d-%H%M%S", localtime(&t));
    static int seq = 0;
    char s[44]; std::snprintf(s, sizeof(s), "%s-%03d", b, (seq++) % 1000);
    return s;
}
// Copy the live PNG into .history before it's overwritten; prune to `keep` newest.
inline void backup_current(const std::string& expr_png, int keep) {
    std::error_code ec;
    if (!ffs::exists(expr_png, ec)) return;
    ffs::path d = vdir(expr_png, ".history");
    ffs::create_directories(d, ec);
    ffs::copy_file(expr_png, d / (stamp() + ".png"),
                   ffs::copy_options::overwrite_existing, ec);
    std::vector<ffs::path> f;
    for (auto& e : ffs::directory_iterator(d, ec))
        if (e.path().extension() == ".png") f.push_back(e.path());
    std::sort(f.begin(), f.end());   // names are timestamps → chronological
    for (int i = 0; i + keep < static_cast<int>(f.size()); ++i) ffs::remove(f[i], ec);
}
inline bool save_named(const std::string& expr_png, std::string name) {
    std::error_code ec;
    if (!ffs::exists(expr_png, ec)) return false;
    for (auto& c : name) if (c == '/' || c == '\\' || c == ':') c = '_';
    if (name.empty()) return false;
    ffs::path d = vdir(expr_png, ".versions");
    ffs::create_directories(d, ec);
    ffs::copy_file(expr_png, d / (name + ".png"),
                   ffs::copy_options::overwrite_existing, ec);
    return !ec;
}
struct Entry { ffs::path path; std::string label; bool named = false; };
inline std::vector<Entry> list(const std::string& expr_png) {
    std::vector<Entry> out; std::error_code ec;
    std::vector<ffs::path> named;
    for (auto& e : ffs::directory_iterator(vdir(expr_png, ".versions"), ec))
        if (e.path().extension() == ".png") named.push_back(e.path());
    std::sort(named.begin(), named.end());
    for (auto& p : named) out.push_back({p, p.stem().string(), true});
    std::vector<ffs::path> hist;
    for (auto& e : ffs::directory_iterator(vdir(expr_png, ".history"), ec))
        if (e.path().extension() == ".png") hist.push_back(e.path());
    std::sort(hist.rbegin(), hist.rend());   // newest first
    for (auto& p : hist) out.push_back({p, p.stem().string(), false});
    return out;
}
} // namespace fvers

// ── MenuBuildContext ──────────────────────────────────────────────────────────
// One member per former build_menu() parameter, with the same name and the
// same semantics. Pointer members point at objects owned by main() that
// outlive the menu; std::function members are copied into the items that
// need them. Defaults mirror the old parameter defaults.
struct MenuBuildContext {
    IFaceController* teensy  = nullptr;
    // Custom-expression runtime (activation/hold/restore) — face/expression_director.h.
    face::ExpressionDirector* expr_director = nullptr;
    // Show the legacy ProtoTracer/Teensy source picker + submenu (hidden by
    // default since the Face Display redesign; protoface.show_prototracer).
    bool show_prototracer = false;
    XRDisplay*       xr      = nullptr;
    CameraManager*   cameras = nullptr;
    LoRaRadio*       lora    = nullptr;
    SmartKnob*       knob    = nullptr;
    AudioEngine*     audio   = nullptr;
    AppState*        state   = nullptr;
    AndroidMirror*   android_mirror  = nullptr;
    bool*            android_overlay = nullptr;
    OverlayConfig*   pip_cfg1 = nullptr;
    OverlayConfig*   pip_cfg2 = nullptr;
    OverlayConfig*   pip_cfg3 = nullptr;
    bool*            pip_cam1_overlay = nullptr;
    bool*            pip_cam2_overlay = nullptr;
    bool*            pip_cam3_overlay = nullptr;
    OverlayConfig*   android_cfg = nullptr;
    HudColors*       hud_col = nullptr;
    HudConfig*       hud_cfg = nullptr;
    MenuSystem**     menu_sys_pp = nullptr;
    Mpu9250*         mpu9250 = nullptr;
    Bno055*          bno055  = nullptr;
    Bno08x*          bno08x  = nullptr;
    const std::vector<std::string>* gif_names = nullptr;
    BtMonitor*       bt_mon = nullptr;
    bool*            sys_panel_active   = nullptr;
    bool*            fps_overlay_active = nullptr;
    AppState*        state_ptr = nullptr;
    // Face Source switching — pass null fp_option to hide Protoface entry
    IFaceController** active_face_pp = nullptr;
    IFaceController*  teensy_option  = nullptr;
    IFaceController*  fp_option      = nullptr;
    // Panel preview toggle (Protoface shm → ProtoHUD ImGui window)
    bool*             panel_preview_pp = nullptr;
    // Panel preview placement (anchor/nudge/size, like the camera PiPs)
    OverlayConfig*    protoface_preview_cfg = nullptr;
    // Panel preview view mode: 0=whole face, 1=left half, 2=right half
    int*              protoface_preview_view_pp = nullptr;
    std::string       map_dir = "/home/user/Pictures/protohud/maps";
    // Eye source selection (render-thread only, no mutex needed)
    EyeSource* left_eye_src  = nullptr;
    EyeSource* right_eye_src = nullptr;
    // Multi-cam quad layout: both CSI cameras (rotated, side-by-side) on
    // the top half, two USB cameras side-by-side on the bottom, the same
    // composite in both eyes. Toggle + which-two-USB pickers.
    bool*      multicam_layout = nullptr;
    EyeSource* multicam_usb_a  = nullptr;
    EyeSource* multicam_usb_b  = nullptr;
    EyeSource* multicam_top_a  = nullptr;
    EyeSource* multicam_top_b  = nullptr;
    // Profile management (Profiles tab: save current / load by restart / delete)
    ProfileManager* profiles = nullptr;
    // HUD/menu visual presets (System tab: built-in themes + save/load/delete)
    ProfileManager* hud_presets = nullptr;
    // Out: curated corner "quick menu" tree (assigned if non-null)
    std::vector<MenuItem>* quick_out = nullptr;
    // GIF folder for the Animations preview (scan order matches play_gif index)
    std::string gifs_dir;
    // Landing-page background library (set after construction; pointer-to-pointer
    // so the menu can capture it before bg_lib exists, same pattern as menu_sys_pp)
    BackgroundLibrary** bg_lib_pp = nullptr;
    // User-writable backgrounds folder ($HOME/protohud/backgrounds). Imports
    // land here; bundled defaults under assets/backgrounds are read-only.
    std::string bg_user_dir;
    // Boop sensor (set after construction; same pointer-to-pointer pattern).
    // Menu toggles/sliders forward live changes via the BoopSensor interface
    // so the next poll cycle picks up the new threshold / enable state.
    sensor::BoopSensor** boop_sensor_pp = nullptr;
    // Voice analyzer (owned by AudioEngine; main passes its address). Menu
    // sliders write through it so the next FFT cycle uses the new params.
    audio::VoiceAnalyzer* voice_analyzer = nullptr;
    // Accessory LED chain (cheekhubs + fins). Menu toggles/sliders push
    // through its zone setters so the next render tick uses them.
    accessory::AccessoryLeds* leds = nullptr;
    sys::FanController* fans = nullptr;
    // Hot-swap callback for Protoface > Hardware > Backend; main wires it
    // to the tear-down-and-rebuild routine that swaps NativeFaceController
    // and panel_driver.py for the new backend. pf_backend_p is the live
    // backend name string for the radio's get_state.
    std::function<void(const std::string&)> swap_backend;
    const std::string* pf_backend_p = nullptr;
    // Edit… callback for Files > Faces > <expr> > Edit. Main wires it
    // to a routine that polls the native controller for canvas dims +
    // covered chain regions, opens the face editor, writes the PNG on
    // commit, and reloads the face.
    std::function<void(const std::string& expression)> edit_face;
    // Chain layout pickers — pointers so the radios can read the live
    // value and mutate it in place. Used by the MAX7219 / RGB matrix
    // editor to draw labelled eye / nose / mouth zones.
    std::string* pf_eye_layout_p   = nullptr;
    std::string* pf_mouth_layout_p = nullptr;
    std::string* pf_nose_layout_p  = nullptr;
    // HUB75 panel layout (pickers + nudges). Owned by main, mutated by
    // the menu; main rebuilds the renderer when the user backs out of
    // the HUB75 Layout submenu (Phase 3) — for now changes take effect
    // on the next backend hot-swap.
    PfHub75Layout* pf_hub75_p = nullptr;
    // Named HUB75 layouts — Save As / Load / Rename / Delete management.
    // pf_hub75_p above is the *working copy* of the active entry; these
    // pointers expose the map + selection so the menu can swap which
    // layout is being edited. pf_layout_changed runs after every
    // mutation (active swap, save-as, rename, delete) so the controller
    // can pick up the new active-layout name for face-folder stamping.
    std::map<std::string, PfHub75Layout>* pf_hub75_layouts_p = nullptr;
    std::string* pf_hub75_active_p = nullptr;
    std::function<void()> pf_layout_changed;
    // MAX7219 panel layout editor (Face Display > MAX7219 Layout). pf_max7219_p
    // is the working copy; pf_max7219_apply serialises it into
    // cfg["protoface"]["max7219"] and hot-swaps the panel output so the change
    // (and the coproc-driven "section" beside HUB75) takes effect live.
    PfMax7219Layout* pf_max7219_p = nullptr;
    std::function<void()> pf_max7219_apply;
    // Trigger content on the MAX7219 "section" panels (content:"symbols"). kind =
    // symbol|text|pattern|clear|next|prev; value = the symbol/pattern/text.
    std::function<void(const std::string& kind, const std::string& value)> pf_max_content;
    // Face animation tunables — pointers + a "push live" callback that
    // forwards the current values into native_ctrl after a slider/toggle
    // change. Caller owns the slots and the persistence to config.json.
    bool*   pf_blink_enabled_p = nullptr;
    double* pf_blink_min_p     = nullptr;
    double* pf_blink_max_p     = nullptr;
    double* pf_blink_dur_p     = nullptr;
    double* pf_expr_fade_p     = nullptr;
    // Editor "V (Preview to panels)" hold time. No live-push callback —
    // the value is read each time the editor opens, so changes take
    // effect on the next Edit… launch.
    double* pf_preview_duration_p = nullptr;
    std::function<void()> pf_anim_push;
    // Pushes an arbitrary particle-system spec (a {"layers":[...]} dict
    // or single-effect spec) directly to the native renderer, bypassing
    // the effect_id mapping. Used by the Layered Effects builder so the
    // user can compose multi-layer particle configs at runtime.
    std::function<void(const nlohmann::json&)> pf_set_effect_json;
    // Reads back the renderer's current particle spec (the effect actually
    // running, restored from protoface_state.json at boot). Lets the Layered
    // builder seed its editable fields from what's live instead of showing
    // empty default layers. Null on non-native backends.
    std::function<nlohmann::json()> pf_get_effect_json;
    // Toggle expression-coupled effects (mood preset follows the face) and
    // the config-backed flag the menu reads. Null on non-native backends.
    std::function<void(bool)> pf_set_expr_effects;
    bool* pf_expr_effects_p = nullptr;
    // "Alive" reactive pack toggles (Face > Effects). Motion Reactive couples
    // directional effects to real gravity/turn-sweep; Weather Sync overrides
    // the ambient effect from live conditions (mapped in main's render loop).
    std::function<void(bool)> pf_set_motion_particles;
    bool* pf_motion_particles_p = nullptr;
    // Face Inertia - the whole face slides opposite quick head motion and
    // springs back like it has mass. Strength scales the maximum slide
    // (1.0 = up to ~10% of the panel).
    std::function<void(bool)>   pf_set_face_inertia;
    bool*                       pf_face_inertia_p = nullptr;
    std::function<void(double)> pf_set_face_inertia_strength;
    double*                     pf_face_inertia_strength_p = nullptr;
    // Global IMU->face motion sensitivity (see main's pf_motion_scale). Read
    // by the feed every frame - the menu just mutates it in place.
    double*                     pf_motion_scale_p = nullptr;
    // Guided IMU motion-range calibration (main owns the wizard). start
    // begins a run; while one is active the same call captures the current
    // pose and advances (press-driven). cancel aborts; status returns a
    // short progress string for the menu row ("" = idle).
    std::function<void()>        imu_cal_start;
    std::function<void()>        imu_cal_cancel;
    std::function<std::string()> imu_cal_status;
    // Floating-window terminal: spawn a fresh shell (HUD > Floating Window >
    // Restart Terminal). Main owns the PtyTerminal.
    std::function<void()>        term_restart;
    std::function<void(bool)> pf_set_weather_effects;
    bool* pf_weather_effects_p = nullptr;
    // Temp Effects - ambient frost / heat shimmer driven by the live outdoor
    // temperature. Thresholds are Celsius. pf_ambient_resync tells main's
    // render loop to re-map the ambient effect immediately after a toggle or
    // threshold change (same path Weather Sync uses).
    bool*   pf_temp_effects_p = nullptr;
    double* pf_temp_cold_p    = nullptr;
    double* pf_temp_hot_p     = nullptr;
    bool*   pf_frost_fractal_p  = nullptr;   // frost fractal ferns + big snowflakes
    bool*   pf_heat_heartbeat_p = nullptr;   // heatwave orange heartbeat rim pulse
    // Transient preview override (not persisted): 0 = off (follow temperature),
    // 1 = force frost, 2 = force heatwave. Lets the wearer eyeball the temp
    // effects on the bench without waiting for the threshold to be crossed.
    int*    pf_temp_force_p     = nullptr;
    std::function<void()> pf_ambient_resync;
    // Live-preview tick: main calls this each frame; when Live Preview is on
    // it re-applies the builder spec on change. Installed inside build_menu.
    std::shared_ptr<std::function<void()>> pf_live_tick;
    // The live cfg JSON object owned by main(). Used by the GPIO
    // Visualizer (pin-claim scan, I²C peripherals, user notes,
    // rail-current estimate) and the Layered Effects builder
    // (Save / Load slots persisted under
    // cfg["protoface"]["custom_effects"]).
    nlohmann::json* cfg_root = nullptr;
    // USB camera live-preview wiring: GL texture handles sampled by the camera
    // image-setting context panels, plus a per-frame "preview request" the
    // panels set (1/2/3) so the render loop opens the stream, hides the
    // on-screen PiP, and shows the feed in the context pane while adjusting.
    GLuint* tex_usb1 = nullptr;
    GLuint* tex_usb2 = nullptr;
    GLuint* tex_usb3 = nullptr;
    int*    usb_preview_req = nullptr;
    // Custom Gradient material editor (Protoface > Material Color). The
    // working copy lives in main(); pf_set_material pushes the built
    // "gradient:…" spec to the live renderer for instant preview.
    PfGradient* pf_gradient_p = nullptr;
    std::function<void(const std::string&)> pf_set_material;
    // Restart the native HUB75 panel pusher (scripts/panel_driver.py) to
    // recover the face feed after a GPIO conflict, without restarting all of
    // ProtoHUD. No-op outside native HUB75 mode.
    std::function<void()> pf_restart_renderer;
    // Configurable GPIO switch map: array of assignable pin slots + count,
    // and the master enable flag. Edited in the GPIO Buttons menu; applied
    // on the next launch (the poller is built from these at startup).
    input::GpioPinCfg* gpio_pins_p = nullptr;
    int gpio_slot_count = 0;
    bool* gpio_inputs_enabled_p = nullptr;
    // Rebuilds the GPIO poller from the live slots so menu edits apply
    // without a relaunch. Shared so main can install it after the poller
    // (which the menu is built before) exists.
    std::shared_ptr<std::function<void()>> gpio_reload;
    integrations::KdeConnectBridge* kdc_p = nullptr;
    std::vector<std::string>* kdc_ignore_p = nullptr;
    std::vector<std::string>* kdc_msg_p = nullptr;
    // Optional button/switch coprocessor (input::CoprocInputs). enabled flag
    // + a live status string getter + a reload hook, mirroring the GPIO
    // poller's shared-after-construction pattern. All null when the build
    // has no coprocessor support wired.
    bool* coproc_enabled_p = nullptr;
    std::shared_ptr<std::function<void()>> coproc_reload;
    std::shared_ptr<std::function<std::string()>> coproc_status;
    // Coprocessor I²C bus test: trigger a scan of its I²C lines, and read back the
    // last result (address list / "none" / "scanning…"). See CoprocInputs.
    std::function<void()>        coproc_i2c_scan;
    std::function<std::string()> coproc_i2c_result;
    // Peripheral TEST verbs (pre-assigned test pins on the coprocessor —
    // servos, WS2812 zone, ADC). Null when not wired.
    std::function<void(int, int)>           coproc_servo;      // ch, deg (-1 = off)
    std::function<void(int, int, int, int)> coproc_led_zone;   // r,g,b,count(-1=default)
    std::function<void(int, int, int, int, int)> coproc_led_pattern; // mode,r,g,b,speed
    std::function<void(int)>                coproc_led_bright; // 0-255
    std::function<void()>                   coproc_adc_read;
    std::function<std::string()>            coproc_adc_result;
    // Live coprocessor config (pins + button maps) for the Pins visualizer/editor.
    // Edited in place, then persisted to cfg["inputs"]["coprocessor"] and re-pushed
    // via coproc_reload.
    input::CoprocConfig* coproc_cfg_p = nullptr;
    // When set, build_system_menu routes its GPIO items here instead of into the
    // System menu, so build_menu can assemble the top-level "GPIO" tab:
    //   gpio_onboard_out  ← Pi 40-pin visualizer + on-board GPIO button map
    //   gpio_expander_out ← RP2350 coprocessor (enable/status + Pico pin editor)
    std::vector<MenuItem>* gpio_onboard_out  = nullptr;
    std::vector<MenuItem>* gpio_expander_out = nullptr;
    // When set, build_hud_menu routes its IMU group (source picker, recenter,
    // axis map, calibration, restart) here instead of nesting it under
    // HUD > Compass, so build_menu can place it in the On-Board GPIO section
    // (the IMU chips hang off the 40-pin header's I²C pins).
    std::vector<MenuItem>* imu_out = nullptr;
    // Glitch post-effect config (null on non-native backends). The menu
    // mutates it in place and re-pushes via pf_anim_push().
    face::GlitchConfig* pf_glitch_p = nullptr;
    // Scrolling-text banner config — same contract as pf_glitch_p (mutate in
    // place, re-push via pf_anim_push()).
    face::ScrollTextConfig* pf_scroll_p = nullptr;
    // Reaction engine (environment/movement reactions; main owns it). The
    // menu edits its Config via set_config and calls the force_* test hooks.
    face::ReactionEngine* reactions = nullptr;
    face::ReactionRules*  reaction_rules = nullptr;

    // ── Build-phase shared fragments ──────────────────────────────────────────
    // NOT caller-supplied: set by build_menu() before the tab builders run
    // (shared helpers), or by an earlier tab builder for a later one
    // (cross-tab menu fragments).
    //
    // Thread-safe notification push (was a build_menu local lambda shared by
    // the LoRa menu and the System tab's Demo Mode / Updates). Tab builders
    // re-wrap it locally to restore the optional trailing actions argument.
    std::function<void(NotifType, std::string, std::string, float,
                       std::vector<NotifAction>)> push_notif;
    // Built-in coordinated HUD/menu theme leaves — the same quick-apply list
    // lives under HUD (Themes and Effects) and System > HUD/Menu Presets.
    std::function<std::vector<MenuItem>()> make_builtin_theme_leaves;
    // Phone (KDE Connect) item: built by the Communications builder, shown
    // under System > Connectivity (alongside SSH/Bluetooth).
    MenuItem phone_item;
    // GIF slot machinery, constructed in build_menu() around one shared
    // animated-preview state. draw_gif_preview is safe to keep in items
    // (captures by value / shared_ptr); gif_leaf and gif_slot_row are
    // BUILD-PHASE ONLY — they capture build_menu locals by reference and
    // must not be called after the build returns.
    MenuContextPanelDraw draw_gif_preview;          // animated slot preview panel
    std::function<MenuItem(uint8_t)> gif_leaf;      // play/import leaf for slot i
    std::function<MenuItem(uint8_t)> gif_slot_row;  // Files management row for slot i
    // Live rendered face canvas (CV_8UC3 RGB, face + material + effects) for the
    // Effects context-panel preview. Returns false until the first frame exists.
    std::function<bool(cv::Mat&)> live_face_frame;
};

// Builds the full deep-menu tree (six top-level tabs) and, when
// ctx.quick_out is set, the curated corner/radial quick menu too.
std::vector<MenuItem> build_menu(MenuBuildContext& ctx);

// Per-tab builders (one TU each; called by build_menu in dependency order).
std::vector<MenuItem> build_vision_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_hud_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_face_display_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_files_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_communications_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_system_menu(MenuBuildContext& ctx);
std::vector<MenuItem> build_quick_menu(MenuBuildContext& ctx);
