// ── build_face_display.cpp ────────────────────────────────────────────────────
// The Face Display tab, moved verbatim out of build_menu(): ProtoTracer
// (faces/colors/palette/animations), the Protoface stack (face options,
// effects + layered builder, material/gradient, hardware backend, HUB75
// layout, size & position, panel preview), and the boop / light / voice /
// accessory-LED drivers.

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <random>
#include <cctype>
#include <ctime>
#include <csignal>
#include <cstdlib>

#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <linux/videodev2.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>

#include "app_state.h"
#include "android/android_mirror.h"
#include "camera/camera_manager.h"
#include "camera/viture_camera.h"
#include "input/gpio_buttons.h"
#include "input/gpio_inputs.h"
#include "input/coproc_inputs.h"
#include "input/cmd_fifo.h"
#include "input/gpio_function.h"
#include "input/gamepad_input.h"
#include "input/wireless_controller.h"
#include "serial/face_controller.h"
#include "serial/protoface_controller.h"
#include "serial/teensy_controller.h"
#include "serial/lora_radio.h"
#include "serial/smartknob.h"
#include "protocols.h"
#include "hud/hud_renderer.h"
#include "menu/menu_system.h"
#include "vitrue/xr_display.h"
#include "vitrue/timewarp.h"
#include "audio/audio_engine.h"
#include "post_process.h"
#include "sensor/mpu9250.h"
#include "sensor/bno055.h"
#include "sensor/light_sensor.h"
#include "sensor/mpr121_boop_sensor.h"
#include "accessory/accessory_leds.h"
#include "sys/fan_controller.h"
#include "sys/system_monitor.h"
#include "sys/scheduler_monitor.h"
#include "sys/gpio_pinmap.h"
#include "sys/gpio_input_reader.h"
#include "net/weather_monitor.h"
#include "net/wifi_monitor.h"
#include "net/ping_monitor.h"
#include "net/bt_monitor.h"
#include "crash_reporter.h"
#include "capture.h"
#include "gl_async_read.h"
#include "video_recorder.h"
#include "qr_scanner.h"
#include "splash.h"
#include "integrations/kdeconnect_bridge.h"   // header is dbus-free; .cpp guarded by HAVE_DBUS in CMake
#include "integrations/phone_inbox.h"
#include "hud/background_library.h"
#include "profile_manager.h"
#include "face/face_config.h"
#include "face/face_image.h"
#include "face/eye_animations.h"
#include "face/gif_player.h"
#include "face/native_face_controller.h"
#include "face/reaction_engine.h"
#include "face/panel_output.h"
#include "face/shm_pusher_output.h"
#include "face/max7219_panel_output.h"
#include "face/max_section_content.h"
#include "face/neopixel_matrix_output.h"

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/file.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

using json = nlohmann::json;

#include "menu/build_menu.h"
#include "menu/item_factories.h"
#include "menu/shared_items.h"

// One layer in the Protoface Effects > Layered Builder. State lives in a
// file-scope struct so kMaxLayers can be a static constexpr (forbidden on a
// local class). The menu's static LayeredEffectState instance persists for
// the program's lifetime.
struct LayerCfg {
    std::string effect = "none";   // "none" disables the slot
    int   count = 20;              // particle count / density
    int   r = 255, g = 255, b = 255;
    float speed_min = 5.f;
    float speed_max = 15.f;
    // Direction of motion, degrees (0 = right, 90 = down, 180 = left,
    // 270 = up). Honoured by snow / rain / embers / confetti; stationary
    // and radial effects ignore it and the slider is hidden in their UI.
    // -1 means "use the effect's historical default."
    float direction_deg = -1.f;
    // Liquid fill fraction for the "water" effect (0..1). Ignored by others.
    float level = 0.4f;
    // Liquid opacity for "water" (0 = fully transparent, 1 = solid). The face
    // and background read through the liquid as this drops.
    float alpha = 0.85f;
    // Liquid viscosity for "water" (0 = thin/snappy, 1 = thick/sluggish).
    float viscosity = 0.15f;
    // How strongly the submerged face glows back through the liquid, tinted
    // by it (0 = opaque liquid). Water only.
    float face_glow = 0.55f;
    // "water" extras: pitch shifts the fill level (look down → liquid rises);
    // bubbles count + style ("rise" bubbles in liquid / "drip" droplets above).
    float pitch_fill = 0.0f;
    int   bubbles = 0;
    std::string bubble_mode = "rise";
    // "lightning" extras: arc mode (crackling arcs vs falling bolts), fork
    // density, and random origin (bolts strike from anywhere instead of the
    // directional edge; bolt mode only).
    bool  arc = false;
    bool  random_origin = false;
    float branches = 0.35f;
    std::string blend = "add";     // "add" | "normal" | "multiply" | "screen"
    // Motion reactivity (opt-in): drives the layer's direction from head
    // movement. "none" | "heading" (lock to compass) | "yaw" (drift when
    // turning) | "tilt" (skew like gravity when rolling the head).
    std::string direction_from = "none";
    // Density reactivity (opt-in): scales particle count from a live signal.
    // "none" | "audio" (mic level) | "yaw_rate" | "accel".
    std::string intensity_from = "none";
};
struct LayeredEffectState {
    static constexpr int kMaxLayers = 5;
    LayerCfg layers[kMaxLayers];
};

// Canonical expression slot list for the Files > Faces hub. Matches the
// standard whole-face layout shipped in the Protoface repo (faces/main +
// faces/example_fox config.json): neutral + happy/angry/sad/surprised, plus
// the blink eyelid overlay and the optional mouth-open speech state.
namespace {
struct FaceSlot { const char* expression; const char* label; };
constexpr FaceSlot kFaceSlots[] = {
    {"neutral",   "Neutral"},
    {"happy",     "Happy"},
    {"angry",     "Angry"},
    {"sad",       "Sad"},
    {"surprised", "Surprised"},
    {"squint",    "Squint"},
    {"sleepy",    "Sleepy"},      // heavy-lidded (drowsy reaction)
    {"asleep",    "Asleep"},      // eyes closed (sleep reaction, Z's on top)
    {"blink",     "Blink"},
};
constexpr int kFaceSlotCount = sizeof(kFaceSlots) / sizeof(kFaceSlots[0]);

// Mouth-shape overlays (visemes). Not expressions in their own right — they
// blend on top of whichever expression is active when the voice analyzer
// drives mouth_open > 0 and (later) viseme selection picks one of these
// shapes based on spectral centroid. file_stem maps to faces/<active>/<stem>.png,
// canonicalised by face_image_path() inside NativeFaceController.
struct MouthShape { const char* file_stem; const char* label; };
constexpr MouthShape kMouthShapes[] = {
    {"mouth_small", "Small Open"},   // closed-vowel / M-N-D family
    {"mouth_open",  "Wide Open"},    // AH family (was the single mouth_open)
    {"mouth_smile", "Smile"},        // EE family
    {"mouth_round", "Round"},        // OOH family
};
constexpr int kMouthShapeCount = sizeof(kMouthShapes) / sizeof(kMouthShapes[0]);

// Boop reaction faces. The boop sensor's on_boop callback prefers these
// PNG names per zone when present in the active face folder; otherwise
// falls back to the user-configured expression in state.boop_zones.
// Files > Faces > Boop Reactions surfaces them as standard slot rows
// (Play / Edit / Replace / Clear / Import) so users can author them
// just like any other expression slot.
struct BoopFaceSlot { const char* file_stem; const char* label; };
constexpr BoopFaceSlot kBoopFaceSlots[] = {
    {"boop_snout", "Snout"},
    {"boop_left",  "Left Cheek"},
    {"boop_right", "Right Cheek"},
    {"boop_both",  "Both Cheeks"},
};
constexpr int kBoopFaceSlotCount = sizeof(kBoopFaceSlots) / sizeof(kBoopFaceSlots[0]);
} // namespace

// Open the face image picker for a given expression. On commit copies the
// chosen PNG into the active face folder (canonical filename, e.g. happy.png),
// rebuilds the loader so the face reflects the new image, and switches the
// live expression so the user sees the import immediately.
static void import_face_into_slot(MenuSystem* menu,
                                  IFaceController* teensy,
                                  std::string expression,
                                  std::string label) {
    if (!menu || !teensy) return;
    char title[64];
    std::snprintf(title, sizeof(title), "Import %s face PNG", label.c_str());
    std::string start = menu->file_picker_dir();
    menu->open_file_picker(
        title, std::move(start), {".png"},
        [teensy, expression = std::move(expression)](const std::string& src) {
            if (!teensy->import_face_image(expression, src)) {
                std::fprintf(stderr,
                             "[face] import failed for '%s' from '%s'\n",
                             expression.c_str(), src.c_str());
                return;
            }
            teensy->set_face_by_name(expression);
        });
}

// Dynamic label shared by every PNG-backed slot row (face / mouth / boop):
// "(empty)" when no PNG; "[<layout>]" when the saved PNG was stamped with a
// layout name that doesn't match the currently-active one. Untagged (legacy)
// faces show plain.
static std::function<std::string()> png_slot_label_fn(
        IFaceController* teensy, std::string expr, std::string label,
        const std::string* active_layout) {
    return [teensy, expr = std::move(expr), label = std::move(label),
            active_layout]() -> std::string {
        if (!teensy->face_image_exists(expr)) return label + " (empty)";
        const std::string tag = teensy->face_image_layout(expr);
        if (tag.empty() || !active_layout || tag == *active_layout)
            return label;
        return label + "  [" + tag + "]";
    };
}

// "Copy from..." submenu over a peer slot table — useful as a starting point
// ("clone 'happy' into 'wink' then tweak the eye"). peers holds every slot in
// the table as (expr, label); self is skipped here. Listed slots are gated
// per-row to hide empties; the submenu itself hides when there's nothing
// copyable. import_face_image reloads the controller so the new art shows
// immediately, matching the editor's commit path.
static MenuItem make_copy_from_submenu(
        IFaceController* teensy, const std::string& expr,
        std::vector<std::pair<std::string, std::string>> peers,
        std::string desc) {
    std::vector<MenuItem> copy_children;
    for (const auto& [src_expr, src_label] : peers) {
        if (src_expr == expr) continue;
        MenuItem ci = leaf(src_label,
            [teensy, src_expr = src_expr, expr]{
                const std::string src = teensy->face_image_path(src_expr);
                if (!src.empty()) teensy->import_face_image(expr, src);
            });
        ci.visible_fn = [teensy, src_expr = src_expr]{
            return teensy->face_image_exists(src_expr); };
        copy_children.push_back(std::move(ci));
    }
    MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
    copy_from.description = std::move(desc);
    copy_from.visible_fn = [teensy, expr, peers = std::move(peers)]{
        for (const auto& [peer_expr, peer_label] : peers) {
            (void)peer_label;
            if (peer_expr == expr) continue;
            if (teensy->face_image_exists(peer_expr)) return true;
        }
        return false;
    };
    return copy_from;
}

std::vector<MenuItem> build_face_display_menu(MenuBuildContext& ctx)
{
    // ── Parameter aliases ─────────────────────────────────────────────────────
    // Re-create build_menu's original parameter names as locals so the moved
    // body below stays byte-for-byte identical to its main.cpp version.
    // By-value parameters become local copies with the same lifetime they
    // always had (the duration of the build).
    IFaceController* teensy  = ctx.teensy;
    XRDisplay*       xr      = ctx.xr;
    CameraManager*   cameras = ctx.cameras;
    LoRaRadio*       lora    = ctx.lora;
    SmartKnob*       knob    = ctx.knob;
    AudioEngine*     audio   = ctx.audio;
    AppState&        state   = *ctx.state;
    AndroidMirror*   android_mirror  = ctx.android_mirror;
    bool*            android_overlay = ctx.android_overlay;
    OverlayConfig*   pip_cfg1 = ctx.pip_cfg1;
    OverlayConfig*   pip_cfg2 = ctx.pip_cfg2;
    OverlayConfig*   pip_cfg3 = ctx.pip_cfg3;
    bool*            pip_cam1_overlay = ctx.pip_cam1_overlay;
    bool*            pip_cam2_overlay = ctx.pip_cam2_overlay;
    bool*            pip_cam3_overlay = ctx.pip_cam3_overlay;
    OverlayConfig*   android_cfg = ctx.android_cfg;
    HudColors*       hud_col = ctx.hud_col;
    HudConfig*       hud_cfg = ctx.hud_cfg;
    MenuSystem**     menu_sys_pp = ctx.menu_sys_pp;
    Mpu9250*         mpu9250 = ctx.mpu9250;
    Bno055*          bno055  = ctx.bno055;
    const std::vector<std::string>& gif_names = *ctx.gif_names;
    BtMonitor*       bt_mon = ctx.bt_mon;
    bool*            sys_panel_active   = ctx.sys_panel_active;
    bool*            fps_overlay_active = ctx.fps_overlay_active;
    AppState*        state_ptr = ctx.state_ptr;
    IFaceController** active_face_pp = ctx.active_face_pp;
    IFaceController*  teensy_option  = ctx.teensy_option;
    IFaceController*  fp_option      = ctx.fp_option;
    bool*             panel_preview_pp = ctx.panel_preview_pp;
    OverlayConfig*    protoface_preview_cfg = ctx.protoface_preview_cfg;
    int*              protoface_preview_view_pp = ctx.protoface_preview_view_pp;
    std::string       map_dir = ctx.map_dir;
    EyeSource* left_eye_src  = ctx.left_eye_src;
    EyeSource* right_eye_src = ctx.right_eye_src;
    bool*      multicam_layout = ctx.multicam_layout;
    EyeSource* multicam_usb_a  = ctx.multicam_usb_a;
    EyeSource* multicam_usb_b  = ctx.multicam_usb_b;
    EyeSource* multicam_top_a  = ctx.multicam_top_a;
    EyeSource* multicam_top_b  = ctx.multicam_top_b;
    ProfileManager* profiles    = ctx.profiles;
    ProfileManager* hud_presets = ctx.hud_presets;
    std::vector<MenuItem>* quick_out = ctx.quick_out;
    std::string gifs_dir = ctx.gifs_dir;
    BackgroundLibrary** bg_lib_pp = ctx.bg_lib_pp;
    std::string bg_user_dir = ctx.bg_user_dir;
    sensor::BoopSensor** boop_sensor_pp = ctx.boop_sensor_pp;
    audio::VoiceAnalyzer* voice_analyzer = ctx.voice_analyzer;
    accessory::AccessoryLeds* leds = ctx.leds;
    sys::FanController* fans = ctx.fans;
    std::function<void(const std::string&)> swap_backend = ctx.swap_backend;
    const std::string* pf_backend_p = ctx.pf_backend_p;
    std::function<void(const std::string&)> edit_face = ctx.edit_face;
    std::string* pf_eye_layout_p   = ctx.pf_eye_layout_p;
    std::string* pf_mouth_layout_p = ctx.pf_mouth_layout_p;
    std::string* pf_nose_layout_p  = ctx.pf_nose_layout_p;
    PfHub75Layout* pf_hub75_p = ctx.pf_hub75_p;
    std::map<std::string, PfHub75Layout>* pf_hub75_layouts_p = ctx.pf_hub75_layouts_p;
    std::string* pf_hub75_active_p = ctx.pf_hub75_active_p;
    std::function<void()> pf_layout_changed = ctx.pf_layout_changed;
    bool*   pf_blink_enabled_p = ctx.pf_blink_enabled_p;
    double* pf_blink_min_p     = ctx.pf_blink_min_p;
    double* pf_blink_max_p     = ctx.pf_blink_max_p;
    double* pf_blink_dur_p     = ctx.pf_blink_dur_p;
    double* pf_expr_fade_p     = ctx.pf_expr_fade_p;
    double* pf_preview_duration_p = ctx.pf_preview_duration_p;
    std::function<void()> pf_anim_push = ctx.pf_anim_push;
    std::function<void(const nlohmann::json&)> pf_set_effect_json = ctx.pf_set_effect_json;
    std::function<nlohmann::json()> pf_get_effect_json = ctx.pf_get_effect_json;
    std::function<void(bool)> pf_set_expr_effects = ctx.pf_set_expr_effects;
    bool* pf_expr_effects_p = ctx.pf_expr_effects_p;
    std::function<void(bool)> pf_set_motion_particles = ctx.pf_set_motion_particles;
    bool* pf_motion_particles_p = ctx.pf_motion_particles_p;
    std::function<void(bool)> pf_set_face_inertia = ctx.pf_set_face_inertia;
    bool* pf_face_inertia_p = ctx.pf_face_inertia_p;
    std::function<void(double)> pf_set_face_inertia_strength =
        ctx.pf_set_face_inertia_strength;
    double* pf_face_inertia_strength_p = ctx.pf_face_inertia_strength_p;
    double* pf_motion_scale_p = ctx.pf_motion_scale_p;
    std::function<void(bool)> pf_set_weather_effects = ctx.pf_set_weather_effects;
    bool* pf_weather_effects_p = ctx.pf_weather_effects_p;
    bool*   pf_temp_effects_p = ctx.pf_temp_effects_p;
    double* pf_temp_cold_p    = ctx.pf_temp_cold_p;
    double* pf_temp_hot_p     = ctx.pf_temp_hot_p;
    std::function<void()> pf_ambient_resync = ctx.pf_ambient_resync;
    std::shared_ptr<std::function<void()>> pf_live_tick = ctx.pf_live_tick;
    nlohmann::json* cfg_root = ctx.cfg_root;
    GLuint* tex_usb1 = ctx.tex_usb1;
    GLuint* tex_usb2 = ctx.tex_usb2;
    GLuint* tex_usb3 = ctx.tex_usb3;
    int*    usb_preview_req = ctx.usb_preview_req;
    PfGradient* pf_gradient_p = ctx.pf_gradient_p;
    std::function<void(const std::string&)> pf_set_material = ctx.pf_set_material;
    std::function<void()> pf_restart_renderer = ctx.pf_restart_renderer;
    input::GpioPinCfg* gpio_pins_p = ctx.gpio_pins_p;
    int   gpio_slot_count = ctx.gpio_slot_count;
    bool* gpio_inputs_enabled_p = ctx.gpio_inputs_enabled_p;
    std::shared_ptr<std::function<void()>> gpio_reload = ctx.gpio_reload;
    integrations::KdeConnectBridge* kdc_p = ctx.kdc_p;
    std::vector<std::string>* kdc_ignore_p = ctx.kdc_ignore_p;
    std::vector<std::string>* kdc_msg_p = ctx.kdc_msg_p;
    bool* coproc_enabled_p = ctx.coproc_enabled_p;
    std::shared_ptr<std::function<void()>> coproc_reload = ctx.coproc_reload;
    std::shared_ptr<std::function<std::string()>> coproc_status = ctx.coproc_status;
    face::GlitchConfig* pf_glitch_p = ctx.pf_glitch_p;
    face::ScrollTextConfig* pf_scroll_p = ctx.pf_scroll_p;

    // GIF slot machinery shared with the Files tab — constructed in
    // build_menu() around one preview state. gif_leaf is build-phase only.
    auto gif_leaf = ctx.gif_leaf;
    MenuContextPanelDraw draw_gif_preview = ctx.draw_gif_preview;

    // Live effect preview for the Effects context panel: shows the actual
    // rendered face canvas (face + material + effects). With Live Preview on,
    // highlighting an effect applies it to the real face, so this previews it.
    auto live_face_frame = ctx.live_face_frame;
    struct EffectPreview { GLuint tex = 0; };
    auto eff_preview = std::make_shared<EffectPreview>();
    MenuContextPanelDraw draw_effect_preview =
        [eff_preview, live_face_frame](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            EffectPreview& ep = *eff_preview;
            // 2:1 thumbnail (HUB75 panel-pair aspect), centred.
            const float pw = std::min(sz.x * 0.9f, sz.y * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));
            cv::Mat rgb;
            if (live_face_frame && live_face_frame(rgb) && !rgb.empty() &&
                rgb.type() == CV_8UC3 && rgb.isContinuous()) {
                if (ep.tex == 0) {
                    glGenTextures(1, &ep.tex);
                    glBindTexture(GL_TEXTURE_2D, ep.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, ep.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, rgb.cols, rgb.rows, 0,
                             GL_RGB, GL_UNSIGNED_BYTE, rgb.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(ep.tex)),
                             {px, py}, {px + pw, py + ph});
            }
        };

    // ── Face effects ──────────────────────────────────────────────────────────
    std::vector<MenuItem> effects;
    for (uint8_t id = 0; id < 10; id++) {
        const char* names[] = { "Idle","Blink","Angry","Happy","Sad",
                                 "Shocked","Rainbow","Pulse","Wave","Custom" };
        effects.push_back(leaf_sel(names[id],
            [teensy, id, &state]{
                teensy->set_effect(id);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.effect_id = id;
            },
            [&state, id]{ return state.face.effect_id == id; }
        ));
    }

    // ── Face colors (presets + custom picker) ─────────────────────────────────
    std::vector<MenuItem> colors;
    colors.push_back(leaf("Teal",   [teensy]{ teensy->set_color(0,220,180,1);   }));
    colors.push_back(leaf("Cyan",   [teensy]{ teensy->set_color(0,180,255,1);   }));
    colors.push_back(leaf("Red",    [teensy]{ teensy->set_color(220,30,30,1);   }));
    colors.push_back(leaf("Green",  [teensy]{ teensy->set_color(30,220,60,1);   }));
    colors.push_back(leaf("Purple", [teensy]{ teensy->set_color(180,30,220,1);  }));
    colors.push_back(leaf("White",  [teensy]{ teensy->set_color(255,255,255,1); }));
    colors.push_back(color_picker(
        "Custom Color",
        [teensy](uint8_t r, uint8_t g, uint8_t b){ teensy->set_color(r, g, b, 1); },
        [&state]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
            return { state.face.r, state.face.g, state.face.b };
        }
    ));

    // ── ProtoTracer material color presets (Menu::GetFaceColor() index 0–11) ──
    std::vector<MenuItem> proto_colors;
    {
        struct PCEntry { const char* label; uint8_t idx; };
        const PCEntry pc_entries[] = {
            { "Default",    0  }, { "Yellow",     1  }, { "Orange",     2  },
            { "White",      3  }, { "Green",      4  }, { "Purple",     5  },
            { "Red",        6  }, { "Blue",       7  }, { "Rainbow",    8  },
            { "Flow Noise", 9  }, { "H Rainbow",  10 }, { "Black",      11 },
        };
        for (const auto& e : pc_entries) {
            proto_colors.push_back(leaf_sel(e.label,
                [teensy, idx = e.idx, &state]{
                    teensy->set_menu_item(8, idx);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.material_color = idx;
                },
                [&state, idx = e.idx]{ return state.face.material_color == idx; }
            ));
        }
    }

    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) gifs.push_back(gif_leaf(i));

    // ── Faces ────────────────────────────────────────────────────────────────
    // Static-PNG preview shared by the Files > Faces hub. Mirrors the GIF
    // preview: the highlighted slot's image is decoded once on the render
    // thread (via face::load_png_rgba) and cached in a GL texture until the
    // path changes.
    struct FacePreview {
        std::string                     loaded_path;    // ("" = no image cached)
        std::filesystem::file_time_type loaded_mtime{}; // detects in-place replace
        cv::Mat     image;         // CV_8UC4 RGBA, panel-sized
        int         want = -1;     // highlighted slot index
        GLuint      tex  = 0;
    };
    auto face_preview = std::make_shared<FacePreview>();

    MenuContextPanelDraw draw_face_preview =
        [face_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *face_preview;
            std::string path, expr_label;
            if (fp.want >= 0 && fp.want < kFaceSlotCount) {
                const auto& s = kFaceSlots[fp.want];
                expr_label = s.label;
                if (teensy->face_image_exists(s.expression))
                    path = teensy->face_image_path(s.expression);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                // 256×128 = 2:1 (HUB75 panel-pair aspect), NEAREST resize
                // preserves the pixel-art look.
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            // 2:1 thumbnail (matches HUB75 panel-pair canvas), centred, leaving
            // a line for the expression label beneath.
            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph},
                              IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!expr_label.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(expr_label.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), expr_label.c_str());
            }
        };

    // One management row per canonical expression: dynamic label, Play /
    // Replace / Clear when present on disk, Import otherwise.
    // Edit… visibility — true only when the active backend has addressable
    // LED regions (today: MAX7219 / RGB matrix). HUB75 + daemon return
    // false here and the leaf stays hidden.
    auto have_led_regions = [teensy]{ return teensy->has_led_face_editor(); };

    // Per-expression Versions submenu: Save Version… (named) + a list of saved
    // versions (named + auto-backups) with a thumbnail panel; Make Current /
    // Delete each. Copy-based — Make Current backs up the live PNG, copies the
    // version in via the controller (which reloads), so the renderer is unchanged.
    struct VerThumb { GLuint tex = 0; std::string loaded;
                      std::filesystem::file_time_type mt{}; cv::Mat img; };
    auto make_versions_submenu = [&, teensy, menu_sys_pp](const std::string& expr) -> MenuItem {
        auto entries = std::make_shared<std::vector<fvers::Entry>>();
        auto focus   = std::make_shared<int>(-1);
        auto thumb   = std::make_shared<VerThumb>();
        auto refresh = [entries, expr, teensy]{
            *entries = fvers::list(teensy->face_image_path(expr)); };

        std::vector<MenuItem> items;
        items.push_back(with_desc(leaf("Save Version\xE2\x80\xA6",
            [menu_sys_pp, expr, teensy, refresh, focus]{
                *focus = -1;
                if (!menu_sys_pp || !*menu_sys_pp) return;
                (*menu_sys_pp)->open_keyboard("Version name", "",
                    [expr, teensy, refresh](const std::string& nm){
                        fvers::save_named(teensy->face_image_path(expr), nm);
                        refresh();
                    });
            }), "Save the current art as a named version you can restore later."));
        for (auto& row : make_dynamic_rows(24,
                 [entries]{ return static_cast<int>(entries->size()); },
                 [&](int i) -> MenuItem {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "version";
            row.label_fn = [entries, i]{
                if (i >= static_cast<int>(entries->size())) return std::string();
                const auto& e = (*entries)[i];
                return e.named ? e.label : ("auto: " + e.label);
            };
            row.on_highlight = [focus, i]{ *focus = i; };
            MenuItem mk = leaf("Make Current",
                [entries, i, expr, teensy, refresh]{
                    if (i >= static_cast<int>(entries->size())) return;
                    const std::string live = teensy->face_image_path(expr);
                    fvers::backup_current(live, 15);                       // undo the switch
                    teensy->import_face_image(expr, (*entries)[i].path.string());  // copy + reload
                    teensy->set_face_by_name(expr);
                    refresh();
                });
            MenuItem del = leaf("Delete", [entries, i, refresh]{
                if (i >= static_cast<int>(entries->size())) return;
                std::error_code ec; std::filesystem::remove((*entries)[i].path, ec);
                refresh();
            });
            row.children = { std::move(mk), std::move(del) };
            return row;
        }))
            items.push_back(std::move(row));
        MenuItem sub = submenu("Versions", std::move(items));
        sub.action = refresh;   // SUBMENU on-enter hook: rescan disk
        sub.context_panel_title = "Version Preview";
        sub.context_panel_draw  = [thumb, focus, entries](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            std::string path;
            if (*focus >= 0 && *focus < static_cast<int>(entries->size()))
                path = (*entries)[*focus].path.string();
            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f), ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f, py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));
            if (path.empty()) {
                const char* m = "Scroll to a version";
                const ImVec2 ts = ImGui::CalcTextSize(m);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f, py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), m);
                return;
            }
            std::error_code ec; auto mt = std::filesystem::last_write_time(path, ec);
            if (path != thumb->loaded || mt != thumb->mt) {
                thumb->img = face::load_png_rgba(path, 256, 128);
                thumb->loaded = path; thumb->mt = mt;
            }
            if (!thumb->img.empty() && thumb->img.isContinuous()) {
                if (!thumb->tex) {
                    glGenTextures(1, &thumb->tex);
                    glBindTexture(GL_TEXTURE_2D, thumb->tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, thumb->tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumb->img.cols, thumb->img.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, thumb->img.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(thumb->tex)),
                             {px, py}, {px + pw, py + ph});
            }
        };
        return sub;
    };

    // Peer tables for the Copy from... submenus, as (expr, label) pairs.
    std::vector<std::pair<std::string, std::string>> face_peers, mouth_peers, boop_peers;
    for (int j = 0; j < kFaceSlotCount; ++j)
        face_peers.emplace_back(kFaceSlots[j].expression, kFaceSlots[j].label);
    for (int j = 0; j < kMouthShapeCount; ++j)
        mouth_peers.emplace_back(kMouthShapes[j].file_stem, kMouthShapes[j].label);
    for (int j = 0; j < kBoopFaceSlotCount; ++j)
        boop_peers.emplace_back(kBoopFaceSlots[j].file_stem, kBoopFaceSlots[j].label);

    auto face_slot_row = [&, face_preview, edit_face, make_versions_submenu](int slot_idx) -> MenuItem {
        const std::string expr  = kFaceSlots[slot_idx].expression;
        const std::string label = kFaceSlots[slot_idx].label;

        AssetSlotRowDesc d;
        d.label        = label;
        d.label_fn     = png_slot_label_fn(teensy, expr, label, pf_hub75_active_p);
        d.on_highlight = [face_preview, slot_idx]{ face_preview->want = slot_idx; };
        d.exists       = [teensy, expr]{ return teensy->face_image_exists(expr); };
        d.play         = [teensy, expr]{ teensy->set_face_by_name(expr); };
        // Edit launches the pixel editor on this slot's PNG. Visible only
        // when the active backend exposes covered LED regions — keeps the
        // option hidden in HUB75 / daemon modes where the editor would
        // have nothing meaningful to draw against.
        d.edit         = [edit_face, expr]{ if (edit_face) edit_face(expr); };
        d.edit_visible = have_led_regions;
        d.clear        = [teensy, expr]{ teensy->clear_face_image(expr); };
        d.import_action = [teensy, menu_sys_pp, expr, label]() {
            import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                  teensy, expr, label);
        };

        MenuItem versions = make_versions_submenu(expr);
        versions.description = "Saved versions of this face (named + auto-backups) "
                               "with thumbnails — Make Current to restore one.";
        d.versions = std::move(versions);

        d.copy_from = make_copy_from_submenu(teensy, expr, face_peers,
            "Copy another face's PNG into this slot as a "
            "starting point. The source slot keeps its art.");
        return make_asset_slot_row(std::move(d));
    };

    std::vector<MenuItem> face_files_menu;
    for (int i = 0; i < kFaceSlotCount; ++i)
        face_files_menu.push_back(face_slot_row(i));

    // ── Mouth Shapes (viseme overlays) ───────────────────────────────────────
    // Same import/preview pipeline as the expression slots, minus Play (these
    // are overlay assets — calling set_face_by_name("mouth_*") would just
    // fall back to neutral since they aren't entries in the loader's
    // expressions_ map). The voice analyzer's spectral centroid drives shape
    // selection at the FaceLoader layer in a later patch; this one only adds
    // the import slots so users can author the assets ahead of time.
    auto mouth_preview = std::make_shared<FacePreview>();

    MenuContextPanelDraw draw_mouth_preview =
        [mouth_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *mouth_preview;
            std::string path, mouth_label;
            if (fp.want >= 0 && fp.want < kMouthShapeCount) {
                const auto& s = kMouthShapes[fp.want];
                mouth_label = s.label;
                if (teensy->face_image_exists(s.file_stem))
                    path = teensy->face_image_path(s.file_stem);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph},
                              IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!mouth_label.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(mouth_label.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), mouth_label.c_str());
            }
        };

    // No Play row for mouth shapes — these are overlay assets; calling
    // set_face_by_name("mouth_*") would just fall back to neutral since they
    // aren't entries in the loader's expressions_ map.
    auto mouth_slot_row = [&, mouth_preview, edit_face, have_led_regions](int idx) -> MenuItem {
        const std::string expr  = kMouthShapes[idx].file_stem;
        const std::string label = kMouthShapes[idx].label;

        AssetSlotRowDesc d;
        d.label        = label;
        d.label_fn     = png_slot_label_fn(teensy, expr, label, pf_hub75_active_p);
        d.on_highlight = [mouth_preview, idx]{ mouth_preview->want = idx; };
        d.exists       = [teensy, expr]{ return teensy->face_image_exists(expr); };
        // Edit the mouth-shape PNG with the pixel editor (mono on MAX7219,
        // color on RGB matrix). Same visibility gate as the expression
        // slots — hidden in HUB75 / daemon modes.
        d.edit         = [edit_face, expr]{ if (edit_face) edit_face(expr); };
        d.edit_visible = have_led_regions;
        d.clear        = [teensy, expr]{ teensy->clear_face_image(expr); };
        d.import_action = [teensy, menu_sys_pp, expr, label]() {
            import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                  teensy, expr, label);
        };
        d.versions = make_versions_submenu(expr);
        // Copy from another mouth-shape slot — useful when one viseme is
        // a small tweak on another (e.g. small → smile).
        d.copy_from = make_copy_from_submenu(teensy, expr, mouth_peers,
            "Copy another viseme's PNG into this slot as a "
            "starting point. The source slot keeps its art.");
        return make_asset_slot_row(std::move(d));
    };

    std::vector<MenuItem> face_mouth_menu;
    for (int i = 0; i < kMouthShapeCount; ++i)
        face_mouth_menu.push_back(mouth_slot_row(i));

    face_files_menu.push_back(
        with_desc(with_panel(submenu("Mouth Shapes", std::move(face_mouth_menu)),
                             "Mouth Preview", draw_mouth_preview),
                  "Viseme overlays. Bond on top of the active expression when "
                  "the voice analyzer drives mouth_open > 0. Asset filenames: "
                  "mouth_small.png, mouth_open.png, mouth_smile.png, "
                  "mouth_round.png in the active face folder."));

    // ── Boop Reactions ───────────────────────────────────────────────────────
    // Authoring slots for the per-zone boop reaction faces. When a PNG
    // exists at faces/<active>/boop_<zone>.png, the on_boop callback above
    // triggers that face instead of the user's configured fallback
    // expression. Filenames: boop_snout / boop_left / boop_right / boop_both.
    auto boop_face_preview = std::make_shared<FacePreview>();
    MenuContextPanelDraw draw_boop_face_preview =
        [boop_face_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *boop_face_preview;
            std::string path, lbl;
            if (fp.want >= 0 && fp.want < kBoopFaceSlotCount) {
                const auto& s = kBoopFaceSlots[fp.want];
                lbl = s.label;
                if (teensy->face_image_exists(s.file_stem))
                    path = teensy->face_image_path(s.file_stem);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!lbl.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(lbl.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), lbl.c_str());
            }
        };

    // Boop slot row: Play pops the dedicated boop face via trigger_boop
    // (so it auto-reverts after the slot's duration). Edit / Replace /
    // Clear / Import behave the same as the expression slot rows.
    auto boop_face_row = [&, boop_face_preview, edit_face, have_led_regions](int idx) -> MenuItem {
        const std::string expr  = kBoopFaceSlots[idx].file_stem;
        const std::string label = kBoopFaceSlots[idx].label;

        AssetSlotRowDesc d;
        d.label        = label;
        d.label_fn     = png_slot_label_fn(teensy, expr, label, pf_hub75_active_p);
        d.on_highlight = [boop_face_preview, idx]{ boop_face_preview->want = idx; };
        d.exists       = [teensy, expr]{ return teensy->face_image_exists(expr); };
        // Play pops the dedicated boop face via trigger_boop (so it
        // auto-reverts after the zone's configured duration), unlike the
        // expression rows' plain set_face_by_name.
        d.play = [teensy, expr, idx, &state]() {
            double dur = 0.8;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (idx >= 0 && idx < 4) dur = state.boop_zones[idx].duration_s;
            }
            teensy->trigger_boop(expr, dur);
        };
        d.edit         = [edit_face, expr]{ if (edit_face) edit_face(expr); };
        d.edit_visible = have_led_regions;
        d.clear        = [teensy, expr]{ teensy->clear_face_image(expr); };
        d.import_action = [teensy, menu_sys_pp, expr, label]() {
            import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                  teensy, expr, label);
        };
        d.versions = make_versions_submenu(expr);
        d.copy_from = make_copy_from_submenu(teensy, expr, boop_peers,
            "Copy another boop reaction's PNG into this "
            "slot as a starting point. The source slot "
            "keeps its art.");
        return make_asset_slot_row(std::move(d));
    };

    std::vector<MenuItem> face_boop_menu;
    for (int i = 0; i < kBoopFaceSlotCount; ++i)
        face_boop_menu.push_back(boop_face_row(i));

    face_files_menu.push_back(
        with_desc(with_panel(submenu("Boop Reactions", std::move(face_boop_menu)),
                             "Boop Reaction Preview", draw_boop_face_preview),
                  "Dedicated face per boop zone. When a slot's PNG exists, "
                  "the boop sensor triggers that face instead of the zone's "
                  "fallback expression. Filenames: boop_snout, boop_left, "
                  "boop_right, boop_both — all in the active face folder."));

    // ── ProtoTracer (Teensy-driven LED matrix) submenu ────────────────────────
    std::vector<MenuItem> prototracer_inner_menu = {
        submenu("Faces",              std::move(effects)),
        submenu("Color",              std::move(colors)),
        submenu("ProtoTracer Palette", std::move(proto_colors)),
        with_panel(submenu("Animations", std::move(gifs)),
                   "GIF Preview", draw_gif_preview),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy, &state](float v){
                // Write the shared state too — the slider's getter reads it, so
                // without this every step snapped back to the stale value (and
                // the setting was lost on restart).
                teensy->set_brightness(static_cast<uint8_t>(v));
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.brightness = static_cast<uint8_t>(v);
            }),
        leaf("Release Control", [teensy]{ teensy->release_control(); }),
        leaf("Save Face Config", [teensy]{ teensy->save_config(); }),
        face_picker("Face", 10,
            [&state]{ return static_cast<int>(state.face.face_index); },
            [&state, teensy](int v){
                teensy->set_menu_item(0, static_cast<uint8_t>(v));
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.face_index = static_cast<uint8_t>(v);
            }),
        slider("Accent LED Brightness", 0.f, 10.f, 1.f, "",
            [&state]{ return static_cast<float>(state.face.accent_bright); },
            [&state, teensy](float v){
                uint8_t val = static_cast<uint8_t>(v);
                teensy->set_menu_item(2, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.accent_bright = val;
            }),
        toggle("Microphone",
            [&state]{ return state.face.microphone != 0; },
            [&state, teensy](bool on){
                uint8_t val = on ? 1 : 0;
                teensy->set_menu_item(3, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.microphone = val;
            }),
        slider("Mic Level", 0.f, 10.f, 1.f, "",
            [&state]{ return static_cast<float>(state.face.mic_level); },
            [&state, teensy](float v){
                uint8_t val = static_cast<uint8_t>(v);
                teensy->set_menu_item(4, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.mic_level = val;
            }),
        toggle("Touch Sensor",
            [&state]{ return state.face.boop_sensor != 0; },
            [&state, teensy](bool on){
                uint8_t val = on ? 1 : 0;
                teensy->set_menu_item(5, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.boop_sensor = val;
            }),
        toggle("Spectrum Mirror",
            [&state]{ return state.face.spectrum_mirror != 0; },
            [&state, teensy](bool on){
                uint8_t val = on ? 1 : 0;
                teensy->set_menu_item(6, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.spectrum_mirror = val;
            }),
        slider("Face Size", 0.f, 10.f, 1.f, "",
            [&state]{ return static_cast<float>(state.face.face_size); },
            [&state, teensy](float v){
                uint8_t val = static_cast<uint8_t>(v);
                teensy->set_menu_item(7, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.face_size = val;
            }),
        slider("Fan Speed", 0.f, 10.f, 1.f, "",
            [&state]{ return static_cast<float>(state.face.fan_speed); },
            [&state, teensy](float v){
                uint8_t val = static_cast<uint8_t>(v);
                teensy->set_menu_item(12, val);
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.fan_speed = val;
            }),
    };

    // ── Protoface (shm-fed LED panel) submenu ─────────────────────────────────
    // Protoface has a different effect_id map and material palette than
    // ProtoTracer, so it gets a dedicated control set. Commands forward to the
    // active backend via the FaceProxy — select "Source: Protoface" first.
    //
    // The Effects submenu hosts both the canned single-effect presets (via
    // teensy->set_effect(effect_id)) AND a Layered Builder where the user can
    // compose up to five particle layers, tweak each one's parameters, save
    // the composition to a slot, and export it to a file.
    // LayerCfg / LayeredEffectState are file-scope (above build_menu) — C++
    // forbids static-constexpr members on local classes, and we need
    // kMaxLayers to size the layer array.
    // Effects whose motion respects direction_deg — used to hide the
    // Direction slider for stationary / radial effects (sparkle / rings /
    // fireflies). Clouds participate too: when direction_deg is set their
    // clumps travel along the unit vector instead of the random horizontal
    // drift the effect uses by default.
    auto effect_is_directional = [](const std::string& e) {
        return e == "snow" || e == "rain" || e == "embers"
            || e == "confetti" || e == "clouds"
            || e == "lightning" || e == "meteor" || e == "bubbles";
    };
    static LayeredEffectState pf_layered;
    LayeredEffectState* pflz = &pf_layered;   // static address — safe to capture

    auto build_layered_spec = [pflz]() -> nlohmann::json {
        nlohmann::json out;
        out["layers"] = nlohmann::json::array();
        for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i) {
            const auto& L = pflz->layers[i];
            if (L.effect == "none") continue;
            nlohmann::json layer;
            layer["effect"]   = L.effect;
            layer["count"]    = L.count;
            layer["colors"]   = nlohmann::json::array({
                nlohmann::json::array({L.r, L.g, L.b})});
            layer["speed_min"]= L.speed_min;
            layer["speed_max"]= L.speed_max;
            layer["blend"]    = L.blend;
            // Only serialise direction when the user has overridden the
            // effect's default (>= 0). Otherwise omit so the particle
            // class falls back to its historical default angle.
            if (L.direction_deg >= 0.f)
                layer["direction_deg"] = L.direction_deg;
            if (L.direction_from != "none")
                layer["direction_from"] = L.direction_from;
            if (L.intensity_from != "none")
                layer["intensity_from"] = L.intensity_from;
            if (L.effect == "water") {
                layer["level"]      = L.level;
                layer["alpha"]      = L.alpha;
                layer["viscosity"]  = L.viscosity;
                layer["pitch_fill"] = L.pitch_fill;
                layer["face_glow"]  = L.face_glow;
                if (L.bubbles > 0) {
                    layer["bubbles"]     = L.bubbles;
                    layer["bubble_mode"] = L.bubble_mode;
                }
            }
            if (L.effect == "lightning") {
                layer["branches"] = L.branches;
                if (L.arc) layer["arc"] = true;
                if (L.random_origin) layer["origin"] = "random";
            }
            out["layers"].push_back(layer);
        }
        return out;
    };

    auto load_layered_spec = [pflz](const nlohmann::json& spec) {
        // Reset all layers, then walk the loaded "layers" array filling slots.
        for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
            pflz->layers[i] = LayerCfg{};
        if (!spec.contains("layers") || !spec["layers"].is_array()) return;
        int i = 0;
        for (const auto& jl : spec["layers"]) {
            if (i >= LayeredEffectState::kMaxLayers) break;
            LayerCfg& L = pflz->layers[i++];
            L.effect = jl.value("effect", std::string("none"));
            L.count  = jl.value("count", 20);
            if (jl.contains("colors") && jl["colors"].is_array()
                && !jl["colors"].empty() && jl["colors"][0].is_array()
                && jl["colors"][0].size() == 3) {
                L.r = jl["colors"][0][0].get<int>();
                L.g = jl["colors"][0][1].get<int>();
                L.b = jl["colors"][0][2].get<int>();
            }
            L.speed_min = jl.value("speed_min", 5.f);
            L.speed_max = jl.value("speed_max", 15.f);
            L.blend     = jl.value("blend", std::string("add"));
            L.direction_deg = jl.value("direction_deg", -1.f);
            L.direction_from = jl.value("direction_from", std::string("none"));
            L.intensity_from = jl.value("intensity_from", std::string("none"));
            L.level = jl.value("level", 0.4f);
            L.alpha = jl.value("alpha", 0.85f);
            L.viscosity = jl.value("viscosity", 0.15f);
            L.pitch_fill = jl.value("pitch_fill", 0.0f);
            L.face_glow = jl.value("face_glow", 0.55f);
            L.bubbles = jl.value("bubbles", 0);
            L.bubble_mode = jl.value("bubble_mode", std::string("rise"));
            L.arc = jl.value("arc", false);
            L.random_origin = jl.value("origin", std::string("edge")) == "random";
            L.branches = jl.value("branches", 0.35f);
        }
    };

    // Seed the layered builder from the effect that's actually running, once,
    // the first time the Face menu is built (i.e. at boot). The renderer
    // restores the last-used effect from protoface_state.json, but pf_layered
    // is a static default-constructed struct, so without this the Custom page
    // shows empty layers even though the panels are already rendering that
    // effect. Guarded by a static flag so later menu rebuilds never stomp the
    // user's in-progress edits, and gated on a real {"layers":[...]} spec so a
    // single-effect / "none" running spec can't blank the builder's defaults.
    {
        static bool pf_layered_seeded = false;
        if (!pf_layered_seeded) {
            pf_layered_seeded = true;
            if (pf_get_effect_json) {
                nlohmann::json running = pf_get_effect_json();
                if (running.is_object() && running.contains("layers")
                    && running["layers"].is_array()
                    && !running["layers"].empty())
                    load_layered_spec(running);
            }
        }
    }

    // Live Preview — when on, edits in the builder/effect-settings apply
    // continuously (and highlighting an effect applies it live, so the Effects
    // context-panel preview shows it). ParticleSystem::set_effect updates params
    // in place when the layer structure is unchanged, so tweaks don't reset the
    // particle sim. Default ON.
    auto pf_live = std::make_shared<bool>(true);
    auto live_apply = [pf_live, build_layered_spec, pf_set_effect_json]{
        if (*pf_live && pf_set_effect_json) pf_set_effect_json(build_layered_spec());
    };
    // main calls *pf_live_tick each frame; it re-applies only when the spec
    // actually changed, so dragging a slider previews smoothly (the particle
    // sim updates in place) without spamming rebuilds.
    if (pf_live_tick) {
        auto last = std::make_shared<nlohmann::json>();
        *pf_live_tick = [pf_live, build_layered_spec, pf_set_effect_json, last]{
            if (!*pf_live || !pf_set_effect_json) return;
            nlohmann::json s = build_layered_spec();
            if (s != *last) { pf_set_effect_json(s); *last = s; }
        };
    }

    // Effects users can pick per layer. "none" is the sentinel for "disable
    // this slot." The strings line up with the names ParticleSystem's factory
    // recognises (see particles.cpp::make_effect).
    static const char* const kLayerEffects[] = {
        "none", "sparkle", "embers", "rain", "snow",
        "confetti", "rings", "fireflies", "clouds",
        "lightning", "meteor", "bubbles", "fireworks", "vortex", "water",
        "starfield", "warp", "constellation", "shootingstars",
        // The "alive"-pack primitives shipped in the renderer + presets but
        // never landed in this list, so Single Effects / the layer editor
        // couldn't reach them.
        "steam", "waveform", "matrix", "circuit", "frost", "heatwave",
        "snooze",
    };
    static const char* const kBlendModes[] = {
        "add", "normal", "multiply", "screen",
    };

    auto build_layer_menu = [&, pflz](int idx) -> MenuItem {
        LayerCfg* L = &pflz->layers[idx];

        std::vector<MenuItem> effect_items;
        for (const char* name : kLayerEffects) {
            effect_items.push_back(leaf_sel(name,
                [L, name]{ L->effect = name; },
                [L, name]{ return L->effect == name; }));
        }

        std::vector<MenuItem> blend_items;
        for (const char* name : kBlendModes) {
            blend_items.push_back(leaf_sel(name,
                [L, name]{ L->blend = name; },
                [L, name]{ return L->blend == name; }));
        }

        // Move Up / Move Down — swap with the neighbouring layer. Hidden at
        // the ends so the user doesn't shuffle into a no-op.
        MenuItem move_up = leaf("Move Up", [pflz, idx]{
            if (idx > 0) std::swap(pflz->layers[idx], pflz->layers[idx - 1]);
        });
        move_up.visible_fn = [idx]{ return idx > 0; };
        MenuItem move_dn = leaf("Move Down", [pflz, idx]{
            if (idx + 1 < LayeredEffectState::kMaxLayers)
                std::swap(pflz->layers[idx], pflz->layers[idx + 1]);
        });
        move_dn.visible_fn = [idx]{ return idx + 1 < LayeredEffectState::kMaxLayers; };

        MenuItem clear = leaf("Clear Layer", [L]{ *L = LayerCfg{}; });

        std::vector<MenuItem> items = {
            submenu("Effect",     std::move(effect_items)),
            slider("Count",      0.f, 100.f,  1.f, "",
                [L]{ return static_cast<float>(L->count); },
                [L](float v){ L->count = static_cast<int>(v); }),
            // Color — the unified full-screen picker (SV square, hue strip,
            // hex/RGB typed entry, shared history). Writes L->r/g/b directly,
            // exactly like the old HSV sliders did, so every adjustment still
            // previews live through the existing layered-effects push.
            color_picker("Color",
                [L](uint8_t r, uint8_t g, uint8_t b){
                    L->r = r; L->g = g; L->b = b;
                },
                [L]() -> std::tuple<uint8_t, uint8_t, uint8_t> {
                    return { static_cast<uint8_t>(L->r),
                             static_cast<uint8_t>(L->g),
                             static_cast<uint8_t>(L->b) };
                }),
            slider("Density",    1.f, 120.f,  1.f, "",
                [L]{ return static_cast<float>(L->count); },
                [L](float v){ L->count = static_cast<int>(v); }),
            slider("Speed Min",  0.f, 100.f,  1.f, "",
                [L]{ return L->speed_min; },
                [L](float v){ L->speed_min = v; if (L->speed_max < v) L->speed_max = v; }),
            slider("Speed Max",  0.f, 100.f,  1.f, "",
                [L]{ return L->speed_max; },
                [L](float v){ L->speed_max = v; if (L->speed_min > v) L->speed_min = v; }),
            // Direction (only for effects whose motion respects an angle).
            // The slider clicks in 10° increments through the full 360°;
            // visible_fn hides the row when the picked effect ignores it.
            ([&]{
                MenuItem dir = slider("Direction", 0.f, 360.f, 10.f, "\xc2\xb0",
                    [L]{ return L->direction_deg < 0.f ? 90.f : L->direction_deg; },
                    [L](float v){ L->direction_deg = v; });
                dir.visible_fn = [L, effect_is_directional]{
                    return effect_is_directional(L->effect);
                };
                return dir;
            })(),
            // Fill Level — only meaningful for the "water" liquid effect.
            ([&]{
                MenuItem lvl = slider("Fill Level", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->level * 100.f; },
                    [L](float v){ L->level = v / 100.f; });
                lvl.visible_fn = [L]{ return L->effect == "water"; };
                return lvl;
            })(),
            // Opacity — water only: how solid the liquid reads. Lower = more
            // transparent, the face and background show through more.
            ([&]{
                MenuItem op = slider("Opacity", 10.f, 100.f, 5.f, "%",
                    [L]{ return L->alpha * 100.f; },
                    [L](float v){ L->alpha = v / 100.f; });
                op.visible_fn = [L]{ return L->effect == "water"; };
                return op;
            })(),
            // Viscosity — only for "water": higher = slower, resists sloshing.
            ([&]{
                MenuItem vis = slider("Viscosity", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->viscosity * 100.f; },
                    [L](float v){ L->viscosity = v / 100.f; });
                vis.visible_fn = [L]{ return L->effect == "water"; };
                return vis;
            })(),
            // Face Glow — water only: the submerged face shines back through
            // the liquid, tinted by it (eyes read through the water).
            ([&]{
                MenuItem fg = slider("Face Glow", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->face_glow * 100.f; },
                    [L](float v){ L->face_glow = v / 100.f; });
                fg.visible_fn = [L]{ return L->effect == "water"; };
                return fg;
            })(),
            // Pitch Fill — water only: head pitch shifts the liquid level.
            ([&]{
                MenuItem pf = slider("Pitch Fill", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->pitch_fill * 100.f; },
                    [L](float v){ L->pitch_fill = v / 100.f; });
                pf.visible_fn = [L]{ return L->effect == "water"; };
                return pf;
            })(),
            // Bubbles / droplets — water only.
            ([&]{
                MenuItem bub = slider("Bubbles", 0.f, 30.f, 1.f, "",
                    [L]{ return static_cast<float>(L->bubbles); },
                    [L](float v){ L->bubbles = static_cast<int>(v); });
                bub.visible_fn = [L]{ return L->effect == "water"; };
                return bub;
            })(),
            ([&]{
                std::vector<MenuItem> bm = {
                    leaf_sel("Rise (bubbles in liquid)", [L]{ L->bubble_mode = "rise"; },
                                                         [L]{ return L->bubble_mode == "rise"; }),
                    leaf_sel("Drip (droplets above)",    [L]{ L->bubble_mode = "drip"; },
                                                         [L]{ return L->bubble_mode == "drip"; }),
                };
                MenuItem m = submenu("Bubble Style", std::move(bm));
                m.label_fn   = [L]{ return std::string("Bubble Style: ") + L->bubble_mode; };
                m.visible_fn = [L]{ return L->effect == "water" && L->bubbles > 0; };
                return m;
            })(),
            // Lightning extras — arc mode, random origin + fork density
            // (lightning only).
            ([&]{
                MenuItem t = toggle("Arc Mode",
                    [L]{ return L->arc; }, [L](bool v){ L->arc = v; });
                t.visible_fn = [L]{ return L->effect == "lightning"; };
                return t;
            })(),
            ([&]{
                MenuItem t = with_desc(toggle("Random Origin",
                    [L]{ return L->random_origin; },
                    [L](bool v){ L->random_origin = v; }),
                    "Bolts strike from random points on the canvas in random "
                    "directions, instead of always falling from the "
                    "directional edge. Bolt mode only — arcs always jump "
                    "between the panel edges.");
                t.visible_fn = [L]{ return L->effect == "lightning" && !L->arc; };
                return t;
            })(),
            ([&]{
                MenuItem br = slider("Branches", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->branches * 100.f; },
                    [L](float v){ L->branches = v / 100.f; });
                br.visible_fn = [L]{ return L->effect == "lightning"; };
                return br;
            })(),
            // Motion reactivity — couple this layer's direction to head movement.
            ([&]{
                static const char* const kMotionSrc[] = {"none", "heading", "yaw", "tilt"};
                std::vector<MenuItem> msrc;
                for (const char* s : kMotionSrc)
                    msrc.push_back(leaf_sel(s,
                        [L, s]{ L->direction_from = s; },
                        [L, s]{ return L->direction_from == s; }));
                MenuItem m = with_desc(submenu("Motion Reactive", std::move(msrc)),
                    "Drive this layer's direction from the IMU: heading locks it "
                    "to the compass, yaw makes it drift as you turn your head, "
                    "tilt skews it like gravity when you roll. Apply Now to push.");
                m.label_fn = [L]{ return std::string("Motion: ") + L->direction_from; };
                return m;
            })(),
            // Density reactivity — scale particle count from audio or motion.
            ([&]{
                static const char* const kIntSrc[] = {"none", "audio", "yaw_rate", "accel"};
                std::vector<MenuItem> isrc;
                for (const char* s : kIntSrc)
                    isrc.push_back(leaf_sel(s,
                        [L, s]{ L->intensity_from = s; },
                        [L, s]{ return L->intensity_from == s; }));
                MenuItem m = with_desc(submenu("Density Reactive", std::move(isrc)),
                    "Scale this layer's particle count from a live signal: audio "
                    "(mic level — pulses with sound), yaw_rate or accel (head "
                    "movement). Apply Now to push.");
                m.label_fn = [L]{ return std::string("Density: ") + L->intensity_from; };
                return m;
            })(),
            submenu("Blend Mode", std::move(blend_items)),
            std::move(move_up),
            std::move(move_dn),
            std::move(clear),
        };
        char name_buf[32];
        std::snprintf(name_buf, sizeof(name_buf), "Layer %d", idx + 1);
        MenuItem m = submenu(name_buf, std::move(items));
        // Dynamic label so the parent menu shows the active effect at a glance.
        m.label_fn = [L, idx]{
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Layer %d: %s", idx + 1,
                          L->effect.c_str());
            return std::string(buf);
        };
        return m;
    };

    std::vector<MenuItem> layered_items;
    for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
        layered_items.push_back(build_layer_menu(i));

    layered_items.push_back(leaf("Apply Now",
        [build_layered_spec, pf_set_effect_json]{
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        }));
    layered_items.push_back(with_desc(toggle("Live Preview",
        [pf_live]{ return *pf_live; },
        [pf_live, live_apply](bool v){ *pf_live = v; if (v) live_apply(); }),
        "Apply edits to the panels continuously as you tweak layers, instead of "
        "hitting Apply Now. The sim updates in place (no reset) while the layer "
        "structure is unchanged."));

    // (Built-in Presets / Randomize / Expression Effects are assembled as their
    //  own top-level Effects pages below — Premade / Random / the toggle.)

    // Save / Load — three numbered quick-save slots PLUS user-named
    // presets entered via the on-screen keyboard. Everything lands in
    // cfg["protoface"]["custom_effects"][key]; the existing mutate_cfg
    // path persists to disk on shutdown.

    // Ensures cfg[key1][key2]... resolves to an object so subsequent
    // operator[] writes/reads can't trip type_error.305/306. nlohmann's
    // operator[] auto-creates an empty object on a null node, but a
    // hand-edited string or array at any of these paths would throw —
    // overwrite with an empty object first to keep the menu robust.
    auto ensure_obj_path = [](json& root,
                              std::initializer_list<const char*> keys) -> json& {
        json* cur = &root;
        for (const char* k : keys) {
            if (!cur->is_object()) *cur = json::object();
            if (!cur->contains(k) || !(*cur)[k].is_object()) (*cur)[k] = json::object();
            cur = &(*cur)[k];
        }
        return *cur;
    };

    // "Save As..." opens the OSK to name the preset; commit writes
    // the live builder spec under cfg["protoface"]["custom_effects"][name].
    layered_items.push_back(leaf("Save As...",
        [cfg_root, menu_sys_pp, build_layered_spec, ensure_obj_path]{
            if (!menu_sys_pp || !*menu_sys_pp || !cfg_root) return;
            (*menu_sys_pp)->open_keyboard(
                "Preset Name", std::string(),
                [cfg_root, build_layered_spec, ensure_obj_path](const std::string& name){
                    if (!cfg_root || name.empty()) return;
                    ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] =
                        build_layered_spec();
                });
        }));

    std::vector<MenuItem> save_items, load_items;
    for (int s = 1; s <= 3; ++s) {
        char nm[16]; std::snprintf(nm, sizeof(nm), "Slot %d", s);
        save_items.push_back(leaf(nm, [cfg_root, s, build_layered_spec, ensure_obj_path]{
            if (!cfg_root) return;
            char key[16]; std::snprintf(key, sizeof(key), "slot_%d", s);
            ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[key] =
                build_layered_spec();
        }));
    }
    layered_items.push_back(submenu("Save to Slot", std::move(save_items)));

    // Load — walks every key under cfg["protoface"]["custom_effects"]
    // (sorted alphabetically) and exposes up to kMaxLoadEntries dynamic
    // rows. Each row's label tracks its slot's name via label_fn so a
    // freshly Saved-As preset shows up immediately on next draw, and a
    // visible_fn hides empty slots so the user only sees real entries.
    constexpr int kMaxLoadEntries = 16;
    auto preset_name_at = [cfg_root](int idx) -> std::string {
        if (!cfg_root || !cfg_root->contains("protoface")
            || !(*cfg_root)["protoface"].is_object()) return {};
        const auto& jpf = (*cfg_root)["protoface"];
        if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object())
            return {};
        std::vector<std::string> keys;
        for (auto& [k, _] : jpf["custom_effects"].items()) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        return (idx >= 0 && idx < static_cast<int>(keys.size()))
                ? keys[static_cast<size_t>(idx)] : std::string{};
    };
    for (int i = 0; i < kMaxLoadEntries; ++i) {
        MenuItem row;
        row.type = MenuItemType::LEAF;
        row.label = "preset";   // overridden by label_fn each draw
        row.label_fn   = [preset_name_at, i]{ return preset_name_at(i); };
        row.visible_fn = [preset_name_at, i]{ return !preset_name_at(i).empty(); };
        row.action = [cfg_root, preset_name_at, i, load_layered_spec]{
            const std::string nm = preset_name_at(i);
            if (!cfg_root || nm.empty()) return;
            if (!(*cfg_root)["protoface"].is_object()) return;
            const auto& jpf = (*cfg_root)["protoface"];
            if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object()) return;
            const auto& ce = jpf["custom_effects"];
            if (ce.contains(nm)) load_layered_spec(ce[nm]);
        };
        load_items.push_back(std::move(row));
    }
    layered_items.push_back(submenu("Load Preset", std::move(load_items)));

    // Delete — same dynamic listing as Load. Removing a preset key drops
    // it from cfg; the next mutate_cfg writeback won't carry it.
    std::vector<MenuItem> delete_items;
    for (int i = 0; i < kMaxLoadEntries; ++i) {
        MenuItem row;
        row.type = MenuItemType::LEAF;
        row.label = "preset";
        row.label_fn   = [preset_name_at, i]{ return preset_name_at(i); };
        row.visible_fn = [preset_name_at, i]{ return !preset_name_at(i).empty(); };
        row.action = [cfg_root, preset_name_at, i]{
            const std::string nm = preset_name_at(i);
            if (!cfg_root || nm.empty()) return;
            if (!(*cfg_root)["protoface"].is_object()) return;
            auto& jpf = (*cfg_root)["protoface"];
            if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object()) return;
            jpf["custom_effects"].erase(nm);
        };
        delete_items.push_back(std::move(row));
    }
    layered_items.push_back(with_desc(
        submenu("Delete Preset", std::move(delete_items)),
        "Remove a saved preset permanently. The slot disappears from cfg "
        "on next save."));

    // Export the live composition to /tmp/protohud_layered_effect.json so
    // the user can copy it elsewhere or paste it into another cfg.
    layered_items.push_back(leaf("Export to File", [build_layered_spec]{
        const std::string path = "/tmp/protohud_layered_effect.json";
        std::ofstream f(path);
        if (!f) {
            std::fprintf(stderr, "[effects] cannot open %s\n", path.c_str());
            return;
        }
        f << build_layered_spec().dump(2) << "\n";
        std::cerr << "[effects] exported layered spec to " << path << "\n";
    }));

    MenuItem pf_layered_item = with_desc(
        submenu("Custom", std::move(layered_items)),
        "Compose up to five particle layers and apply the stack live. Each "
        "layer is independent: pick an effect, tweak count / colour / speed / "
        "blend, reorder with Move Up / Move Down, clear to disable.\n"
        "  • Apply Now pushes the composition to the renderer.\n"
        "  • Save As... names the current build and stores it.\n"
        "  • Save to Slot writes one of three numbered quick-save slots.\n"
        "  • Load Preset lists every named or slot save (alphabetical).\n"
        "  • Delete Preset removes a saved entry.\n"
        "  • Export to File dumps the live spec to "
        "/tmp/protohud_layered_effect.json.\n"
        "All presets persist under cfg[\"protoface\"][\"custom_effects\"].");

    // ── Effects first page: Single / Premade / Custom / Random ───────────────
    // 1) SINGLE EFFECTS — one primitive at a time. Select applies it (on layer 0,
    //    clearing the rest); Ctrl+Select opens that effect's full settings.
    std::vector<MenuItem> single_items;
    for (const char* nm : kLayerEffects) {
        if (std::string(nm) == "none") continue;
        const std::string name = nm;
        MenuItem it = leaf(name, [pflz, name, build_layered_spec, pf_set_effect_json]{
            for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i) pflz->layers[i] = LayerCfg{};
            pflz->layers[0].effect = name;
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        });
        it.description = "Select: apply this effect.  Ctrl+Select: open its settings.";
        // Ctrl+Select: switch layer 0 to this effect, then open the layer editor.
        it.secondary_action = [pflz, name]{
            pflz->layers[0] = LayerCfg{}; pflz->layers[0].effect = name;
            for (int i = 1; i < LayeredEffectState::kMaxLayers; ++i) pflz->layers[i] = LayerCfg{};
        };
        it.secondary_children = build_layer_menu(0).children;   // full per-layer settings
        single_items.push_back(std::move(it));
    }
    MenuItem single_item = with_desc(submenu("Single Effects", std::move(single_items)),
        "One effect at a time. Select applies it; Ctrl+Select opens its settings "
        "(colour, density, speed, and effect-specific options).");

    // 2) PREMADE EFFECTS — curated combos; the side panel shows the recipe.
    struct Premade { const char* name; const char* combo; };
    static const Premade kPremades[] = {
        {"gentle_snow","snow — light drift"}, {"heavy_snow","snow — dense"},
        {"campfire","embers"}, {"galaxy","sparkle — multicolour"},
        {"party","confetti"}, {"radar","expanding rings"},
        {"fire","embers + embers + sparkle"}, {"aurora","fireflies + sparkle"},
        {"blizzard","driven snow x2"}, {"sonar","rings + fireflies"},
        {"celebration","confetti + sparkle"}, {"plasma","blue embers x2 + ring"},
        {"thunderstorm","rain + branched lightning"}, {"arc","crackling electric arcs"},
        {"meteor_shower","meteors + sparkle"},
        {"fireworks","fireworks bursts"}, {"bubbles","rising bubbles"},
        {"vortex","comet vortex (cool) + sparkle"},
        {"vortex_ember","comet vortex (fire palette)"},
        {"vortex_toxic","comet vortex (toxic green)"},
        {"vortex_rose","comet vortex (pink/violet)"},
        {"vortex_rainbow","comet vortex (rainbow)"},
        {"nebula","clouds x2 + sparkle"},
        {"starfield","parallax stars from centre"},
        {"warp","hyperspace streaks"},
        {"constellation","still twinkling sky"},
        {"shooting_stars","meteors from centre"},
        {"night_sky","twinkle + shooting stars"},
        {"water","liquid — cyan, bubbles"}, {"lava","liquid — lava, thick"},
        {"toxic","liquid — green, bubbles"}, {"ocean","liquid — teal"},
        {"plasma_fluid","liquid — magenta"}, {"mercury","liquid — silver, thick"},
    };
    auto premade_combo = std::make_shared<std::string>();
    std::vector<MenuItem> premade_items;
    for (const auto& pm : kPremades) {
        const std::string name = pm.name, combo = pm.combo;
        MenuItem it = leaf(name, [name, pf_set_effect_json]{
            nlohmann::json spec; spec["preset"] = name;
            if (pf_set_effect_json) pf_set_effect_json(spec);
        });
        it.on_highlight = [premade_combo, name, combo]{ *premade_combo = name + "\n" + combo; };
        it.description  = "Select: apply.  Ctrl+Select: save a copy to Custom.";
        it.secondary_action = [cfg_root, name, ensure_obj_path]{
            if (!cfg_root) return;
            nlohmann::json spec; spec["preset"] = name;
            ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] = spec;
        };
        premade_items.push_back(std::move(it));
    }
    *premade_combo = std::string(kPremades[0].name) + "\n" + kPremades[0].combo;  // seed panel
    MenuItem premade_item = submenu("Premade Effects", std::move(premade_items));
    premade_item.context_panel_title = "Recipe";
    premade_item.context_panel_draw =
        [premade_combo](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            (void)sz;
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const std::string& s = *premade_combo;
            float y = o.y + 6.f;
            size_t start = 0;
            while (start <= s.size()) {
                const size_t nl = s.find('\n', start);
                const std::string line = s.substr(start,
                    nl == std::string::npos ? std::string::npos : nl - start);
                dl->AddText(font, fs, ImVec2(o.x + 6.f, y),
                            IM_COL32(220, 225, 235, 255), line.c_str());
                y += fs + 4.f;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        };
    premade_item = with_desc(std::move(premade_item),
        "Curated multi-layer presets. The side panel shows what each is made of. "
        "Select applies; Ctrl+Select saves a copy to Custom.");

    // 3) CUSTOM is the layered builder (pf_layered_item, renamed above).
    // 4) RANDOM — generate a fresh mix; optionally save it to Custom.
    std::vector<MenuItem> random_items;
    random_items.push_back(with_desc(leaf("Randomize (Surprise Me)",
        [pflz, build_layered_spec, pf_set_effect_json]{
            static std::mt19937 rng(std::random_device{}());
            static const char* const fx[] = {
                "sparkle","embers","rain","snow","confetti","rings","fireflies",
                "clouds","lightning","meteor","bubbles","fireworks","vortex",
                "starfield","constellation"};
            static const int pal[][3] = {
                {0,220,180},{255,80,80},{80,180,255},{255,200,40},
                {180,80,255},{80,255,160},{255,120,20},{255,255,255}};
            const int nfx  = static_cast<int>(sizeof(fx)/sizeof(fx[0]));
            const int npal = static_cast<int>(sizeof(pal)/sizeof(pal[0]));
            for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
                pflz->layers[i] = LayerCfg{};
            std::uniform_int_distribution<int> nlayers(1,3), pf(0,nfx-1),
                pp(0,npal-1), cnt(10,60), spd(2,30);
            const int N = nlayers(rng);
            for (int i = 0; i < N; ++i) {
                LayerCfg& L = pflz->layers[i];
                L.effect = fx[pf(rng)];
                const int* c = pal[pp(rng)];
                L.r = c[0]; L.g = c[1]; L.b = c[2];
                L.count = cnt(rng);
                L.speed_min = static_cast<float>(spd(rng));
                L.speed_max = L.speed_min + static_cast<float>(spd(rng));
                L.blend = "add";
            }
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        }),
        "Roll a fresh 1–3 layer mix and apply it. Tweak it in Custom, or save it "
        "below."));
    random_items.push_back(leaf("Save Current to Custom...",
        [cfg_root, menu_sys_pp, build_layered_spec, ensure_obj_path]{
            if (!menu_sys_pp || !*menu_sys_pp || !cfg_root) return;
            (*menu_sys_pp)->open_keyboard("Preset Name", std::string(),
                [cfg_root, build_layered_spec, ensure_obj_path](const std::string& name){
                    if (!cfg_root || name.empty()) return;
                    ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] =
                        build_layered_spec();
                });
        }));
    MenuItem random_item = with_desc(submenu("Random", std::move(random_items)),
        "Discover new looks. Randomize applies a fresh mix; Save Current to Custom "
        "keeps the one you like.");

    std::vector<MenuItem> pf_effects;
    pf_effects.push_back(std::move(single_item));
    pf_effects.push_back(std::move(premade_item));
    pf_effects.push_back(std::move(pf_layered_item));   // "Custom"
    pf_effects.push_back(std::move(random_item));
    if (pf_expr_effects_p && pf_set_expr_effects)
        pf_effects.push_back(with_desc(toggle("Expression Effects",
            [pf_expr_effects_p]{ return *pf_expr_effects_p; },
            [pf_expr_effects_p, pf_set_expr_effects, cfg_root](bool v){
                *pf_expr_effects_p = v; pf_set_expr_effects(v);
                if (cfg_root) (*cfg_root)["protoface"]["expression_effects"] = v;
            }),
            "Auto-swap the particle effect to a mood preset as the face changes "
            "(angry\xe2\x86\x92""fire, happy\xe2\x86\x92""celebration, "
            "sad\xe2\x86\x92""rain, shocked\xe2\x86\x92""galaxy)."));
    if (pf_motion_particles_p && pf_set_motion_particles)
        pf_effects.push_back(with_desc(toggle("Motion Reactive",
            [pf_motion_particles_p]{ return *pf_motion_particles_p; },
            [pf_motion_particles_p, pf_set_motion_particles, cfg_root](bool v){
                *pf_motion_particles_p = v; pf_set_motion_particles(v);
                if (cfg_root) (*cfg_root)["protoface"]["motion_particles"] = v;
            }),
            "Couple directional effects to real head motion: rain/snow/steam "
            "lean with gravity as you tilt and get swept sideways by quick "
            "turns. Needs an IMU feeding head tracking."));
    if (pf_motion_scale_p)
        pf_effects.push_back(with_desc(slider("Motion Sensitivity", 10.f, 200.f, 10.f, "%",
            [pf_motion_scale_p]{ return static_cast<float>(*pf_motion_scale_p * 100.0); },
            [pf_motion_scale_p, cfg_root](float v){
                *pf_motion_scale_p = v / 100.0;
                if (cfg_root) (*cfg_root)["protoface"]["motion_scale"] = v / 100.0;
            }),
            "Maps IMU angles to face response: how strongly head roll/pitch/"
            "turn rate drive Motion Reactive, water tilt/slosh and Face "
            "Inertia. 100% = raw angles; drop it if slight tilts feel "
            "exaggerated on the panels. The HUD compass is never scaled."));
    if (pf_face_inertia_p && pf_set_face_inertia) {
        pf_effects.push_back(with_desc(toggle("Face Inertia",
            [pf_face_inertia_p]{ return *pf_face_inertia_p; },
            [pf_face_inertia_p, pf_set_face_inertia, cfg_root](bool v){
                *pf_face_inertia_p = v; pf_set_face_inertia(v);
                if (cfg_root) (*cfg_root)["protoface"]["face_inertia"] = v;
            }),
            "The whole face slides opposite quick head motion and springs "
            "back with a small overshoot, like it has mass: eyes lag on a "
            "fast turn and bob on a nod, then settle. Uses the same IMU "
            "feed as Motion Reactive."));
        if (pf_face_inertia_strength_p && pf_set_face_inertia_strength) {
            MenuItem m = with_desc(slider("Shift Amount", 10.f, 200.f, 10.f, "%",
                [pf_face_inertia_strength_p]{
                    return static_cast<float>(*pf_face_inertia_strength_p * 100.0);
                },
                [pf_face_inertia_strength_p, pf_set_face_inertia_strength,
                 cfg_root](float v){
                    *pf_face_inertia_strength_p = v / 100.0;
                    pf_set_face_inertia_strength(v / 100.0);
                    if (cfg_root)
                        (*cfg_root)["protoface"]["face_inertia_strength"] = v / 100.0;
                }),
                "How far the face can slide: 100% is about a tenth of the "
                "panel width.");
            m.visible_fn = [pf_face_inertia_p]{ return *pf_face_inertia_p; };
            pf_effects.push_back(std::move(m));
        }
    }
    if (pf_weather_effects_p && pf_set_weather_effects)
        pf_effects.push_back(with_desc(toggle("Weather Sync",
            [pf_weather_effects_p]{ return *pf_weather_effects_p; },
            [pf_weather_effects_p, pf_set_weather_effects, cfg_root](bool v){
                *pf_weather_effects_p = v; pf_set_weather_effects(v);
                if (cfg_root) (*cfg_root)["protoface"]["weather_effects"] = v;
            }),
            "Match the face's ambient effect to the real weather (rain, snow, "
            "thunderstorm, clouds; a clear night shows the night sky). Uses "
            "the HUD's weather monitor; your chosen effect returns when it "
            "clears up. Expression moods still play on top."));
    if (pf_temp_effects_p && pf_ambient_resync) {
        pf_effects.push_back(with_desc(toggle("Temp Effects",
            [pf_temp_effects_p]{ return *pf_temp_effects_p; },
            [pf_temp_effects_p, pf_ambient_resync, cfg_root](bool v){
                *pf_temp_effects_p = v; pf_ambient_resync();
                if (cfg_root) (*cfg_root)["protoface"]["temp_effects"] = v;
            }),
            "Ambient frost crystals when it's freezing outside and rising "
            "heat shimmer when it's scorching, from the live temperature. "
            "Weather Sync's precipitation takes priority when both are on; "
            "the thresholds below are Â°""C."));
        if (pf_temp_cold_p) {
            MenuItem m = with_desc(slider("Cold Below", -20.f, 15.f, 1.f,
                "Â°""C",
                [pf_temp_cold_p]{ return static_cast<float>(*pf_temp_cold_p); },
                [pf_temp_cold_p, pf_ambient_resync, cfg_root](float v){
                    *pf_temp_cold_p = v; pf_ambient_resync();
                    if (cfg_root) (*cfg_root)["protoface"]["temp_cold_c"] = v;
                }),
                "Frost appears at or below this outdoor temperature.");
            m.visible_fn = [pf_temp_effects_p]{ return *pf_temp_effects_p; };
            pf_effects.push_back(std::move(m));
        }
        if (pf_temp_hot_p) {
            MenuItem m = with_desc(slider("Hot Above", 25.f, 50.f, 1.f,
                "Â°""C",
                [pf_temp_hot_p]{ return static_cast<float>(*pf_temp_hot_p); },
                [pf_temp_hot_p, pf_ambient_resync, cfg_root](float v){
                    *pf_temp_hot_p = v; pf_ambient_resync();
                    if (cfg_root) (*cfg_root)["protoface"]["temp_hot_c"] = v;
                }),
                "Heat shimmer appears at or above this outdoor temperature.");
            m.visible_fn = [pf_temp_effects_p]{ return *pf_temp_effects_p; };
            pf_effects.push_back(std::move(m));
        }
    }
    {
        // Legacy Teensy/ProtoTracer single-effect ids (only meaningful on the
        // Teensy face backend) — tucked into their own page.
        const char* pf_effect_names[] = {
            "None","Sparkle","Embers","Rain","Snow","Confetti","Rings","Fireflies",
            "Fire","Aurora","Blizzard","Sonar","Plasma","Celebration","Galaxy","Party",
            "Clouds","Nebula","Starfield","Warp","Constellation","Shooting Stars",
            "Night Sky","Steam","Waveform","Matrix","Circuit","Frost","Heatwave","Snooze",
        };
        const uint8_t pf_effect_count =
            static_cast<uint8_t>(sizeof(pf_effect_names) / sizeof(pf_effect_names[0]));
        std::vector<MenuItem> teensy_fx;
        for (uint8_t id = 0; id < pf_effect_count; id++)
            teensy_fx.push_back(leaf_sel(pf_effect_names[id],
                [teensy, id, &state]{
                    teensy->set_effect(id);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.effect_id = id;
                },
                [&state, id]{ return state.face.effect_id == id; }));
        pf_effects.push_back(with_desc(submenu("Teensy Face Effects", std::move(teensy_fx)),
            "Single-effect ids for the Teensy/ProtoTracer face backend (no effect "
            "on the native Protoface particle renderer)."));
    }

    // Material Color — the single colour/material picker (Face Color was folded
    // in here). Named solids + multi-colour patterns/gradients, plus a Custom
    // Color (solid) picker and the Custom Gradient editor below.
    std::vector<MenuItem> pf_palette;
    {
        // Face-colour pass-through: draw each expression's own RGB art instead of
        // tinting it with the material. Also switches the face editor to its colour
        // canvas so HUB75 faces can be drawn in colour. HUB75/native backend only.
        MenuItem t = with_desc(
            toggle("Use Drawn Colors",
                [&state]{ std::lock_guard<std::mutex> lk(state.mtx); return state.face.face_colors; },
                [teensy, &state](bool v){
                    teensy->set_menu_item(9, v ? 1 : 0);   // 9 = native face-colour pass-through
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.face_colors = v;
                }),
            "Show each expression's own drawn colours instead of overriding them "
            "with the Material Color below, and turn on the editor's colour canvas "
            "so faces can be drawn in colour. GIFs already show their own colours; "
            "particles/background still use the material.");
        t.visible_fn = [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "hub75"; };
        pf_palette.push_back(std::move(t));
    }
    {
        struct PFMat { const char* label; uint8_t idx; };
        const PFMat pf_mats[] = {
            // Solids
            { "Teal",    0 }, { "Yellow", 1 }, { "Orange", 2 }, { "White", 3 },
            { "Green",   4 }, { "Purple", 5 }, { "Red",    6 }, { "Blue",  7 },
            { "Black",  11 },
            // Multi-colour patterns / gradients (8-10 are PNG patterns; 12+ are
            // built-in GradientMaterial presets — see preset_material()).
            { "Rainbow", 8 }, { "Cool",    9 }, { "Warm",  10 },
            { "Sunset", 12 }, { "Ocean",  13 }, { "Forest",14 }, { "Fire",  15 },
            { "Aurora", 16 }, { "Lava",   17 }, { "Galaxy",18 }, { "Pastel",19 },
            { "Candy",  20 }, { "Toxic",  21 },
        };
        for (const auto& m : pf_mats)
            pf_palette.push_back(leaf_sel(m.label,
                [teensy, idx = m.idx, &state]{
                    teensy->set_menu_item(8, idx);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.material_color = idx;
                },
                [&state, idx = m.idx]{ return state.face.material_color == idx; }));

        // ── Pride flags ──────────────────────────────────────────────────────
        // Vertical smooth gradients matching each flag's colours (presets 22-33;
        // see NativeFaceController::preset_material). Grouped in a submenu so the
        // top-level Material Color list stays compact.
        struct PFFlag { const char* label; uint8_t idx; };
        const PFFlag pf_pride[] = {
            { "Rainbow",     22 }, { "Progress",    23 }, { "Trans",       24 },
            { "Bisexual",    25 }, { "Pansexual",   26 }, { "Lesbian",     27 },
            { "Nonbinary",   28 }, { "Asexual",     29 }, { "Genderfluid", 30 },
            { "Genderqueer", 31 }, { "Aromantic",   32 }, { "Intersex",    33 },
        };
        std::vector<MenuItem> pride_items;
        // Hard-edged distinct stripes vs a smooth blend, for every flag below.
        pride_items.push_back(with_desc(
            toggle("Sharp Bands",
                [&state]{ std::lock_guard<std::mutex> lk(state.mtx); return state.face.pride_sharp; },
                [teensy, &state](bool v){
                    teensy->set_menu_item(10, v ? 1 : 0);   // 10 = native pride sharp-bands
                    uint8_t idx;
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.face.pride_sharp = v;
                        idx = state.face.material_color;
                    }
                    // Re-apply the current flag so the change shows immediately.
                    if (idx >= 22 && idx <= 33) teensy->set_menu_item(8, idx);
                }),
            "Draw each flag as hard-edged distinct stripes (real-flag look) "
            "instead of a smooth blend between colours."));
        // Rotate the flag stripes. Passed in 15° units (set_menu_item is a byte).
        pride_items.push_back(with_desc(
            slider("Rotation", 0.f, 360.f, 15.f, "\xc2\xb0",
                [&state]{ std::lock_guard<std::mutex> lk(state.mtx); return static_cast<float>(state.face.pride_angle); },
                [teensy, &state](float v){
                    int ang = (static_cast<int>(v) % 360 + 360) % 360;
                    teensy->set_menu_item(11, static_cast<uint8_t>(ang / 15));  // 11 = pride rotation
                    uint8_t idx;
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.face.pride_angle = ang;
                        idx = state.face.material_color;
                    }
                    if (idx >= 22 && idx <= 33) teensy->set_menu_item(8, idx);  // re-apply live
                }),
            "Rotate the flag stripes. 90\xc2\xb0 is the usual vertical stripes; "
            "0\xc2\xb0 lays them left\xe2\x86\x92right, other values give diagonals."));
        for (const auto& f : pf_pride)
            pride_items.push_back(leaf_sel(f.label,
                [teensy, idx = f.idx, &state]{
                    teensy->set_menu_item(8, idx);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.material_color = idx;
                },
                [&state, idx = f.idx]{ return state.face.material_color == idx; }));
        pf_palette.push_back(with_desc(submenu("Pride", std::move(pride_items)),
            "Colour gradients matching pride flags (vertical stripes, top \xe2\x86\x92 "
            "bottom). Applies like any other material; persists with your setup."));

        // Custom solid colour (moved here from the removed Face Color menu) —
        // an arbitrary flat colour via the RGB/hex picker, applied as a
        // SolidMaterial like the named solids above.
        pf_palette.push_back(color_picker("Custom Color",
            [teensy, &state](uint8_t r, uint8_t g, uint8_t b){
                teensy->set_color(r, g, b);
                // Remember the choice so the picker re-opens on it (get_color
                // below reads these back — otherwise it always shows the default).
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.r = r; state.face.g = g; state.face.b = b;
            },
            [&state]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                std::lock_guard<std::mutex> lk(state.mtx);
                return { state.face.r, state.face.g, state.face.b };
            }));
    }

    // ── Custom Gradient — multi-colour material, optionally scrolling ─────────
    // Appended to the Material Color list. Every edit previews live via
    // pf_set_material (native renderer only); persisted to cfg["protoface"].
    if (pf_gradient_p) {
        auto* G = pf_gradient_p;
        auto apply_grad = [G, pf_set_material]{
            if (pf_set_material) pf_set_material(pf_gradient_spec(*G));
        };
        std::vector<MenuItem> grad_items;

        std::vector<MenuItem> gcount_items;
        for (int n = 2; n <= 6; ++n)
            gcount_items.push_back(leaf_sel(std::to_string(n) + " colors",
                [G, n, apply_grad]{ G->count = n; apply_grad(); },
                [G, n]{ return G->count == n; }));
        grad_items.push_back(submenu("Colors", std::move(gcount_items)));

        for (int i = 0; i < 6; ++i) {
            MenuItem cp = color_picker(std::string("Color ") + std::to_string(i + 1),
                [G, i, apply_grad](uint8_t r, uint8_t g, uint8_t b){
                    G->colors[i] = {r, g, b}; apply_grad();
                },
                [G, i]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                    return { static_cast<uint8_t>(G->colors[i][0]),
                             static_cast<uint8_t>(G->colors[i][1]),
                             static_cast<uint8_t>(G->colors[i][2]) };
                });
            cp.visible_fn = [G, i]{ return i < G->count; };
            grad_items.push_back(std::move(cp));
        }

        grad_items.push_back(toggle("Smooth Blend",
            [G]{ return G->smooth; },
            [G, apply_grad](bool v){ G->smooth = v; apply_grad(); }));

        grad_items.push_back(with_desc(toggle("Mirror at Center",
            [G]{ return G->mirror; },
            [G, apply_grad](bool v){ G->mirror = v; apply_grad(); }),
            "Reflect the gradient about the centre so both halves match, instead "
            "of stretching one continuous ramp across the face."));

        grad_items.push_back(with_desc(
            slider("Rotation", 0.f, 360.f, 15.f, "\xc2\xb0",
                [G]{ return static_cast<float>(G->angle); },
                [G, apply_grad](float v){ G->angle = static_cast<int>(v) % 360; apply_grad(); }),
            "Rotate the gradient direction. 0\xc2\xb0 runs left\xe2\x86\x92right, "
            "90\xc2\xb0 top\xe2\x86\x92bottom; values in between give diagonals."));

        grad_items.push_back(slider("Speed", -60.f, 60.f, 1.f, " px/s",
            [G]{ return static_cast<float>(G->speed); },
            [G, apply_grad](float v){ G->speed = static_cast<int>(v); apply_grad(); }));

        grad_items.push_back(leaf("Use This Gradient", apply_grad));

        pf_palette.push_back(with_desc(
            submenu("Custom Gradient", std::move(grad_items)),
            "Multi-colour gradient painted behind the face. Pick 2–6 colours, "
            "Smooth or banded, a flow Direction and a Speed to scroll them "
            "behind the mask (0 = static). Previews live; persisted to "
            "cfg[\"protoface\"][\"gradient\"]."));
    }

    std::vector<MenuItem> pf_gifs;
    for (uint8_t i = 0; i < 8; i++) pf_gifs.push_back(gif_leaf(i));

    // ── Protoface > Hardware > Backend ────────────────────────────────────
    // Switches NativeFaceController between HUB75 panels (via piomatter
    // + panel_driver.py) and direct-to-spidev MAX7219 matrices. The toggle
    // fires a hot-swap callback main.cpp owns; HUD keeps rendering through
    // the transition.
    std::vector<MenuItem> pf_backend_items = {
        leaf_sel("HUB75 panels (piomatter)",
            [swap_backend]{ if (swap_backend) swap_backend("hub75"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "hub75"; }),
        leaf_sel("MAX7219 matrices (direct SPI)",
            [swap_backend]{ if (swap_backend) swap_backend("max7219"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "max7219"; }),
        leaf_sel("RGB matrix (WS2812 drop-in for MAX7219)",
            [swap_backend]{ if (swap_backend) swap_backend("rgb_matrix"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "rgb_matrix"; }),
    };
    // Chain Layout pickers — drive the editor's eye/nose/mouth bounding
    // boxes. Each radio mutates the live string the editor reads when it
    // opens, so the next "Edit…" shows the updated zones. Only shown for
    // pixel-grid backends (MAX7219 / RGB matrix); HUB75 has no per-zone
    // chain concept.
    auto layout_pick = [](const char* lbl, std::string* slot,
                                   const char* value) -> MenuItem {
        return leaf_sel(lbl,
            [slot, value]{ if (slot) *slot = value; },
            [slot, value]{ return slot && *slot == value; });
    };

    // Context-panel preview for a chain-layout submenu (Eyes / Mouth /
    // Nose). Renders a labelled grid sized to the active layout, with
    // bold yellow borders drawn between each 8x8 LED module so the user
    // can see at a glance how many panels they're configuring. The "none"
    // pick shows a "Disabled" placeholder instead of a grid.
    auto chain_layout_preview = [](const std::string& zone_name,
                                   std::string* layout_ptr) -> MenuContextPanelDraw {
        return [zone_name, layout_ptr](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            // Header: zone name + pixel dimensions for the active layout.
            const ImU32 head_col = IM_COL32(230, 235, 240, 255);
            const ImU32 sub_col  = IM_COL32(170, 185, 200, 200);
            const ImU32 frame_bg = IM_COL32(20, 24, 32, 255);
            const ImU32 cell_col = IM_COL32(255, 255, 255, 30);
            const ImU32 mod_col  = IM_COL32(255, 220, 60, 255);
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            const std::string layout = layout_ptr ? *layout_ptr : std::string();
            int rows = 0, cols = 0;
            if (layout.size() == 3 && layout[1] == 'x' &&
                layout[0] >= '0' && layout[0] <= '9' &&
                layout[2] >= '0' && layout[2] <= '9') {
                rows = layout[0] - '0';
                cols = layout[2] - '0';
            }

            char title[64];
            std::snprintf(title, sizeof(title), "%s", zone_name.c_str());
            dl->AddText(font, fs * 1.4f, {o.x, o.y}, head_col, title);

            char sub[96];
            if (layout == "none" || rows == 0 || cols == 0) {
                std::snprintf(sub, sizeof(sub), "Disabled");
            } else {
                std::snprintf(sub, sizeof(sub),
                              "%d row × %d col   (%d × %d px)",
                              rows, cols, cols * 8, rows * 8);
            }
            dl->AddText(font, fs * 0.95f, {o.x, o.y + fs * 1.5f}, sub_col, sub);

            if (rows == 0 || cols == 0) return;

            // Fit the schematic into the remaining context-pane space, with
            // 8px padding on each side and a top margin below the header.
            const float top = o.y + fs * 3.2f;
            const float pad = 8.f;
            const float avail_w = std::max(20.f, sz.x - pad * 2.f);
            const float avail_h = std::max(20.f, o.y + sz.y - top - pad);
            const int   px_w    = cols * 8;
            const int   px_h    = rows * 8;
            const float cell    = std::max(1.f,
                std::floor(std::min(avail_w / px_w, avail_h / px_h)));
            const float grid_w  = cell * px_w;
            const float grid_h  = cell * px_h;
            const float gx      = o.x + (sz.x - grid_w) * 0.5f;
            const float gy      = top + (avail_h - grid_h) * 0.5f;

            // Background fill.
            dl->AddRectFilled({gx, gy}, {gx + grid_w, gy + grid_h}, frame_bg);

            // Faint per-pixel grid — only when cells are big enough to read.
            if (cell >= 6.f) {
                for (int x = 1; x < px_w; ++x) {
                    const float xx = gx + x * cell;
                    dl->AddLine({xx, gy}, {xx, gy + grid_h}, cell_col, 1.f);
                }
                for (int y = 1; y < px_h; ++y) {
                    const float yy = gy + y * cell;
                    dl->AddLine({gx, yy}, {gx + grid_w, yy}, cell_col, 1.f);
                }
            }

            // Bold module borders every 8 cells — the visual cue the user
            // asked for: "borders drawn between each 8x8 matrix."
            for (int c = 0; c <= cols; ++c) {
                const float xx = gx + c * 8.f * cell;
                dl->AddLine({xx, gy}, {xx, gy + grid_h}, mod_col, 2.f);
            }
            for (int r = 0; r <= rows; ++r) {
                const float yy = gy + r * 8.f * cell;
                dl->AddLine({gx, yy}, {gx + grid_w, yy}, mod_col, 2.f);
            }
        };
    };
    auto visible_for_native = [pf_backend_p] {
        return pf_backend_p &&
               (*pf_backend_p == "max7219" || *pf_backend_p == "rgb_matrix");
    };
    std::vector<MenuItem> eye_items = {
        layout_pick("1x2  (16x8)",  pf_eye_layout_p, "1x2"),
        layout_pick("1x3  (24x8)",  pf_eye_layout_p, "1x3"),
        layout_pick("2x2  (16x16)", pf_eye_layout_p, "2x2"),
        layout_pick("2x3  (24x16)", pf_eye_layout_p, "2x3"),
    };
    std::vector<MenuItem> mouth_items = {
        layout_pick("1x3  (24x8)",  pf_mouth_layout_p, "1x3"),
        layout_pick("1x4  (32x8)",  pf_mouth_layout_p, "1x4"),
        layout_pick("2x3  (24x16)", pf_mouth_layout_p, "2x3"),
        layout_pick("2x4  (32x16)", pf_mouth_layout_p, "2x4"),
    };
    std::vector<MenuItem> nose_items = {
        layout_pick("None",         pf_nose_layout_p, "none"),
        layout_pick("1x1  (8x8)",   pf_nose_layout_p, "1x1"),
        layout_pick("1x2  (16x8)",  pf_nose_layout_p, "1x2"),
        layout_pick("1x3  (24x8)",  pf_nose_layout_p, "1x3"),
    };
    std::vector<MenuItem> pf_chain_layout_items = {
        with_desc(with_panel(submenu("Eyes",  std::move(eye_items)),
                             "Eyes",  chain_layout_preview("Eyes",  pf_eye_layout_p)),
                  "Panels per eye. The right eye is mirrored on the same row "
                  "so both eyes match. Used by the face editor to outline "
                  "Left Eye / Right Eye zones."),
        with_desc(with_panel(submenu("Mouth", std::move(mouth_items)),
                             "Mouth", chain_layout_preview("Mouth", pf_mouth_layout_p)),
                  "Mouth panels (anchor 4,25). Used by the face editor to "
                  "outline the Mouth zone."),
        with_desc(with_panel(submenu("Nose",  std::move(nose_items)),
                             "Nose",  chain_layout_preview("Nose",  pf_nose_layout_p)),
                  "Nose panels (single row, centred). \"None\" omits the "
                  "nose zone entirely."),
    };
    MenuItem pf_chain_layout_item = with_desc(
        submenu("Chain Layout", std::move(pf_chain_layout_items)),
        "Pick panels per zone — drives the bounding boxes the face editor "
        "highlights so you can see which canvas pixels each panel will "
        "display. Persisted to config.json.");
    pf_chain_layout_item.visible_fn = visible_for_native;

    // ── HUB75 Panel Layout (presets + per-panel pixel nudge) ─────────────────
    // Visible only on the hub75 backend. Picks panel size + count + arrangement
    // and exposes a per-panel Nudge X/Y so the user can shift any single panel
    // ±32 px to compensate for tiny mounting offsets between physical panels.
    MenuItem pf_hub75_layout_item;
    MenuItem pf_color_order_item;
    MenuItem pf_camera_mode_item;
    if (pf_hub75_p) {
        auto* H = pf_hub75_p;
        // String picker that also reapplies the auto-placed centre offsets
        // when the global Default Panel Size / Arrangement changes — the
        // canvas resizes around the new geometry, so the existing nudges
        // would point at the wrong canvas centres.
        auto hub_pick_str = [H](const char* lbl, std::string* slot, const char* v) {
            return leaf_sel(lbl,
                [slot, v, H]{ if (slot) { *slot = v; pf_hub75_apply_defaults(*H); } },
                [slot, v]{ return slot && *slot == v; });
        };
        auto hub_pick_int = [H](const char* lbl, int* slot, int v) {
            return leaf_sel(lbl,
                [slot, v, H]{ if (slot) { *slot = v; pf_hub75_apply_defaults(*H); } },
                [slot, v]{ return slot && *slot == v; });
        };
        std::vector<MenuItem> size_items = {
            hub_pick_str("32x16",  &H->panel_size, "32x16"),
            hub_pick_str("64x32",  &H->panel_size, "64x32"),
            hub_pick_str("64x64",  &H->panel_size, "64x64"),
            hub_pick_str("96x48",  &H->panel_size, "96x48"),
            hub_pick_str("128x32", &H->panel_size, "128x32"),
            hub_pick_str("128x64", &H->panel_size, "128x64"),
        };
        std::vector<MenuItem> count_items = {
            hub_pick_int("1 panel",  &H->panel_count, 1),
            hub_pick_int("2 panels", &H->panel_count, 2),
            hub_pick_int("3 panels", &H->panel_count, 3),
            hub_pick_int("4 panels", &H->panel_count, 4),
        };
        std::vector<MenuItem> arr_items = {
            hub_pick_str("Horizontal Chain", &H->arrangement, "horizontal"),
            hub_pick_str("Vertical Stack",   &H->arrangement, "vertical"),
            hub_pick_str("2x2 Grid",         &H->arrangement, "grid2x2"),
        };

        // Per-panel submenus carrying that slot's Size override + Nudge.
        // "Use Default" tracks the global Panel Size picker; the six other
        // entries pin this slot to that physical size regardless of the
        // global. Visible only when the panel index is within the active
        // count.
        // Per-slot Size pickers also reapply defaults — sizing a panel up
        // can grow the canvas, which shifts what "centre" means.
        auto size_pick = [H](const char* lbl, int i, const char* v) {
            return leaf_sel(lbl,
                [H, i, v]{ H->panel_size_per[i] = v; pf_hub75_apply_defaults(*H); },
                [H, i, v]{ return H->panel_size_per[i] == v; });
        };
        // Slider range: ±canvas/2 so each panel can be moved to either
        // edge of the canvas (centre-offset model). Capped at ±512 for
        // very large layouts so the encoder stays usable.
        auto nudge_x_max = [H]() -> float {
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            return static_cast<float>(std::min(512, std::max(64, cw / 2)));
        };
        auto nudge_y_max = [H]() -> float {
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            return static_cast<float>(std::min(512, std::max(64, ch / 2)));
        };
        std::vector<MenuItem> nudge_items;
        for (int i = 0; i < 4; ++i) {
            std::vector<MenuItem> size_items = {
                size_pick("Use Default", i, ""),
                size_pick("32x16",       i, "32x16"),
                size_pick("64x32",       i, "64x32"),
                size_pick("64x64",       i, "64x64"),
                size_pick("96x48",       i, "96x48"),
                size_pick("128x32",      i, "128x32"),
                size_pick("128x64",      i, "128x64"),
            };
            const float xmax = nudge_x_max();
            const float ymax = nudge_y_max();
            std::vector<MenuItem> axis_items = {
                submenu("Size", std::move(size_items)),
                slider("Offset X (from centre)", -xmax, xmax, 1.f, " px",
                    [H, i]{ return static_cast<float>(H->nudge_dx[i]); },
                    [H, i](float v){ H->nudge_dx[i] = static_cast<int>(v); }),
                slider("Offset Y (from centre)", -ymax, ymax, 1.f, " px",
                    [H, i]{ return static_cast<float>(H->nudge_dy[i]); },
                    [H, i](float v){ H->nudge_dy[i] = static_cast<int>(v); }),
                // Per-panel orientation. Pushed to the live renderer via
                // pf_layout_changed so the flip is visible on the panels
                // immediately (geometry edits above still need a restart/
                // backend round-trip; flips don't).
                toggle("Flip Horizontal",
                    [H, i]{ return H->flip_x[i]; },
                    [H, i, pf_layout_changed](bool v){
                        H->flip_x[i] = v;
                        if (pf_layout_changed) pf_layout_changed();
                    }),
                toggle("Flip Vertical",
                    [H, i]{ return H->flip_y[i]; },
                    [H, i, pf_layout_changed](bool v){
                        H->flip_y[i] = v;
                        if (pf_layout_changed) pf_layout_changed();
                    }),
                leaf("Reset to Default Position",
                     [H, i]{
                         // Reset just this slot by reapplying defaults to a
                         // scratch copy and copying the slot's values back.
                         PfHub75Layout tmp = *H;
                         pf_hub75_apply_defaults(tmp);
                         H->nudge_dx[i] = tmp.nudge_dx[i];
                         H->nudge_dy[i] = tmp.nudge_dy[i];
                     }),
            };
            char nm[32]; std::snprintf(nm, sizeof(nm), "Panel %d", i + 1);
            MenuItem m = submenu(nm, std::move(axis_items));
            // Dynamic label so the parent shows each slot's effective size.
            m.label_fn = [H, i]{
                const std::string& s = H->panel_size_per[i].empty()
                                       ? H->panel_size : H->panel_size_per[i];
                return std::string("Panel ") + std::to_string(i + 1)
                       + " (" + s + ")";
            };
            m.visible_fn = [H, i]{ return i < H->panel_count; };
            nudge_items.push_back(std::move(m));
        }

        // Schematic preview — draws all panels at their canvas positions
        // (after nudge) so the user sees the whole-set arrangement update
        // live as they tweak picker rows or nudge sliders.
        auto hub_preview = [H](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            dl->AddText(font, fs * 1.1f, {o.x, o.y},
                        IM_COL32(230, 235, 240, 255), "HUB75 Panel Layout");
            const auto panels = pf_hub75_panels(*H);
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            char sub[96];
            std::snprintf(sub, sizeof(sub),
                          "%d × %s   %s   canvas %dx%d",
                          H->panel_count, H->panel_size.c_str(),
                          H->arrangement.c_str(), cw, ch);
            dl->AddText(font, fs * 0.85f, {o.x, o.y + fs * 1.2f},
                        IM_COL32(170, 180, 190, 220), sub);

            const float top = o.y + fs * 2.6f;
            const float pad = 8.f;
            const float avail_w = std::max(40.f, sz.x - pad * 2.f);
            const float avail_h = std::max(40.f, o.y + sz.y - top - pad);
            if (cw <= 0 || ch <= 0) return;
            const float scale = std::min(avail_w / cw, avail_h / ch);
            const float gw = cw * scale, gh = ch * scale;
            const float ox = o.x + (sz.x - gw) * 0.5f;
            const float oy = top + (avail_h - gh) * 0.5f;
            const ImU32 bg   = IM_COL32(20, 24, 32, 255);
            const ImU32 pcol = IM_COL32(255, 220, 60, 255);
            const ImU32 ptxt = IM_COL32(20, 24, 28, 255);
            dl->AddRectFilled({ox, oy}, {ox + gw, oy + gh}, bg);
            for (size_t i = 0; i < panels.size(); ++i) {
                const auto& r = panels[i].rect;
                const float x0 = ox + r.x * scale;
                const float y0 = oy + r.y * scale;
                const float x1 = x0 + r.width  * scale;
                const float y1 = y0 + r.height * scale;
                dl->AddRectFilled({x0, y0}, {x1, y1},
                                  IM_COL32(255, 220, 60, 60));
                dl->AddRect({x0, y0}, {x1, y1}, pcol, 2.f, 0, 2.f);
                char lab[16];
                std::snprintf(lab, sizeof(lab), "P%zu", i + 1);
                const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, lab);
                dl->AddText(font, fs,
                    {x0 + ((x1 - x0) - ts.x) * 0.5f,
                     y0 + ((y1 - y0) - ts.y) * 0.5f},
                    ptxt, lab);
            }
        };

        std::vector<MenuItem> hub_items;

        // ── Named-layout management (Save As / Load / Rename / Delete) ───────
        // Shown only when the host wired through the named-layout pointers.
        // Operations work on the live `*H` so the user's in-progress edits to
        // the active layout are preserved when switching.
        if (pf_hub75_layouts_p && pf_hub75_active_p) {
            auto* M = pf_hub75_layouts_p;
            auto* A = pf_hub75_active_p;
            auto layout_changed = pf_layout_changed;
            // Helper: ith name in the map's iteration order (stable for std::map).
            auto nth_name = [M](int i) -> std::string {
                if (i < 0) return {};
                int k = 0;
                for (const auto& [n, _] : *M) { if (k++ == i) return n; }
                return {};
            };

            // Save As — copy the current working layout into a new named slot
            // and switch the working copy onto it.
            hub_items.push_back(with_desc(
                leaf("Save As...", [H, M, A, layout_changed, menu_sys_pp]{
                    if (!menu_sys_pp || !*menu_sys_pp) return;
                    (*menu_sys_pp)->open_keyboard("Layout Name", *A,
                        [H, M, A, layout_changed](const std::string& name){
                            if (name.empty()) return;
                            (*M)[*A]   = *H;          // checkpoint current edits
                            (*M)[name] = *H;          // copy into new slot
                            *A = name;
                            if (layout_changed) layout_changed();
                        });
                }),
                "Save the panels + nudges you've configured here as a new "
                "named layout. Subsequent edits go to the new layout; switch "
                "back via Load Layout."));

            // Load — pre-allocated slot rows (make_dynamic_rows); each one's
            // label_fn pulls the i-th map entry and the action loads it.
            // Hidden when i is past the current map size.
            std::vector<MenuItem> load_items = make_dynamic_rows(16,
                [M]{ return static_cast<int>(M->size()); },
                [&](int i) -> MenuItem {
                    MenuItem li;
                    li.type  = MenuItemType::LEAF;
                    li.label = "layout";
                    li.label_fn = [nth_name, A, i]{
                        const std::string nm = nth_name(i);
                        if (nm.empty()) return std::string();
                        return (*A == nm) ? (nm + "  *") : nm;
                    };
                    li.action = [H, M, A, layout_changed, nth_name, i]{
                        const std::string nm = nth_name(i);
                        if (nm.empty() || *A == nm) return;
                        (*M)[*A] = *H;
                        *A = nm;
                        *H = (*M)[nm];
                        if (layout_changed) layout_changed();
                    };
                    return li;
                });
            MenuItem load_sub = submenu("Load Layout", std::move(load_items));
            load_sub.label_fn = [A]{ return std::string("Load Layout  (") + *A + ")"; };
            load_sub.description =
                "Switch to a different saved layout. The current layout's "
                "edits are checkpointed first. * marks the active one.";
            hub_items.push_back(std::move(load_sub));

            // Rename — replace the active layout's key without copying data.
            hub_items.push_back(with_desc(
                leaf("Rename Active...", [H, M, A, layout_changed, menu_sys_pp]{
                    if (!menu_sys_pp || !*menu_sys_pp) return;
                    const std::string old_name = *A;
                    (*menu_sys_pp)->open_keyboard("Layout Name", old_name,
                        [H, M, A, layout_changed, old_name](const std::string& name){
                            if (name.empty() || name == old_name) return;
                            (*M)[old_name] = *H;
                            (*M)[name] = (*M)[old_name];
                            M->erase(old_name);
                            *A = name;
                            if (layout_changed) layout_changed();
                        });
                }),
                "Rename the active layout. Face PNGs stamped with the old "
                "name will read as a mismatch until you re-import them."));

            // Delete — only when there's at least one other layout to fall back to.
            MenuItem del = leaf("Delete Active",
                [H, M, A, layout_changed]{
                    if (M->size() <= 1) return;
                    M->erase(*A);
                    *A = M->begin()->first;
                    *H = M->begin()->second;
                    if (layout_changed) layout_changed();
                });
            del.label_fn = [A]{ return std::string("Delete \"") + *A + "\""; };
            del.visible_fn = [M]{ return M->size() > 1; };
            del.description = "Remove this layout. The first remaining "
                              "saved layout becomes active.";
            hub_items.push_back(std::move(del));
        }

        hub_items.push_back(
            with_desc(submenu("Default Panel Size", std::move(size_items)),
                      "Size applied to every panel slot whose Panel N > Size "
                      "is set to \"Use Default\". Per-slot overrides let a "
                      "build mix sizes."));
        hub_items.push_back(submenu("Panel Count", std::move(count_items)));
        hub_items.push_back(submenu("Arrangement", std::move(arr_items)));
        {
            // Panel color-channel order. "Auto" = the pinout's default;
            // explicit orders fix oddly-wired panels (red/green swapped →
            // GRB, red/blue swapped → BGR). Applies by relaunching the
            // panel driver so the fix shows immediately. Lives under
            // Protoface > Hardware (it's wiring, not layout), but the value
            // is stored per HUB75 layout so builds can differ.
            auto restart = pf_restart_renderer;
            auto order_pick = [H, restart](const char* lbl, const char* v) {
                return leaf_sel(lbl,
                    [H, restart, v]{
                        H->color_order = v;
                        if (restart) restart();
                    },
                    [H, v]{ return H->color_order == v; });
            };
            std::vector<MenuItem> order_items = {
                order_pick("Auto (board default)", "auto"),
                order_pick("RGB", "rgb"),
                order_pick("RBG", "rbg"),
                order_pick("GRB", "grb"),
                order_pick("GBR", "gbr"),
                order_pick("BRG", "brg"),
                order_pick("BGR", "bgr"),
            };
            pf_color_order_item = with_desc(
                submenu("Color Order", std::move(order_items)),
                "Color-channel order the HUB75 panels expect. Auto uses the "
                "bonnet's default (straight RGB on the Adafruit bonnet, the "
                "Active-3 rotate on active3). If red and green are swapped "
                "pick GRB; red/blue swapped is BGR; otherwise try options "
                "until the colors look right. Selecting restarts the panel "
                "driver so it applies immediately.");
            pf_color_order_item.label_fn = [H]{
                return std::string("Color Order  (") +
                       (H->color_order == "auto" ? "auto" : H->color_order) + ")";
            };
            pf_color_order_item.visible_fn = [pf_backend_p]{
                return pf_backend_p && *pf_backend_p == "hub75";
            };
        }
        {
            // Camera Mode (piomatter): drives the panels with extra temporal
            // dithering / bit planes so the face reads cleanly on video (less
            // banding/flicker). Relaunches the panel driver, same as Color Order.
            auto restart = pf_restart_renderer;
            pf_camera_mode_item = with_desc(
                toggle("Camera Mode (flicker-free)",
                    [H]{ return H->camera_mode; },
                    [H, restart](bool v){ H->camera_mode = v; if (restart) restart(); }),
                "Drives the HUB75 panels with extra temporal dithering / bit "
                "planes so the face doesn't band or flicker when filmed. A "
                "tune-and-test knob (piomatter's PIO refresh is already stable); "
                "fine-tune camera_planes / camera_temporal_planes in config if "
                "needed. Restarts the panel driver so it applies immediately.");
            pf_camera_mode_item.visible_fn = [pf_backend_p]{
                return pf_backend_p && *pf_backend_p == "hub75";
            };
        }
        for (auto& it : nudge_items) hub_items.push_back(std::move(it));
        hub_items.push_back(with_desc(
            leaf("Reset All Positions",
                 [H]{ pf_hub75_apply_defaults(*H); }),
            "Re-seed every panel's Offset X/Y to the auto-placed positions "
            "for the current size + count + arrangement."));

        pf_hub75_layout_item = with_desc(
            with_panel(submenu("HUB75 Layout", std::move(hub_items)),
                       "HUB75 Panel Layout", hub_preview),
            "Panel size + count + arrangement for the HUB75 backend. "
            "Each panel exposes a Nudge X/Y slider (±32 px integer) so you "
            "can align the editor canvas to physical mounting offsets, plus "
            "Flip Horizontal / Vertical toggles to match how each panel is "
            "mounted/wired (these apply to the panels live). "
            "Save As / Load lets you keep multiple named layouts (e.g. one "
            "per helmet build); faces are stamped with the active layout "
            "name on import. Persisted to cfg[\"protoface\"][\"hub75_layouts\"].");
        if (pf_hub75_active_p) {
            auto* A = pf_hub75_active_p;
            pf_hub75_layout_item.label_fn = [A]{
                return std::string("HUB75 Layout  (") + *A + ")";
            };
        }
        pf_hub75_layout_item.visible_fn = [pf_backend_p]{
            return pf_backend_p && *pf_backend_p == "hub75";
        };
    }

    // ── MAX7219 Layout editor (coproc-driven panels) ─────────────────────────
    // A ragged grid of 8×8 modules the user builds live: rows, panels per row,
    // and the daisy-chain order — with a Wiring diagram showing the DIN→DOUT
    // path and which coprocessor pins to connect. Runs beside HUB75 (Section)
    // or as the whole face (Main).
    MenuItem pf_max7219_layout_item;
    if (ctx.pf_max7219_p) {
        auto* MX  = ctx.pf_max7219_p;
        auto  app = ctx.pf_max7219_apply;
        auto  reapply = [app]{ if (app) app(); };

        // Edits mutate the working copy live (the Wiring diagram updates as you
        // go); the panels are (re)built only on Enabled and "Apply Layout", since
        // a rebuild tears down and restarts the renderer — too heavy per slider
        // tick. Same "applies on a round-trip" model as the HUB75 layout editor.
        std::vector<MenuItem> mx_items;
        mx_items.push_back(toggle("Enabled",
            [MX]{ return MX->enabled; },
            [MX, reapply](bool v){ MX->enabled = v; reapply(); }));

        std::vector<MenuItem> mode_items = {
            leaf_sel("Section (beside HUB75)",
                [MX]{ MX->mode = "section"; },
                [MX]{ return MX->mode != "main"; }),
            leaf_sel("Main (the whole face)",
                [MX]{ MX->mode = "main"; },
                [MX]{ return MX->mode == "main"; }),
        };
        mx_items.push_back(submenu("Mode", std::move(mode_items)));

        // Section content source: mirror the face-canvas region, or show
        // independent triggerable symbols/text/patterns (the library below).
        std::vector<MenuItem> content_items = {
            leaf_sel("Mirror Face Region",
                [MX]{ MX->content = "face"; },
                [MX]{ return MX->content != "symbols"; }),
            leaf_sel("Symbols / Text / Patterns",
                [MX]{ MX->content = "symbols"; },
                [MX]{ return MX->content == "symbols"; }),
        };
        mx_items.push_back(with_desc(submenu("Content", std::move(content_items)),
            "What the section panels show: a crop of the face, or independent "
            "symbols/text/patterns you trigger (Content Library below, a max_* "
            "button, or `echo max_symbol:heart > /run/protohud/cmd`)."));

        // Content Library — live triggers (need a running section controller).
        if (ctx.pf_max_content) {
            auto trig = ctx.pf_max_content;
            std::vector<MenuItem> sym_items;
            for (const auto& s : face::max_content::symbol_names())
                sym_items.push_back(leaf(s, [trig, s]{ trig("symbol", s); }));
            std::vector<MenuItem> pat_items;
            for (const auto& p : face::max_content::pattern_names())
                pat_items.push_back(leaf(p, [trig, p]{ trig("pattern", p); }));
            std::vector<MenuItem> lib_items;
            lib_items.push_back(submenu("Symbols", std::move(sym_items)));
            lib_items.push_back(submenu("Patterns", std::move(pat_items)));
            lib_items.push_back(leaf("Next Symbol",  [trig]{ trig("next", ""); }));
            lib_items.push_back(leaf("Prev Symbol",  [trig]{ trig("prev", ""); }));
            lib_items.push_back(leaf("Clear",        [trig]{ trig("clear", ""); }));
            mx_items.push_back(with_desc(submenu("Content Library", std::move(lib_items)),
                "Show a symbol or pattern on the section panels now. Text is set "
                "over the FIFO (`echo max_text:HELLO > /run/protohud/cmd`). Only "
                "active when Content = Symbols and the panels are wired + enabled."));
        }

        mx_items.push_back(slider("Rows", 1.f, 8.f, 1.f, "",
            [MX]{ return static_cast<float>(MX->rows.size()); },
            [MX](float v){ MX->rows.resize(std::clamp(static_cast<int>(v), 1, 8), 1); }));
        for (int r = 0; r < 8; ++r) {
            MenuItem row = slider("Row " + std::to_string(r + 1) + " panels",
                1.f, 16.f, 1.f, "",
                [MX, r]{ return r < static_cast<int>(MX->rows.size())
                                    ? static_cast<float>(MX->rows[r]) : 1.f; },
                [MX, r](float v){
                    if (r < static_cast<int>(MX->rows.size()))
                        MX->rows[r] = std::clamp(static_cast<int>(v), 1, 16);
                });
            row.visible_fn = [MX, r]{ return r < static_cast<int>(MX->rows.size()); };
            mx_items.push_back(std::move(row));
        }

        std::vector<MenuItem> order_items = {
            leaf_sel("Serpentine (zig-zag)",
                [MX]{ MX->chain_order = "serpentine"; },
                [MX]{ return MX->chain_order != "row_major"; }),
            leaf_sel("Row-major (every row L\xe2\x86\x92R)",
                [MX]{ MX->chain_order = "row_major"; },
                [MX]{ return MX->chain_order == "row_major"; }),
        };
        mx_items.push_back(submenu("Chain Order", std::move(order_items)));

        std::vector<MenuItem> type_items = {
            leaf_sel("FC16 (4-in-1 boards)",
                [MX]{ MX->module_type = "fc16"; },
                [MX]{ return MX->module_type != "generic1088"; }),
            leaf_sel("Generic 1088AS",
                [MX]{ MX->module_type = "generic1088"; },
                [MX]{ return MX->module_type == "generic1088"; }),
        };
        mx_items.push_back(submenu("Module Type", std::move(type_items)));

        mx_items.push_back(slider("Brightness", 0.f, 15.f, 1.f, "",
            [MX]{ return static_cast<float>(MX->intensity); },
            [MX](float v){ MX->intensity = std::clamp(static_cast<int>(v), 0, 15); }));
        mx_items.push_back(slider("Coproc CS index", 0.f, 3.f, 1.f, "",
            [MX]{ return static_cast<float>(MX->coproc_cs); },
            [MX](float v){ MX->coproc_cs = std::clamp(static_cast<int>(v), 0, 3); }));
        mx_items.push_back(leaf("Apply Layout", [reapply]{ reapply(); }));

        // Wiring diagram — module grid numbered in DIN→DOUT order, arrows along
        // the daisy chain, DIN/DOUT markers, and the coproc pin legend.
        auto wiring = [MX](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            dl->AddText(font, fs * 1.1f, {o.x, o.y},
                        IM_COL32(230, 235, 240, 255), "MAX7219 Wiring");

            const auto mods = pf_max7219_modules(*MX);
            const int total = static_cast<int>(mods.size());
            int maxx = 8, maxy = 8;
            for (const auto& m : mods) { maxx = std::max(maxx, m[0] + 8); maxy = std::max(maxy, m[1] + 8); }
            char sub[96];
            std::snprintf(sub, sizeof(sub), "%d modules   %dx%d px   %s",
                          total, maxx, maxy, MX->chain_order.c_str());
            dl->AddText(font, fs * 0.85f, {o.x, o.y + fs * 1.2f},
                        IM_COL32(170, 180, 190, 220), sub);
            dl->AddText(font, fs * 0.8f, {o.x, o.y + fs * 2.2f},
                        IM_COL32(120, 200, 140, 235),
                        "DIN=GP11  CLK=GP10  CS=GP13  +5V  GND");

            const float top = o.y + fs * 3.4f;
            const float pad = 8.f;
            const float aw = std::max(40.f, sz.x - pad * 2.f);
            const float ah = std::max(40.f, o.y + sz.y - top - pad);
            if (total == 0 || maxx <= 0 || maxy <= 0) return;
            const float scale = std::min(aw / maxx, ah / maxy);
            const float gw = maxx * scale, gh = maxy * scale;
            const float ox = o.x + (sz.x - gw) * 0.5f;
            const float oy = top + (ah - gh) * 0.5f;

            std::vector<ImVec2> ctr;
            ctr.reserve(total);
            for (int i = 0; i < total; ++i) {
                const float x0 = ox + mods[i][0] * scale, y0 = oy + mods[i][1] * scale;
                const float x1 = x0 + 8 * scale,          y1 = y0 + 8 * scale;
                dl->AddRectFilled({x0, y0}, {x1, y1}, IM_COL32(255, 220, 60, 50));
                dl->AddRect({x0, y0}, {x1, y1}, IM_COL32(255, 220, 60, 255), 2.f, 0, 1.5f);
                ctr.push_back({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f});
                char n[8]; std::snprintf(n, sizeof(n), "%d", i);
                const ImVec2 tsz = font->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0.f, n);
                dl->AddText(font, fs * 0.8f,
                            {ctr[i].x - tsz.x * 0.5f, ctr[i].y - tsz.y * 0.5f},
                            IM_COL32(235, 240, 245, 255), n);
            }
            for (int i = 0; i + 1 < total; ++i)
                dl->AddLine(ctr[i], ctr[i + 1], IM_COL32(120, 200, 255, 220), 1.5f);
            dl->AddText(font, fs * 0.8f, {ctr[0].x - 8, ctr[0].y - fs * 1.7f},
                        IM_COL32(120, 200, 140, 255), "DIN");
            dl->AddText(font, fs * 0.8f,
                        {ctr[total - 1].x - 14, ctr[total - 1].y + fs * 0.7f},
                        IM_COL32(255, 150, 120, 255), "DOUT");
        };

        pf_max7219_layout_item = with_desc(
            with_panel(submenu("MAX7219 Layout", std::move(mx_items)), "Wiring", wiring),
            "Build the MAX7219 panel grid: rows, panels per row, and the daisy-"
            "chain order. The Wiring panel shows the DIN\xe2\x86\x92DOUT path and "
            "which coprocessor pins to connect. Runs beside HUB75 (Section) or as "
            "the whole face (Main). Driven over the coprocessor \xe2\x80\x94 no CM5 GPIO.");
    }

    std::vector<MenuItem> pf_hardware_menu = {
        with_desc(submenu("Backend", std::move(pf_backend_items)),
                  "What LED hardware Protoface paints. Switching tears down "
                  "the running renderer and brings up a new one with the new "
                  "backend; the HUD keeps running through the transition. "
                  "Persists to config.json so the next launch starts here."),
    };
    if (ctx.pf_max7219_p) pf_hardware_menu.push_back(std::move(pf_max7219_layout_item));
    // HUB75 panel wiring fix-up — hidden on other backends.
    if (pf_hub75_p) pf_hardware_menu.push_back(std::move(pf_color_order_item));
    if (pf_hub75_p) pf_hardware_menu.push_back(std::move(pf_camera_mode_item));
    if (pf_restart_renderer) {
        pf_hardware_menu.push_back(with_desc(
            menu_shared::restart_face_renderer_leaf(pf_restart_renderer,
                                                    state_ptr, pf_backend_p),
            "Kill + relaunch the HUB75 panel pusher to recover the face feed "
            "(e.g. after the live GPIO read stole a panel pin), without "
            "restarting ProtoHUD."));
    }

    // The chain layout config used to live under Hardware, but it's really a
    // face-authoring concern (it shapes the editor's bbox guides), so it now
    // sits inside Face Options alongside the per-expression slots.
    face_files_menu.push_back(std::move(pf_chain_layout_item));
    if (pf_hub75_p) face_files_menu.push_back(std::move(pf_hub75_layout_item));

    // ── Size & Position — per-face placement transform ───────────────────────
    // Writes fit/scale/offset into the active face folder's config.json so a
    // face drawn for one panel size scales + sits correctly on another, then
    // reloads the loaders live. Honored by FaceLoader (see face_loader.cpp).
    {
        struct FaceXform { std::string folder, fit = "stretch"; double scale = 1.0; int ox = 0, oy = 0; };
        auto fx = std::make_shared<FaceXform>();
        auto fx_folder = [teensy]() -> std::string {
            const std::string p = teensy ? teensy->face_image_path("neutral") : std::string();
            if (p.empty()) return {};
            return std::filesystem::path(p).parent_path().string();
        };
        auto fx_load = [fx, fx_folder]{
            *fx = FaceXform{};
            fx->folder = fx_folder();
            if (fx->folder.empty()) return;
            std::ifstream f(fx->folder + "/config.json");
            if (!f) return;
            json j; try { f >> j; } catch (...) { return; }
            if (j.contains("fit") && j["fit"].is_string())          fx->fit   = j["fit"].get<std::string>();
            if (j.contains("scale") && j["scale"].is_number())      fx->scale = j["scale"].get<double>();
            if (j.contains("offset_x") && j["offset_x"].is_number())fx->ox    = (int)std::lround(j["offset_x"].get<double>());
            if (j.contains("offset_y") && j["offset_y"].is_number())fx->oy    = (int)std::lround(j["offset_y"].get<double>());
        };
        auto fx_save = [fx, teensy]{
            if (fx->folder.empty()) return;
            const std::string cp = fx->folder + "/config.json";
            json j = json::object();
            { std::ifstream f(cp); if (f) { try { f >> j; } catch (...) { j = json::object(); } } }
            if (!j.is_object()) j = json::object();
            j["fit"] = fx->fit; j["scale"] = fx->scale;
            j["offset_x"] = fx->ox; j["offset_y"] = fx->oy;
            const std::string tmp = cp + ".tmp";
            { std::ofstream f(tmp); if (f) f << j.dump(2); }
            std::error_code ec; std::filesystem::rename(tmp, cp, ec);
            if (teensy) teensy->reload_faces();      // live preview
        };
        std::vector<MenuItem> fit_items = {
            leaf_sel("Stretch (fill)", [fx, fx_save]{ fx->fit = "stretch"; fx_save(); },
                                       [fx]{ return fx->fit == "stretch"; }),
            leaf_sel("Contain (fit)",  [fx, fx_save]{ fx->fit = "contain"; fx_save(); },
                                       [fx]{ return fx->fit == "contain"; }),
            leaf_sel("Cover (crop)",   [fx, fx_save]{ fx->fit = "cover";   fx_save(); },
                                       [fx]{ return fx->fit == "cover"; }),
        };
        std::vector<MenuItem> xform_items = {
            with_desc(submenu("Fit Mode", std::move(fit_items)),
                "How the face fills the panel when its drawn size differs: Stretch "
                "fills both axes (may distort); Contain keeps aspect + letterboxes; "
                "Cover keeps aspect + crops to fill."),
            with_desc(slider("Scale", 0.25f, 3.0f, 0.05f, "x",
                [fx]{ return (float)fx->scale; },
                [fx, fx_save](float v){ fx->scale = v; fx_save(); }),
                "Extra uniform zoom on top of the fit. 1.0 = none."),
            with_desc(slider("Offset X", -128.f, 128.f, 1.f, "px",
                [fx]{ return (float)fx->ox; },
                [fx, fx_save](float v){ fx->ox = (int)std::lround(v); fx_save(); }),
                "Shift the face left/right (panel pixels)."),
            with_desc(slider("Offset Y", -128.f, 128.f, 1.f, "px",
                [fx]{ return (float)fx->oy; },
                [fx, fx_save](float v){ fx->oy = (int)std::lround(v); fx_save(); }),
                "Shift the face up/down (panel pixels)."),
            with_desc(leaf("Reset", [fx, fx_save]{
                fx->fit = "stretch"; fx->scale = 1.0; fx->ox = 0; fx->oy = 0; fx_save(); }),
                "Back to fill-the-panel with no zoom or shift."),
        };
        MenuItem xform_sub = with_desc(submenu("Size & Position", std::move(xform_items)),
            "Scale and shift the current face so art drawn for a different panel "
            "size fits this one. Applies live and is saved into the face folder, so "
            "it travels with the face. Affects every expression in the face.");
        xform_sub.action     = fx_load;   // on-enter: read current values from disk
        xform_sub.visible_fn = [fx_folder]{ return !fx_folder().empty(); };
        face_files_menu.push_back(std::move(xform_sub));
    }

    // Visibility predicates — Effects / Face Color / Material Color /
    // Animations / Save Face Config / Release Control are concepts from
    // the existing HUB75 + Protoface-daemon path; the native MAX7219 /
    // RGB-matrix renderer doesn't speak them. Hide on those backends so
    // the menu shows only what the active hardware actually responds to.
    auto visible_for_hub75 = [pf_backend_p] {
        // Default to "shown" if no backend pointer is plumbed — keeps
        // older callers (and the Protoface-daemon mode where pf_backend
        // isn't relevant) working.
        return !pf_backend_p || *pf_backend_p == "hub75";
    };
    auto gated = [](MenuItem m, std::function<bool()> vis) -> MenuItem {
        m.visible_fn = std::move(vis);
        return m;
    };

    // Panel preview controls — toggle + position + size + view — all wrap
    // into one "Panel Preview" submenu so the parent stays tidy when
    // visibility-gating filters cut the menu down to the essentials.
    std::vector<MenuItem> pf_preview_menu;
    if (panel_preview_pp)
        pf_preview_menu.push_back(toggle("Enabled",
            [panel_preview_pp]{ return *panel_preview_pp; },
            [panel_preview_pp](bool v){ *panel_preview_pp = v; }));
    if (protoface_preview_cfg) {
        pf_preview_menu.push_back(submenu("Position",
            make_position_items(protoface_preview_cfg)));
        pf_preview_menu.push_back(make_size_slider("Size", protoface_preview_cfg));
    }
    if (protoface_preview_view_pp) {
        int* vp = protoface_preview_view_pp;
        // The view picker (Whole Face / Left / Right) only makes sense on
        // HUB75: that canvas is a mirrored pair (two physical panels, each
        // holding one face), so the user can choose which half to preview.
        // MAX7219 / RGB-matrix canvases already get cropped to a single
        // centred face in pick_face_tex, so this picker is hidden there.
        MenuItem view_it = submenu("View", std::vector<MenuItem>{
            leaf_sel("Whole Face", [vp]{ *vp = 0; }, [vp]{ return *vp == 0; }),
            leaf_sel("Left Half",  [vp]{ *vp = 1; }, [vp]{ return *vp == 1; }),
            leaf_sel("Right Half", [vp]{ *vp = 2; }, [vp]{ return *vp == 2; }),
        });
        view_it.visible_fn = visible_for_hub75;
        pf_preview_menu.push_back(std::move(view_it));
    }

    std::vector<MenuItem> protoface_inner_menu = {
        gated(with_panel(submenu("Effects", std::move(pf_effects)),
                         "Effect Preview", draw_effect_preview), visible_for_hub75),
        gated(submenu("Material Color", std::move(pf_palette)),  visible_for_hub75),
        // Face PNGs (per-expression slots, mouth shapes, boop reactions)
        // live here under Protoface rather than the generic Files menu —
        // they're meaningful per-backend, and the editor only makes sense
        // when the active backend supports it (MAX7219 / RGB matrix).
        with_desc(with_panel(submenu("Face Options", std::move(face_files_menu)),
                             "Face Preview", draw_face_preview),
                  "Per-expression face PNGs, mouth/boop slots, the in-HUD "
                  "pixel editor, and the chain layout pickers that shape "
                  "its bounding boxes. Edit... opens whenever the active "
                  "backend (Hardware > Backend) has an editor capability "
                  "— today: MAX7219 and RGB matrix; HUB75 stays "
                  "import-only. Files are stored in "
                  "faces/<active>[_<backend>]/ so each panel technology "
                  "keeps its own art."),
        // Face Animations — blink timing + expression fade. Mutates the live
        // FaceState on every panel via the pf_anim_push callback wired by
        // main; persists to cfg["protoface"]["animation"].
        ([&]() -> MenuItem {
            std::vector<MenuItem> blink_items;
            if (pf_blink_enabled_p) {
                blink_items.push_back(toggle("Enable Blinking",
                    [pf_blink_enabled_p]{ return *pf_blink_enabled_p; },
                    [pf_blink_enabled_p, pf_anim_push](bool v){
                        *pf_blink_enabled_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_min_p) {
                blink_items.push_back(slider("Min Interval", 1.0f, 15.0f, 0.5f, "s",
                    [pf_blink_min_p]{ return static_cast<float>(*pf_blink_min_p); },
                    [pf_blink_min_p, pf_blink_max_p, pf_anim_push](float v){
                        *pf_blink_min_p = v;
                        if (pf_blink_max_p && *pf_blink_max_p < v) *pf_blink_max_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_max_p) {
                blink_items.push_back(slider("Max Interval", 1.0f, 30.0f, 0.5f, "s",
                    [pf_blink_max_p]{ return static_cast<float>(*pf_blink_max_p); },
                    [pf_blink_min_p, pf_blink_max_p, pf_anim_push](float v){
                        *pf_blink_max_p = v;
                        if (pf_blink_min_p && *pf_blink_min_p > v) *pf_blink_min_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_dur_p) {
                blink_items.push_back(slider("Duration", 0.05f, 0.5f, 0.01f, "s",
                    [pf_blink_dur_p]{ return static_cast<float>(*pf_blink_dur_p); },
                    [pf_blink_dur_p, pf_anim_push](float v){
                        *pf_blink_dur_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            std::vector<MenuItem> anim_items;
            if (!blink_items.empty()) {
                anim_items.push_back(with_desc(
                    submenu("Blink", std::move(blink_items)),
                    "Eyes-closed overlay timing. Min/Max set the random "
                    "interval between blinks; Duration is the full "
                    "close→open arc. Disable to hold eyes open."));
            }
            if (pf_expr_fade_p) {
                anim_items.push_back(slider("Expression Fade", 0.0f, 2.0f, 0.05f, "s",
                    [pf_expr_fade_p]{ return static_cast<float>(*pf_expr_fade_p); },
                    [pf_expr_fade_p, pf_anim_push](float v){
                        *pf_expr_fade_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_preview_duration_p) {
                anim_items.push_back(with_desc(
                    slider("Editor Preview Hold", 5.0f, 60.0f, 1.0f, "s",
                        [pf_preview_duration_p]{
                            return static_cast<float>(*pf_preview_duration_p);
                        },
                        [pf_preview_duration_p](float v){
                            *pf_preview_duration_p = v;
                        }),
                    "How long the V (Preview to panels) key in the face "
                    "editor shows the in-progress canvas on the physical "
                    "panels before auto-restoring the saved face."));
            }
            if (anim_items.empty()) {
                MenuItem empty;
                empty.visible_fn = []{ return false; };
                return empty;
            }
            return with_desc(submenu("Animations", std::move(anim_items)),
                "Idle-face animation tuning: blink cadence, blink duration, "
                "and the crossfade time between expressions. Applies to the "
                "native (MAX7219 / RGB matrix) renderer.");
        })(),
        // Glitch post-effect — one effect, every look an independent variable.
        ([&]() -> MenuItem {
            if (!pf_glitch_p) { MenuItem e; e.visible_fn = []{ return false; }; return e; }
            face::GlitchConfig* G = pf_glitch_p;
            std::vector<MenuItem> gi;
            gi.push_back(with_desc(toggle("Glitch",
                [G]{ return G->enabled; },
                [G, pf_anim_push](bool v){ G->enabled = v; if (pf_anim_push) pf_anim_push(); }),
                "Master enable for the glitch post-effect. Off is a true "
                "no-op — zero render cost. The component sliders below "
                "keep their values while disabled."));
            {   // Curated looks — applying one overwrites every slider below.
                std::vector<MenuItem> pi;
                for (const auto& [pname, pcfg] : face::GlitchConfig::presets()) {
                    const face::GlitchConfig pc = pcfg;
                    pi.push_back(leaf(pname, [G, pc, pf_anim_push]{
                        *G = pc;
                        if (pf_anim_push) pf_anim_push();
                    }));
                }
                gi.push_back(with_desc(submenu("Presets", std::move(pi)),
                    "Curated glitch looks (vhs, datamosh, signal_loss, haunted, "
                    "subtle, meltdown). Selecting one enables the glitch and sets "
                    "every slider; tweak from there."));
            }
            gi.push_back(with_desc(slider("Intensity", 0.f, 200.f, 5.f, "%",
                [G]{ return static_cast<float>(G->intensity * 100.0); },
                [G, pf_anim_push](float v){ G->intensity = v / 100.0; if (pf_anim_push) pf_anim_push(); }),
                "Master strength multiplier applied on top of every "
                "component amount. 100% = the component sliders as set; "
                "200% doubles everything for a total-meltdown look."));
            gi.push_back(with_desc(slider("Burst Rate", 0.f, 3.f, 0.1f, "/s",
                [G]{ return static_cast<float>(G->burst_rate); },
                [G, pf_anim_push](float v){ G->burst_rate = v; if (pf_anim_push) pf_anim_push(); }),
                "How many corruption bursts fire per second. Between "
                "bursts the face is clean, so the glitch stutters in like "
                "a failing signal. 0 = no bursts: corruption runs "
                "constantly at full Intensity."));
            gi.push_back(with_desc(slider("Burst Min", 0.02f, 1.f, 0.02f, "s",
                [G]{ return static_cast<float>(G->burst_min); },
                [G, pf_anim_push](float v){ G->burst_min = v; if (pf_anim_push) pf_anim_push(); }),
                "Shortest duration of a single burst. Each burst lasts a "
                "random time between Burst Min and Burst Max."));
            gi.push_back(with_desc(slider("Burst Max", 0.05f, 2.f, 0.05f, "s",
                [G]{ return static_cast<float>(G->burst_max); },
                [G, pf_anim_push](float v){ G->burst_max = v; if (pf_anim_push) pf_anim_push(); }),
                "Longest duration of a single burst. Each burst lasts a "
                "random time between Burst Min and Burst Max."));
            // Per-component amounts (0 = off). Each is an independent variable.
            auto comp = [&](const char* name, double face::GlitchConfig::* member,
                            const char* desc) {
                gi.push_back(with_desc(slider(name, 0.f, 100.f, 5.f, "%",
                    [G, member]{ return static_cast<float>((G->*member) * 100.0); },
                    [G, member, pf_anim_push](float v){ G->*member = v / 100.0; if (pf_anim_push) pf_anim_push(); }),
                    desc));
            };
            comp("Chromatic Split",    &face::GlitchConfig::chromatic,
                 "Splits the red/green/blue channels apart so the face gets "
                 "colored fringes, like chromatic aberration or a badly "
                 "converged CRT. Higher = wider separation.");
            comp("Band Tearing",       &face::GlitchConfig::tearing,
                 "Displaces random horizontal bands sideways — the classic "
                 "VHS tracking / rolling-tear look. Higher = more bands "
                 "shifted further.");
            comp("Block Shuffle",      &face::GlitchConfig::blocks,
                 "Swaps rectangular chunks of the face with each other, like "
                 "corrupted video macroblocks. Higher = more and bigger "
                 "blocks out of place.");
            comp("Bitcrush",           &face::GlitchConfig::bitcrush,
                 "Posterizes the colors down to fewer levels, giving banded, "
                 "crunchy shading instead of smooth gradients. Higher = "
                 "fewer color steps.");
            comp("Dropout Bars",       &face::GlitchConfig::dropout,
                 "Replaces random horizontal bars with black or static — "
                 "signal-loss stripes. Higher = more frequent and taller "
                 "bars.");
            comp("Datamosh",           &face::GlitchConfig::datamosh,
                 "Smears ghosts of the previous frame into the current one, "
                 "so motion leaves melting trails (compression-artifact "
                 "style). Most visible while the face is moving/blinking.");
            comp("Eyes/Mouth Desync",  &face::GlitchConfig::region_desync,
                 "Slips the top (eyes) and bottom (mouth) halves of the face "
                 "sideways independently, as if the two regions lost sync "
                 "with each other.");
            comp("Expression Flicker", &face::GlitchConfig::expr_flicker,
                 "Occasionally flashes a DIFFERENT expression for a single "
                 "frame — the face \"corrupts\" into another mood and "
                 "snaps back. Higher = more frequent flashes.");
            return with_desc(submenu("Glitch", std::move(gi)),
                "Digital glitch corruption of the face. Master Intensity and Burst "
                "Rate gate the look (Burst Rate 0 = constant); each component below "
                "is an independent amount.");
        })(),
        // Scrolling text banner — a marquee across every panel, drawn above the
        // face / effects / glitch so it stays legible.
        ([&]() -> MenuItem {
            if (!pf_scroll_p) { MenuItem e; e.visible_fn = []{ return false; }; return e; }
            face::ScrollTextConfig* S = pf_scroll_p;
            std::vector<MenuItem> si;
            si.push_back(with_desc(toggle("Show Text",
                [S]{ return S->enabled; },
                [S, pf_anim_push](bool v){ S->enabled = v; if (pf_anim_push) pf_anim_push(); }),
                "Master enable for the scrolling banner. Off is a true no-op. "
                "The text, speed and color keep their values while disabled."));
            {
                MenuItem t = leaf("Edit Text...", [S, menu_sys_pp, pf_anim_push]{
                    if (!menu_sys_pp || !*menu_sys_pp) return;
                    (*menu_sys_pp)->open_keyboard("Scroll Text", S->text,
                        [S, pf_anim_push](const std::string& v){
                            S->text = v;
                            if (!v.empty()) S->enabled = true;
                            if (pf_anim_push) pf_anim_push();
                        });
                });
                t.label_fn = [S]{
                    return S->text.empty() ? std::string("Edit Text...")
                                           : "Text: " + S->text;
                };
                si.push_back(with_desc(std::move(t),
                    "The message to scroll (on-screen keyboard). Committing a "
                    "non-empty text switches the banner on; glyphs the 5x7 "
                    "font lacks render blank."));
            }
            si.push_back(with_desc(slider("Speed", 4.f, 120.f, 4.f, "px/s",
                [S]{ return static_cast<float>(S->speed_px_s); },
                [S, pf_anim_push](float v){ S->speed_px_s = v; if (pf_anim_push) pf_anim_push(); }),
                "Scroll speed in canvas pixels per second (right to left)."));
            {
                std::vector<MenuItem> zi;
                for (int k = 1; k <= 4; ++k)
                    zi.push_back(leaf_sel(std::to_string(k) + "x",
                        [S, k, pf_anim_push]{ S->scale = k; if (pf_anim_push) pf_anim_push(); },
                        [S, k]{ return S->scale == k; }));
                si.push_back(with_desc(submenu("Size", std::move(zi)),
                    "Integer upscale of the 5x7 font. 2x (10x14) reads well "
                    "across a 32px-tall face; 4x fills most of the panel "
                    "height."));
            }
            {
                struct C { const char* n; uint8_t r, g, b; };
                static constexpr C kCols[] = {
                    {"White", 255,255,255}, {"Amber", 255,160,32},
                    {"Red",   255, 50, 50}, {"Green",  30,220,60},
                    {"Cyan",    0,180,255}, {"Purple",180, 30,220},
                };
                std::vector<MenuItem> ci;
                for (const auto& c : kCols)
                    ci.push_back(leaf_sel(c.n,
                        [S, c, pf_anim_push]{ S->r = c.r; S->g = c.g; S->b = c.b;
                                              if (pf_anim_push) pf_anim_push(); },
                        [S, c]{ return S->r == c.r && S->g == c.g && S->b == c.b; }));
                si.push_back(submenu("Color", std::move(ci)));
            }
            si.push_back(with_desc(toggle("Loop",
                [S]{ return S->loop; },
                [S, pf_anim_push](bool v){ S->loop = v; if (pf_anim_push) pf_anim_push(); }),
                "On: the message wraps around forever. Off: one pass across "
                "the panels, then the banner switches itself off."));
            return with_desc(submenu("Scrolling Text", std::move(si)),
                "Scroll a text banner across the face panels (5x7 pixel font, "
                "tinted, above every layer). Config: cfg[\"protoface\"]"
                "[\"scroll_text\"].");
        })(),
        // Environment / movement reactions (sleepy, wake, ...). The engine
        // lives in main; this page edits its config and fires test runs.
        ([&]() -> MenuItem {
            face::ReactionEngine* R = ctx.reactions;
            if (!R) { MenuItem e; e.visible_fn = []{ return false; }; return e; }
            nlohmann::json* cfgr = cfg_root;
            auto save = [R, cfgr]{
                if (cfgr) (*cfgr)["reactions"] = R->config().to_json();
            };
            std::vector<MenuItem> ri;
            {
                MenuItem st = leaf("Status", []{});
                st.label_fn = [R]{
                    char b[64];
                    snprintf(b, sizeof b, "State: %s  (energy %.0f)",
                             R->activity_name(), R->energy_dps());
                    return std::string(b);
                };
                ri.push_back(with_desc(std::move(st),
                    "Live activity state from the IMU: awake / drowsy / "
                    "asleep, with the smoothed motion energy the timers "
                    "watch (deg/s-equivalent)."));
            }
            ri.push_back(with_desc(toggle("Reactions",
                [R]{ return R->config().enabled; },
                [R, save](bool v){ auto c = R->config(); c.enabled = v;
                                   R->set_config(c); save(); }),
                "Master enable for environment/movement reactions."));
            ri.push_back(with_desc(toggle("Sleepy / Wake",
                [R]{ return R->config().sleepy_enabled; },
                [R, save](bool v){ auto c = R->config(); c.sleepy_enabled = v;
                                   R->set_config(c); save(); }),
                "Hold still and the face gets heavy-lidded (slow blinks + "
                "the 'sleepy' face if the folder has one), then falls asleep "
                "- eyes closed ('asleep' face) with floating Z's. Any sharp "
                "head motion snaps it awake with a surprised flash and "
                "restores everything."));
            ri.push_back(with_desc(slider("Drowsy After", 30.f, 600.f, 15.f, "s",
                [R]{ return static_cast<float>(R->config().drowsy_after_s); },
                [R, save](float v){ auto c = R->config(); c.drowsy_after_s = v;
                                    R->set_config(c); save(); }),
                "Seconds of stillness before the heavy-lidded stage."));
            ri.push_back(with_desc(slider("Sleep After", 60.f, 1800.f, 30.f, "s",
                [R]{ return static_cast<float>(R->config().sleep_after_s); },
                [R, save](float v){ auto c = R->config(); c.sleep_after_s = v;
                                    R->set_config(c); save(); }),
                "Seconds of stillness before falling asleep (measured from "
                "the start of stillness, not from drowsy)."));
            ri.push_back(with_desc(slider("Wake Threshold", 15.f, 120.f, 5.f, "\xc2\xb0/s",
                [R]{ return static_cast<float>(R->config().wake_dps); },
                [R, save](float v){ auto c = R->config(); c.wake_dps = v;
                                    R->set_config(c); save(); }),
                "How sharp a head motion wakes the face. Lower = lighter "
                "sleeper."));
            ri.push_back(with_desc(leaf("Test: Fall Asleep", [R]{ R->force_sleepy(); }),
                "Preview the sleep look right now (runs drowsy then asleep). "
                "Move your head - or use Test: Wake - to end it."));
            ri.push_back(leaf("Test: Wake", [R]{ R->force_wake(); }));
            return with_desc(submenu("Reactions", std::move(ri)),
                "The face reacts to the real world: stillness makes it "
                "drowsy then asleep (floating Z's), sharp motion wakes it. "
                "The dark-to-bright squint lives under its light-sensor "
                "config; more reactions land here as they're added.");
        })(),
        gated(with_panel(submenu("GIFs", std::move(pf_gifs)),
                         "GIF Preview", draw_gif_preview), visible_for_hub75),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy, &state](float v){
                // Write the shared state too — the slider's getter reads it, so
                // without this every step snapped back to the stale value (and
                // the setting was lost on restart).
                teensy->set_brightness(static_cast<uint8_t>(v));
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.brightness = static_cast<uint8_t>(v);
            }),
        submenu("Hardware",       std::move(pf_hardware_menu)),
        gated(leaf("Save Face Config", [teensy]{ teensy->save_config(); }),
              visible_for_hub75),
        gated(leaf("Release Control",  [teensy]{ teensy->release_control(); }),
              visible_for_hub75),
    };
    // "Start Protoface" first: launch the daemon (if not running) and make it the
    // active source. Targets the Protoface backend directly (not the proxy).
    if (fp_option) {
        protoface_inner_menu.insert(protoface_inner_menu.begin(),
            gated(leaf("Start Protoface", [fp_option, active_face_pp]{
                fp_option->launch();
                if (active_face_pp) *active_face_pp = fp_option;
            }), visible_for_hub75));
        protoface_inner_menu.insert(protoface_inner_menu.begin() + 1,
            gated(leaf("Restart Protoface", [fp_option, active_face_pp]{
                fp_option->restart();
                if (active_face_pp) *active_face_pp = fp_option;
            }), visible_for_hub75));
    }
    if (!pf_preview_menu.empty())
        protoface_inner_menu.push_back(submenu("Panel Preview",
                                               std::move(pf_preview_menu)));

    // ── Face Display root: Source picker (radios) + per-backend submenus ─────
    // Protoface first (the primary renderer), then the source radios and the
    // ProtoTracer submenu.
    std::vector<MenuItem> face_display_menu;
    if (!protoface_inner_menu.empty())
        face_display_menu.push_back(submenu("Protoface", std::move(protoface_inner_menu)));
    if (active_face_pp && teensy_option && fp_option) {
        face_display_menu.push_back(leaf_sel("Source: Teensy (ProtoTracer)",
            [active_face_pp, teensy_option]{ *active_face_pp = teensy_option; },
            [active_face_pp, teensy_option]{ return *active_face_pp == teensy_option; }));
        face_display_menu.push_back(leaf_sel("Source: Protoface",
            [active_face_pp, fp_option]{ *active_face_pp = fp_option; },
            [active_face_pp, fp_option]{ return *active_face_pp == fp_option; }));
    }
    face_display_menu.push_back(submenu("ProtoTracer", std::move(prototracer_inner_menu)));

    // ── Boop sensor (Protoface-side reactive behaviour) ──────────────────────
    // One submenu per zone, each with Enabled / Expression / Hold Duration /
    // Touch Threshold (lower = more sensitive) / Test. Tunable changes mirror
    // into the sensor immediately so the next poll cycle uses them; the
    // values persist to config.json via mutate_cfg on shutdown.
    auto boop_zone_menu = [&, teensy, boop_sensor_pp](int idx, std::string label) -> MenuItem {
        const auto zone_enum = static_cast<sensor::BoopSensor::Zone>(idx);

        std::vector<MenuItem> expr_items;
        for (int ei = 0; ei < kFaceSlotCount; ++ei) {
            const std::string expr     = kFaceSlots[ei].expression;
            const std::string ex_label = kFaceSlots[ei].label;
            expr_items.push_back(leaf_sel(ex_label,
                [&state, idx, expr]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.boop_zones[idx].expression = expr;
                },
                [&state, idx, expr]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return state.boop_zones[idx].expression == expr;
                }));
        }

        std::vector<MenuItem> items = {
            toggle("Enabled",
                [&state, idx]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return state.boop_zones[idx].enabled;
                },
                [&state, idx, boop_sensor_pp, zone_enum](bool v){
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.boop_zones[idx].enabled = v;
                    }
                    if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                        s->set_zone_enabled(zone_enum, v);
                }),
            with_desc(submenu("Expression", std::move(expr_items)),
                      "Which face the boop triggers. Auto-reverts when the "
                      "hold duration elapses."),
            slider("Hold Duration", 0.2f, 3.0f, 0.1f, " s",
                [&state, idx]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return static_cast<float>(state.boop_zones[idx].duration_s);
                },
                [&state, idx](float v){
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.boop_zones[idx].duration_s = static_cast<double>(v);
                }),
            // Touch threshold is meaningless for the BothCheeks zone — it's
            // derived from the left/right cheek events via the coalescer.
            // Hide the slider there so the menu stays tidy.
            [&]{
                MenuItem m = with_desc(slider("Touch Threshold", 4.f, 30.f, 1.f, "",
                    [&state, idx]{
                        std::lock_guard<std::mutex> lk(state.mtx);
                        return static_cast<float>(state.boop_zones[idx].threshold);
                    },
                    [&state, idx, boop_sensor_pp, zone_enum](float v){
                        const auto t = static_cast<uint8_t>(v);
                        {
                            std::lock_guard<std::mutex> lk(state.mtx);
                            state.boop_zones[idx].threshold = t;
                        }
                        if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                            s->set_zone_threshold(zone_enum, t);
                    }),
                    "MPR121 touch threshold. Lower numbers are more sensitive; "
                    "raise if you see false triggers.");
                m.visible_fn = [idx]{ return idx != static_cast<int>(sensor::BoopSensor::Zone::BothCheeks); };
                return m;
            }(),
            // MPR121 electrode → zone mapping (the I²C cap-touch input). Applies
            // on the next launch — the sensor reads it at start(). Hidden for
            // BothCheeks (derived from the two cheek electrodes).
            [&]{
                MenuItem m = with_desc(slider("Electrode (MPR121)", -1.f, 11.f, 1.f, "",
                    [&state, idx]{
                        std::lock_guard<std::mutex> lk(state.mtx);
                        return static_cast<float>(state.boop_zones[idx].electrode);
                    },
                    [&state, idx](float v){
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.boop_zones[idx].electrode = static_cast<int>(v);
                    }),
                    "Which MPR121 capacitive electrode (0-11) drives this zone; "
                    "-1 disables it. Takes effect on the next launch.");
                m.visible_fn = [idx]{ return idx != static_cast<int>(sensor::BoopSensor::Zone::BothCheeks); };
                return m;
            }(),
            leaf("Test Boop",
                [teensy, &state, idx]{
                    std::string expr;
                    double dur = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        expr = state.boop_zones[idx].expression;
                        dur  = state.boop_zones[idx].duration_s;
                    }
                    if (!expr.empty()) teensy->trigger_boop(expr, dur);
                }),
        };

        // ── Animated Eyes — rapid-boop easter egg ────────────────────────────
        // After Trigger Count fast boops on this zone, a procedural eye
        // animation plays instead of the normal reaction. The built-in
        // animations are re-skinnable (rate / scale / colour / hold time).
        {
            auto& bz = state;   // capture helper alias
            std::vector<MenuItem> anim_items;
            for (int a = 0; a < face::eye_anim_count(); ++a)
                anim_items.push_back(leaf_sel(face::eye_anim_name(static_cast<face::EyeAnim>(a)),
                    [&bz, idx, a]{
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.anim = a;
                    },
                    [&bz, idx, a]{
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        return bz.boop_zones[idx].eye_trigger.anim == a;
                    }));

            std::vector<MenuItem> eye_items = {
                toggle("Enabled",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return bz.boop_zones[idx].eye_trigger.enabled; },
                    [&bz, idx](bool v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.enabled = v; }),
                submenu("Animation", std::move(anim_items)),
                with_desc(slider("Trigger Count", 2.f, 10.f, 1.f, " boops",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.count); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.count = static_cast<int>(v); }),
                    "How many fast boops on this zone trigger the eyes."),
                with_desc(slider("Window", 1.f, 8.f, 0.5f, " s",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.window_s); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.window_s = static_cast<double>(v); }),
                    "Consecutive boops must land within this window or the "
                    "counter resets."),
                slider("Speed", 0.2f, 4.f, 0.1f, "x",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.speed); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.speed = static_cast<double>(v); }),
                slider("Size", 0.4f, 2.5f, 0.1f, "x",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.size); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.size = static_cast<double>(v); }),
                slider("Duration", 1.f, 8.f, 0.5f, " s",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.duration_s); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.duration_s = static_cast<double>(v); }),
                color_picker("Color",
                    [&bz, idx](uint8_t r, uint8_t g, uint8_t b){
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.r = r;
                        bz.boop_zones[idx].eye_trigger.g = g;
                        bz.boop_zones[idx].eye_trigger.b = b; },
                    [&bz, idx]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        const auto& e = bz.boop_zones[idx].eye_trigger;
                        return { e.r, e.g, e.b }; }),
                leaf("Test Eyes",
                    [teensy, &bz, idx]{
                        EyeTriggerConfig e;
                        { std::lock_guard<std::mutex> lk(bz.mtx);
                          e = bz.boop_zones[idx].eye_trigger; }
                        teensy->play_eye_animation(e.anim, e.speed, e.size,
                                                   e.r, e.g, e.b, e.duration_s);
                    }),
            };
            items.push_back(with_desc(submenu("Animated Eyes", std::move(eye_items)),
                "Boop this zone Trigger-Count times within Window seconds and a "
                "procedural eye animation plays instead of the normal reaction. "
                "Tune the built-in animations with Speed / Size / Color."));
        }

        return submenu(std::move(label), std::move(items));
    };

    std::vector<MenuItem> boop_menu = {
        boop_zone_menu(0, "Snout"),
        boop_zone_menu(1, "Left Cheek"),
        boop_zone_menu(2, "Right Cheek"),
        boop_zone_menu(3, "Both Cheeks"),
        with_desc(slider("Coalesce Window", 0.f, 0.30f, 0.01f, " s",
            [&state]{ return state.boop_coalesce_window_s; },
            [&state, boop_sensor_pp](float v){
                state.boop_coalesce_window_s = v;
                if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                    s->set_coalesce_window_s(static_cast<double>(v));
            }),
            "When left and right cheeks both land touch events within this "
            "window, both single-cheek events are suppressed and a Both "
            "Cheeks event fires instead. Set to 0 to disable coalescing "
            "(single-side events fire immediately)."),
    };
    face_display_menu.push_back(
        with_desc(submenu("Boop", std::move(boop_menu)),
                  "Per-zone capacitive-touch reactions. Drives "
                  "Protoface's trigger_boop() — fires the chosen expression "
                  "when the snout, a cheek pad, or both cheeks together are "
                  "touched."));

    // ── Light Sensor (BH1750) → squint reaction ────────────────────────────
    // Edge-detects dark→bright transitions (helmet stepping into sunlight)
    // and fires the configured expression for the chosen duration. The
    // expression name lines up with a Files > Faces slot so the user can
    // author it in the editor.
    std::vector<MenuItem> light_menu = {
        toggle("Enabled",
            [&state]{ return state.light_squint.enabled; },
            [&state](bool v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.enabled = v;
            }),
        slider("Dark Threshold", 1.f, 1000.f, 5.f, " lx",
            [&state]{ return state.light_squint.dark_threshold_lux; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.dark_threshold_lux = v;
                if (state.light_squint.bright_threshold_lux <= v)
                    state.light_squint.bright_threshold_lux = v + 10.f;
            }),
        slider("Bright Threshold", 50.f, 20000.f, 50.f, " lx",
            [&state]{ return state.light_squint.bright_threshold_lux; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.bright_threshold_lux = v;
                if (state.light_squint.dark_threshold_lux >= v)
                    state.light_squint.dark_threshold_lux = std::max(1.f, v - 10.f);
            }),
        slider("Transition Window", 0.2f, 10.f, 0.1f, " s",
            [&state]{ return state.light_squint.transition_window_s; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.transition_window_s = v;
            }),
        slider("Hold Duration", 0.2f, 5.f, 0.1f, " s",
            [&state]{ return static_cast<float>(state.light_squint.duration_s); },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.duration_s = v;
            }),
        slider("Cooldown", 0.5f, 30.f, 0.5f, " s",
            [&state]{ return state.light_squint.cooldown_s; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.cooldown_s = v;
            }),
    };
    face_display_menu.push_back(
        with_desc(submenu("Light Sensor", std::move(light_menu)),
                  "Triggers a face reaction when the wearer steps from a dim "
                  "area into a bright one. Reads a BH1750 over I²C "
                  "(/dev/i2c-1, addr 0x23). The expression name (default "
                  "\"squint\") maps to a Files > Faces slot — author the PNG "
                  "there. Cooldown gates back-to-back triggers under "
                  "flickering light."));

    // ── Voice → mouth_open driver ────────────────────────────────────────────
    // Sliders write through to the live analyzer so the next FFT cycle picks
    // up new values; state.voice_mouth mirrors them so mutate_cfg can persist
    // to config.json on exit. Disabled by default — flip Enabled to start
    // analysing (or set "voice_mouth.enabled":true in config.json).
    auto voice_apply_band = [voice_analyzer, &state]() {
        if (voice_analyzer)
            voice_analyzer->set_band(state.voice_mouth.band_lo_hz,
                                     state.voice_mouth.band_hi_hz);
    };
    std::vector<MenuItem> voice_menu = {
        toggle("Enabled",
            [&state]{ return state.voice_mouth.enabled; },
            [&state, voice_analyzer](bool v){
                state.voice_mouth.enabled = v;
                if (voice_analyzer) voice_analyzer->set_enabled(v);
            }),
        with_desc(slider("Sensitivity", 0.25f, 6.f, 0.05f, "x",
            [&state]{ return state.voice_mouth.sensitivity; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.sensitivity = v;
                if (voice_analyzer) voice_analyzer->set_sensitivity(v);
            }),
            "Multiplies the speech-band RMS before the gate / clip to 1.0. "
            "Raise if the mouth barely opens during normal speech."),
        with_desc(slider("Noise Gate", 0.f, 0.2f, 0.005f, "",
            [&state]{ return state.voice_mouth.noise_gate; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.noise_gate = v;
                if (voice_analyzer) voice_analyzer->set_noise_gate(v);
            }),
            "Band RMS below this floor reads as silence. Raise to ignore "
            "fan / motor / background noise."),
        with_desc(slider("Attack", 5.f, 150.f, 5.f, " ms",
            [&state]{ return state.voice_mouth.attack_ms; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.attack_ms = v;
                if (voice_analyzer) voice_analyzer->set_attack_ms(v);
            }),
            "Time constant for opening the mouth. Lower feels snappier; "
            "higher smooths over transients."),
        with_desc(slider("Release", 30.f, 600.f, 10.f, " ms",
            [&state]{ return state.voice_mouth.release_ms; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.release_ms = v;
                if (voice_analyzer) voice_analyzer->set_release_ms(v);
            }),
            "Time constant for closing. Higher = mouth lingers open between "
            "syllables (less stuttery)."),
        with_desc(slider("Band Low", 40.f, 1200.f, 10.f, " Hz",
            [&state]{ return state.voice_mouth.band_lo_hz; },
            [&state, voice_apply_band](float v){
                state.voice_mouth.band_lo_hz = v;
                voice_apply_band();
            }),
            "Lower edge of the analysis band. Raise to cut low-frequency "
            "rumble; lower if your voice's fundamentals are getting clipped."),
        with_desc(slider("Band High", 1000.f, 8000.f, 100.f, " Hz",
            [&state]{ return state.voice_mouth.band_hi_hz; },
            [&state, voice_apply_band](float v){
                state.voice_mouth.band_hi_hz = v;
                voice_apply_band();
            }),
            "Upper edge. Lower if hiss / sibilants are over-driving the mouth."),

        // ── Visemes (multi-shape mouth) ─────────────────────────────────
        with_desc(toggle("Visemes Enabled",
            [&state]{ return state.voice_mouth.visemes_enabled; },
            [&state, voice_analyzer](bool v){
                state.voice_mouth.visemes_enabled = v;
                if (voice_analyzer) voice_analyzer->set_visemes_enabled(v);
            }),
            "Switch the mouth overlay between mouth_open/_small/_smile/_round "
            "based on the analyzer's spectral centroid. Falls back to "
            "mouth_open when off (matches the pre-viseme behaviour)."),
    };

    // Shared updater for the three viseme thresholds.
    auto voice_apply_visemes = [voice_analyzer, &state]() {
        if (voice_analyzer)
            voice_analyzer->set_viseme_thresholds(
                state.voice_mouth.viseme_round_max_hz,
                state.voice_mouth.viseme_open_max_hz,
                state.voice_mouth.viseme_small_max_hz);
    };
    voice_menu.push_back(with_desc(slider("Round → Open", 100.f, 1500.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_round_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_round_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is the OOH (round) viseme; above, AH (open)."));
    voice_menu.push_back(with_desc(slider("Open → Small", 800.f, 2500.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_open_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_open_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is AH; above, the M/N small-open shape."));
    voice_menu.push_back(with_desc(slider("Small → Smile", 1500.f, 4000.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_small_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_small_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is the small-open shape; above, EE (smile)."));
    face_display_menu.push_back(
        with_desc(submenu("Voice", std::move(voice_menu)),
                  "Mic-driven mouth_open. FFT-based: speech-band RMS feeds an "
                  "envelope follower whose output drives face::set_audio() each "
                  "audio period (~5 ms)."));

    // ── Accessory LEDs (cheekhubs + fins) ────────────────────────────────────
    // Per-zone pattern / color / breathe rate live in the AccessoryLeds
    // manager's atomic-snapshotted config; the menu's setters write through
    // so the next render tick uses the new values. Brightness is global to
    // the whole chain. Pattern picker exposes Off / Solid / Breathe / Level
    // — Flash is reserved for event-driven overlays (boop hooks) only.
    auto led_zone_menu = [&, leds](accessory::Zone z, std::string label) -> MenuItem {
        // Pattern picker — radio-style leaf_sel set, one per pattern.
        struct PatOpt { const char* label; accessory::Pattern pat; };
        const PatOpt opts[] = {
            { "Off",     accessory::Pattern::Off     },
            { "Solid",   accessory::Pattern::Solid   },
            { "Breathe", accessory::Pattern::Breathe },
            { "Level",   accessory::Pattern::Level   },
        };
        std::vector<MenuItem> pat_items;
        for (const auto& o : opts) {
            const accessory::Pattern p = o.pat;
            pat_items.push_back(leaf_sel(o.label,
                [leds, z, p]{ if (leds) leds->set_zone_pattern(z, p); },
                [leds, z, p]{
                    return leds && leds->zone(z).pattern == p;
                }));
        }

        std::vector<MenuItem> items = {
            with_desc(submenu("Pattern", std::move(pat_items)),
                      "What the zone does each tick. Level uses the mic "
                      "volume; Breathe pulses at its own rate; Solid is a "
                      "steady colour. Boop events flash on top regardless."),
            color_picker("Color",
                [leds, z](uint8_t r, uint8_t g, uint8_t b) {
                    if (leds) leds->set_zone_color(z, r, g, b);
                },
                [leds, z]() -> std::tuple<uint8_t, uint8_t, uint8_t> {
                    if (!leds) return {0, 0, 0};
                    auto zc = leds->zone(z);
                    return {zc.r, zc.g, zc.b};
                }),
            with_desc(slider("Breathe Rate", 0.05f, 5.f, 0.05f, " Hz",
                [leds, z]{ return leds ? leds->zone(z).breathe_hz : 0.5f; },
                [leds, z](float v){ if (leds) leds->set_zone_breathe_hz(z, v); }),
                "How fast the Breathe pattern oscillates. Only meaningful "
                "when Pattern is Breathe."),
            leaf("Test Flash", [leds, z]{ if (leds) leds->trigger_flash(z, 0.35); }),
        };
        return submenu(std::move(label), std::move(items));
    };

    std::vector<MenuItem> led_menu = {
        with_desc(slider("Brightness", 0.f, 255.f, 1.f, "",
            [leds]{ return leds ? static_cast<float>(leds->global_brightness()) : 0.f; },
            [leds](float v){ if (leds) leds->set_global_brightness(static_cast<uint8_t>(v)); }),
            "Master brightness applied to the whole accessory chain at SPI "
            "encode time. WS2812s draw a lot of current at full white — "
            "keep this low (~64) unless you have power injection."),
        led_zone_menu(accessory::Zone::LeftCheekhub,  "Left Cheekhub"),
        led_zone_menu(accessory::Zone::RightCheekhub, "Right Cheekhub"),
        led_zone_menu(accessory::Zone::LeftFin,       "Left Fin"),
        led_zone_menu(accessory::Zone::RightFin,      "Right Fin"),
    };
    face_display_menu.push_back(
        with_desc(submenu("LEDs", std::move(led_menu)),
                  "Accessory WS2812 strip (cheekhubs + fins). Driven via "
                  "SPI MOSI; boops flash the matching zone, mic volume "
                  "drives Level zones."));

    return face_display_menu;
}
