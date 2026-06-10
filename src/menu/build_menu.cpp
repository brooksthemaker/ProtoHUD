// ── build_menu.cpp ────────────────────────────────────────────────────────────
// The deep-menu builder, moved verbatim out of main.cpp. build_menu() fills
// the six top-level tabs (Vision / HUD / Face Display / Files /
// Communications / System) plus the curated quick (corner/radial) tree
// written through ctx.quick_out. The body is unchanged from its main.cpp
// days — the alias block at the top of build_menu() re-creates the original
// parameter names from the MenuBuildContext.
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
#include "face/panel_output.h"
#include "face/shm_pusher_output.h"
#include "face/max7219_panel_output.h"
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
    // Liquid viscosity for "water" (0 = thin/snappy, 1 = thick/sluggish).
    float viscosity = 0.3f;
    // "water" extras: pitch shifts the fill level (look down → liquid rises);
    // bubbles count + style ("rise" bubbles in liquid / "drip" droplets above).
    float pitch_fill = 0.0f;
    int   bubbles = 0;
    std::string bubble_mode = "rise";
    // "lightning" extras: arc mode (crackling arcs vs falling bolts) + fork density.
    bool  arc = false;
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

// ── I2C bus scanner ───────────────────────────────────────────────────────────
// Runs in a background thread. Opens the bus, probes addresses 0x08–0x77, stores
// found addresses in state.i2c_scan_results, then clears i2c_scan_busy.
static void run_i2c_scan(AppState* sp) {
    std::string bus;
    { std::lock_guard<std::mutex> lk(sp->mtx); bus = sp->i2c_scan_bus; }

    std::vector<uint8_t> found;
    int fd = open(bus.c_str(), O_RDWR);
    if (fd >= 0) {
        for (int addr = 0x08; addr <= 0x77; ++addr) {
            if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;
            uint8_t buf = 0;
            if (read(fd, &buf, 1) >= 0 || (errno != ENODEV && errno != ENXIO))
                found.push_back(static_cast<uint8_t>(addr));
        }
        close(fd);
    }

    std::lock_guard<std::mutex> lk(sp->mtx);
    sp->i2c_scan_results = std::move(found);
    sp->i2c_scan_busy    = false;
}

// Open the GIF import picker for the given slot. On commit, copies the chosen
// file into gifs_dir, binds the manifest slot via the face controller, and
// plays it so the face reflects the import immediately. Shared by the inline
// Animations leaves (when an empty slot is selected) and the Files > GIFs
// management rows (Import.../Replace... actions).
static void import_gif_into_slot(MenuSystem* menu,
                                 IFaceController* teensy,
                                 std::string gifs_dir,
                                 uint8_t slot) {
    if (!menu || !teensy) return;
    char title[48];
    std::snprintf(title, sizeof(title),
                  "Import GIF -> slot %u", static_cast<unsigned>(slot));
    std::string start = menu->file_picker_dir();
    menu->open_file_picker(
        title, std::move(start), {".gif"},
        [teensy, slot, gifs_dir = std::move(gifs_dir)](const std::string& src) {
            std::error_code ec;
            std::filesystem::create_directories(gifs_dir, ec);
            const std::string fname =
                std::filesystem::path(src).filename().string();
            const std::string dst = gifs_dir + "/" + fname;
            std::filesystem::copy_file(
                src, dst,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                std::fprintf(stderr, "[gif] import copy failed %s -> %s: %s\n",
                             src.c_str(), dst.c_str(), ec.message().c_str());
                return;
            }
            teensy->bind_gif_slot(slot, fname);
            teensy->play_gif(slot);
        });
}

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

std::vector<MenuItem> build_menu(MenuBuildContext& ctx)
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
    std::function<void(bool)> pf_set_expr_effects = ctx.pf_set_expr_effects;
    bool* pf_expr_effects_p = ctx.pf_expr_effects_p;
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

    (void)lora; (void)knob;


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

    // ── GIFs ─────────────────────────────────────────────────────────────────
    // Animated preview shared by both "Animations" submenus (ProtoTracer +
    // Protoface). The highlighted slot's GIF is decoded on the render thread and
    // uploaded to a GL texture each frame. scan_folder() matches the face
    // controller's order, so the slot index equals the play_gif() index.
    struct GifPreview {
        // 256×128 matches the typical HUB75 panel-pair canvas (2:1, often 128×64
        // native) at 2x. Frames are NEAREST-resized to keep the pixel-art look.
        face::GifPlayer          player{256, 128};
        std::vector<std::string> files;
        bool        scanned = false;
        std::string loaded_path;       // file currently decoded ("" = none)
        int         want    = -1;      // highlighted slot (set by on_highlight)
        GLuint      tex     = 0;
    };
    auto gif_preview = std::make_shared<GifPreview>();

    MenuContextPanelDraw draw_gif_preview =
        [gif_preview, gifs_dir, gif_names, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            GifPreview& gp = *gif_preview;
            if (!gp.scanned) {
                gp.files   = face::GifPlayer::scan_folder(gifs_dir);
                gp.scanned = true;
                if (gp.want < 0 && !gp.files.empty()) gp.want = 0;
            }

            // Resolve slot → file path: prefer the manifest binding (what
            // play_gif() will actually play), fall back to sorted scan order.
            std::string slot_path, slot_label;
            if (gp.want >= 0) {
                const std::string bound = teensy->gif_slot(static_cast<uint8_t>(gp.want));
                if (!bound.empty()) {
                    slot_path  = gifs_dir + "/" + bound;
                    slot_label = std::filesystem::path(bound).stem().string();
                } else if (gp.want < static_cast<int>(gp.files.size())) {
                    slot_path  = gp.files[gp.want];
                    slot_label = std::filesystem::path(slot_path).stem().string();
                }
            }
            const bool have = !slot_path.empty();

            if (have && slot_path != gp.loaded_path) {
                gp.player.load(slot_path, true);
                gp.loaded_path = slot_path;
            } else if (!have && !gp.loaded_path.empty()) {
                gp.player.stop();
                gp.loaded_path.clear();
            }
            gp.player.update(ImGui::GetIO().DeltaTime);

            // 2:1 thumbnail (matches the HUB75 panel-pair canvas aspect),
            // centred, leaving a line for the name beneath.
            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));

            cv::Mat fr = gp.player.get_frame();   // CV_8UC4 RGBA; empty when idle
            if (!fr.empty() && fr.isContinuous()) {
                if (gp.tex == 0) {
                    glGenTextures(1, &gp.tex);
                    glBindTexture(GL_TEXTURE_2D, gp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, gp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fr.cols, fr.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fr.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(gp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = !have ? "(empty)" : "Decode failed";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f, py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            std::string name = have
                ? ((gp.want < static_cast<int>(gif_names.size()) && !gif_names[gp.want].empty())
                       ? gif_names[gp.want]
                       : slot_label)
                : std::string("(none)");
            const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
            dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                        IM_COL32(220, 230, 235, 230), name.c_str());
        };

    // Build a GIF leaf:
    //   - Label is dynamic so the menu reflects manifest changes without a
    //     rebuild: shows the slot's configured name (gif_names[i]) or the bound
    //     filename's stem, with a "(empty)" suffix when the slot is unbound.
    //   - Highlight updates the preview only (no device command), so scrolling
    //     the list animates the thumbnail.
    //   - Select plays the bound GIF, or opens the file picker rooted at the
    //     last visited dir for an unbound slot — the import callback copies the
    //     chosen file into gifs_dir, binds the manifest slot, and plays it.
    auto gif_leaf = [&, gif_preview](uint8_t i) -> MenuItem {
        MenuItem m;
        m.type  = MenuItemType::LEAF;
        m.label = "GIF #" + std::to_string(static_cast<int>(i));   // static id fallback

        m.label_fn = [teensy, i, gn = gif_names]() -> std::string {
            const std::string bound = teensy->gif_slot(i);
            const bool named = (i < gn.size() && !gn[i].empty());
            if (!bound.empty())
                return named ? gn[i] : std::filesystem::path(bound).stem().string();
            std::string base = named ? gn[i]
                                     : "GIF #" + std::to_string(static_cast<int>(i));
            return base + " (empty)";
        };

        m.action = [teensy, i, menu_sys_pp, gifs_dir]() {
            if (!teensy->gif_slot(i).empty()) { teensy->play_gif(i); return; }
            import_gif_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                 teensy, gifs_dir, i);
        };

        m.on_highlight = [gif_preview, i]{ gif_preview->want = static_cast<int>(i); };
        return m;
    };

    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) gifs.push_back(gif_leaf(i));

    // Slot-management row for the Files > GIFs hub. The row itself is a
    // submenu whose visible children depend on whether the slot is bound:
    //   bound   → Play / Replace... / Clear
    //   unbound → Import...
    // Same dynamic label and preview-highlight behaviour as gif_leaf.
    auto gif_slot_row = [&, gif_preview](uint8_t i) -> MenuItem {
        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = "GIF Slot #" + std::to_string(static_cast<int>(i));

        m.label_fn = [teensy, i, gn = gif_names]() -> std::string {
            const std::string bound = teensy->gif_slot(i);
            const bool named = (i < gn.size() && !gn[i].empty());
            std::string name = !bound.empty()
                ? (named ? gn[i] : std::filesystem::path(bound).stem().string())
                : (named ? gn[i] : "GIF #" + std::to_string(static_cast<int>(i)));
            if (bound.empty()) name += " (empty)";
            return name;
        };

        m.on_highlight = [gif_preview, i]{ gif_preview->want = static_cast<int>(i); };

        auto bound_now = [teensy, i]{ return !teensy->gif_slot(i).empty(); };

        MenuItem play = leaf("Play", [teensy, i]{ teensy->play_gif(i); });
        play.visible_fn = bound_now;

        MenuItem replace = leaf("Replace...",
            [teensy, i, menu_sys_pp, gifs_dir]() {
                import_gif_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                     teensy, gifs_dir, i);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear", [teensy, i]{ teensy->clear_gif_slot(i); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, i, menu_sys_pp, gifs_dir]() {
                import_gif_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                     teensy, gifs_dir, i);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        m.children = { std::move(play), std::move(replace),
                       std::move(clear), std::move(imp) };
        return m;
    };

    std::vector<MenuItem> gif_files_menu;
    for (uint8_t i = 0; i < 8; i++) gif_files_menu.push_back(gif_slot_row(i));

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
        for (int i = 0; i < 24; ++i) {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "version";
            row.label_fn = [entries, i]{
                if (i >= static_cast<int>(entries->size())) return std::string();
                const auto& e = (*entries)[i];
                return e.named ? e.label : ("auto: " + e.label);
            };
            row.visible_fn   = [entries, i]{ return i < static_cast<int>(entries->size()); };
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
            items.push_back(std::move(row));
        }
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

    auto face_slot_row = [&, face_preview, edit_face, make_versions_submenu](int slot_idx) -> MenuItem {
        const std::string expr  = kFaceSlots[slot_idx].expression;
        const std::string label = kFaceSlots[slot_idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
        };

        m.on_highlight = [face_preview, slot_idx]{ face_preview->want = slot_idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem play = leaf("Play",
            [teensy, expr]{ teensy->set_face_by_name(expr); });
        play.visible_fn = bound_now;

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        // Copy from another face slot — useful as a starting point ("clone
        // 'happy' into 'wink' then tweak the eye"). Listed slots are gated
        // per-row to hide empties and self; the submenu itself hides when
        // there's nothing copyable. native_ctrl reload + on-screen set so
        // the new art shows immediately, matching the editor's commit path.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kFaceSlotCount; ++src_idx) {
            if (src_idx == slot_idx) continue;
            const std::string src_expr  = kFaceSlots[src_idx].expression;
            const std::string src_label = kFaceSlots[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another face's PNG into this slot as a "
                                "starting point. The source slot keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kFaceSlotCount; ++j) {
                if (kFaceSlots[j].expression == expr) continue;
                if (teensy->face_image_exists(kFaceSlots[j].expression)) return true;
            }
            return false;
        };

        // Edit launches the pixel editor on this slot's PNG. Visible only
        // when the active backend exposes covered LED regions — keeps the
        // option hidden in HUB75 / daemon modes where the editor would
        // have nothing meaningful to draw against.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem versions = make_versions_submenu(expr);
        versions.description = "Saved versions of this face (named + auto-backups) "
                               "with thumbnails — Make Current to restore one.";
        versions.visible_fn = bound_now;

        m.children = { std::move(play), std::move(edit_it), std::move(versions),
                       std::move(replace), std::move(copy_from),
                       std::move(clear), std::move(imp) };
        return m;
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

    auto mouth_slot_row = [&, mouth_preview, edit_face, have_led_regions](int idx) -> MenuItem {
        const std::string expr  = kMouthShapes[idx].file_stem;
        const std::string label = kMouthShapes[idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
        };
        m.on_highlight = [mouth_preview, idx]{ mouth_preview->want = idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        // Copy from another mouth-shape slot — useful when one viseme is
        // a small tweak on another (e.g. small → smile). Same pattern as
        // the expression Copy from... above.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kMouthShapeCount; ++src_idx) {
            if (src_idx == idx) continue;
            const std::string src_expr  = kMouthShapes[src_idx].file_stem;
            const std::string src_label = kMouthShapes[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another viseme's PNG into this slot as a "
                                "starting point. The source slot keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kMouthShapeCount; ++j) {
                if (kMouthShapes[j].file_stem == expr) continue;
                if (teensy->face_image_exists(kMouthShapes[j].file_stem)) return true;
            }
            return false;
        };

        // Edit the mouth-shape PNG with the pixel editor (mono on MAX7219,
        // color on RGB matrix). Same visibility gate as the expression
        // slots — hidden in HUB75 / daemon modes.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem versions = make_versions_submenu(expr);
        versions.visible_fn = bound_now;

        m.children = { std::move(edit_it), std::move(versions), std::move(replace),
                       std::move(copy_from), std::move(clear), std::move(imp) };
        return m;
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

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
        };
        m.on_highlight = [boop_face_preview, idx]{ boop_face_preview->want = idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem play = leaf("Play",
            [teensy, expr, idx, &state]() {
                double dur = 0.8;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (idx >= 0 && idx < 4) dur = state.boop_zones[idx].duration_s;
                }
                teensy->trigger_boop(expr, dur);
            });
        play.visible_fn = bound_now;

        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        // Copy from another boop reaction slot — same pattern as the
        // expression / mouth slot rows.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kBoopFaceSlotCount; ++src_idx) {
            if (src_idx == idx) continue;
            const std::string src_expr  = kBoopFaceSlots[src_idx].file_stem;
            const std::string src_label = kBoopFaceSlots[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another boop reaction's PNG into this "
                                "slot as a starting point. The source slot "
                                "keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kBoopFaceSlotCount; ++j) {
                if (kBoopFaceSlots[j].file_stem == expr) continue;
                if (teensy->face_image_exists(kBoopFaceSlots[j].file_stem)) return true;
            }
            return false;
        };

        MenuItem versions = make_versions_submenu(expr);
        versions.visible_fn = bound_now;

        m.children = { std::move(play), std::move(edit_it), std::move(versions),
                       std::move(replace), std::move(copy_from),
                       std::move(clear), std::move(imp) };
        return m;
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

    // ── Backgrounds ──────────────────────────────────────────────────────────
    // Landing-page background library. Unlike GIFs/Faces this isn't slot-based:
    // the library is a flat sorted list (defaults under assets/backgrounds win
    // over duplicates under $HOME/protohud/backgrounds). v1 surfaces Import +
    // per-entry Delete (only for user-owned files); cycling between them on
    // the landing page is unchanged.
    struct BgPreview {
        std::string                     loaded_path;
        std::filesystem::file_time_type loaded_mtime{};
        cv::Mat     image;
        int         want = -1;        // highlighted entry index
        GLuint      tex  = 0;
    };
    auto bg_preview = std::make_shared<BgPreview>();

    auto bg_get_lib = [bg_lib_pp]() -> BackgroundLibrary* {
        return bg_lib_pp ? *bg_lib_pp : nullptr;
    };

    MenuContextPanelDraw draw_bg_preview =
        [bg_preview, bg_get_lib](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            BgPreview& bp = *bg_preview;
            auto* lib = bg_get_lib();
            std::string path, label;
            if (lib && bp.want >= 0 && bp.want < lib->count()) {
                path  = lib->path(bp.want);
                label = lib->name(bp.want);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != bp.loaded_path || mt != bp.loaded_mtime)) {
                bp.image        = face::load_png_rgba(path, 256, 160);
                bp.loaded_path  = path;
                bp.loaded_mtime = mt;
            } else if (!have && !bp.loaded_path.empty()) {
                bp.image = cv::Mat();
                bp.loaded_path.clear();
                bp.loaded_mtime = {};
            }

            // 16:10 thumbnail, centred, name beneath.
            const float w = std::min(sz.x * 0.9f, sz.y * 0.85f * 1.6f);
            const float h = w / 1.6f;
            const float px = o.x + (sz.x - w) * 0.5f;
            const float py = o.y + (sz.y - h) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + w, py + h}, IM_COL32(10, 16, 22, 190));

            if (have && !bp.image.empty() && bp.image.isContinuous()) {
                if (bp.tex == 0) {
                    glGenTextures(1, &bp.tex);
                    glBindTexture(GL_TEXTURE_2D, bp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, bp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bp.image.cols, bp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, bp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(bp.tex)),
                             {px, py}, {px + w, py + h});
            } else {
                const char* msg = have ? "Decode failed" : "(no backgrounds)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + w * 0.5f - ts.x * 0.5f, py + h * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!label.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(label.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), label.c_str());
            }
        };

    // Import leaf: opens picker with image filters; on commit copies the file
    // into the user bg dir and refreshes the library.
    MenuItem bg_import = leaf("Import...",
        [bg_lib_pp, bg_user_dir, menu_sys_pp]() {
            if (!menu_sys_pp || !*menu_sys_pp) return;
            std::string start = (*menu_sys_pp)->file_picker_dir();
            (*menu_sys_pp)->open_file_picker(
                "Import background", std::move(start),
                {".png", ".jpg", ".jpeg", ".bmp"},
                [bg_lib_pp, bg_user_dir](const std::string& src) {
                    std::error_code ec;
                    std::filesystem::create_directories(bg_user_dir, ec);
                    const std::string fname =
                        std::filesystem::path(src).filename().string();
                    const std::string dst = bg_user_dir + "/" + fname;
                    std::filesystem::copy_file(
                        src, dst,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        std::fprintf(stderr,
                            "[bg] import copy failed %s -> %s: %s\n",
                            src.c_str(), dst.c_str(), ec.message().c_str());
                        return;
                    }
                    if (auto* lib = bg_lib_pp ? *bg_lib_pp : nullptr) {
                        lib->refresh();
                        // Select the just-imported background so it's visible
                        // immediately on the next landing-page open.
                        lib->set_current_by_name(
                            std::filesystem::path(fname).stem().string());
                    }
                });
        });

    // One leaf per library entry — highlight-only, so the preview pane animates
    // as the user scrolls. visible_fn tied to the live count, so adds / deletes
    // show up without rebuilding the menu tree. Caps at 16 entries; bump if
    // needed. A read-only suffix on bundled defaults makes it obvious which
    // entries can be deleted.
    constexpr int kBgRowCap = 16;
    auto bg_is_user_at = [bg_get_lib, bg_user_dir](int idx) -> bool {
        auto* lib = bg_get_lib();
        if (!lib || idx < 0 || idx >= lib->count()) return false;
        const std::string& p = lib->path(idx);
        return !bg_user_dir.empty() && p.rfind(bg_user_dir, 0) == 0;
    };

    auto bg_row = [&, bg_preview](int idx) -> MenuItem {
        MenuItem m;
        m.type  = MenuItemType::LEAF;
        m.label = "Background #" + std::to_string(idx);

        m.label_fn = [bg_get_lib, bg_is_user_at, idx]() -> std::string {
            auto* lib = bg_get_lib();
            if (!lib || idx >= lib->count()) return {};
            std::string n = lib->name(idx);
            if (!bg_is_user_at(idx)) n += "  (read-only)";
            return n;
        };
        m.visible_fn = [bg_get_lib, idx]{
            auto* lib = bg_get_lib();
            return lib && idx < lib->count();
        };
        m.on_highlight = [bg_preview, idx]{ bg_preview->want = idx; };
        m.action = []{};   // highlighting drives the preview; selecting is a no-op
        return m;
    };

    // Single Delete entry that targets the currently-highlighted background,
    // visible only when that selection is a user import (bundled defaults
    // stay read-only).
    MenuItem bg_delete = leaf("Delete Highlighted",
        [bg_preview, bg_get_lib, bg_user_dir]() {
            auto* lib = bg_get_lib();
            if (!lib) return;
            const int i = bg_preview->want;
            if (i < 0 || i >= lib->count()) return;
            const std::string p = lib->path(i);
            if (bg_user_dir.empty() || p.rfind(bg_user_dir, 0) != 0) return;
            std::error_code ec;
            std::filesystem::remove(p, ec);
            lib->refresh();
            // refresh() may shuffle indices; just clamp so want stays valid.
            if (bg_preview->want >= lib->count()) bg_preview->want = lib->count() - 1;
        });
    bg_delete.visible_fn = [bg_preview, bg_is_user_at]{
        return bg_is_user_at(bg_preview->want);
    };

    std::vector<MenuItem> bg_files_menu;
    bg_files_menu.push_back(std::move(bg_import));
    for (int i = 0; i < kBgRowCap; ++i) bg_files_menu.push_back(bg_row(i));
    bg_files_menu.push_back(std::move(bg_delete));

    // ── Camera controls ───────────────────────────────────────────────────────
    std::vector<MenuItem> af_triggers = {
        leaf("Left",  [cameras, &state]{
            if (!cameras || !cameras->owl_left()) return;
            cameras->owl_left()->start_autofocus();
            if (state.focus_left.mode == CameraFocusState::Mode::SLAVE && cameras->owl_right()) {
                std::thread([cameras](){
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (cameras->owl_left() && cameras->owl_right())
                        cameras->owl_right()->set_focus_position(
                            cameras->owl_left()->get_focus_position());
                }).detach();
            }
        }),
        leaf("Right", [cameras, &state]{
            if (!cameras || !cameras->owl_right()) return;
            cameras->owl_right()->start_autofocus();
            if (state.focus_right.mode == CameraFocusState::Mode::SLAVE && cameras->owl_left()) {
                std::thread([cameras](){
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (cameras->owl_left() && cameras->owl_right())
                        cameras->owl_left()->set_focus_position(
                            cameras->owl_right()->get_focus_position());
                }).detach();
            }
        }),
        leaf("Both",  [cameras, &state]{
            if (!cameras) return;
            if (cameras->owl_left())  cameras->owl_left()->start_autofocus();
            if (cameras->owl_right()) cameras->owl_right()->start_autofocus();
            if ((state.focus_left.mode  == CameraFocusState::Mode::SLAVE ||
                 state.focus_right.mode == CameraFocusState::Mode::SLAVE) &&
                cameras->owl_left() && cameras->owl_right()) {
                std::thread([cameras](){
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    int avg = (cameras->owl_left()->get_focus_position()
                             + cameras->owl_right()->get_focus_position()) / 2;
                    cameras->owl_left()->set_focus_position(avg);
                    cameras->owl_right()->set_focus_position(avg);
                }).detach();
            }
        }),
    };

    std::vector<MenuItem> shutter_speeds = {
        leaf_sel("1/4000", [&state]{ state.night_vision.shutter_us =   250; }, [&state]{ return state.night_vision.shutter_us ==   250; }),
        leaf_sel("1/2000", [&state]{ state.night_vision.shutter_us =   500; }, [&state]{ return state.night_vision.shutter_us ==   500; }),
        leaf_sel("1/1000", [&state]{ state.night_vision.shutter_us =  1000; }, [&state]{ return state.night_vision.shutter_us ==  1000; }),
        leaf_sel("1/500",  [&state]{ state.night_vision.shutter_us =  2000; }, [&state]{ return state.night_vision.shutter_us ==  2000; }),
        leaf_sel("1/250",  [&state]{ state.night_vision.shutter_us =  4000; }, [&state]{ return state.night_vision.shutter_us ==  4000; }),
        leaf_sel("1/125",  [&state]{ state.night_vision.shutter_us =  8000; }, [&state]{ return state.night_vision.shutter_us ==  8000; }),
        leaf_sel("1/60",   [&state]{ state.night_vision.shutter_us = 16667; }, [&state]{ return state.night_vision.shutter_us == 16667; }),
        leaf_sel("1/30",   [&state]{ state.night_vision.shutter_us = 33333; }, [&state]{ return state.night_vision.shutter_us == 33333; }),
        leaf_sel("1/25",   [&state]{ state.night_vision.shutter_us = 40000; }, [&state]{ return state.night_vision.shutter_us == 40000; }),
    };

    // ── Resolution presets ────────────────────────────────────────────────────
    struct ResPreset { const char* label; int w, h, fps; };
    static const ResPreset RES_PRESETS[] = {
        { "640x400  @120fps",  640,  400, 120 },
        { "1280x800  @60fps", 1280,  800,  60 },  // default
        { "1920x1080 @30fps", 1920, 1080,  30 },
        { "2560x1440 @15fps", 2560, 1440,  15 },
    };

    std::vector<MenuItem> resolution_presets;
    for (const auto& p : RES_PRESETS) {
        resolution_presets.push_back(leaf_sel(
            p.label,
            [cameras, &state, w = p.w, h = p.h, fps = p.fps](){
                if (!cameras) return;
                if (cameras->set_resolution(w, h, fps)) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.camera_resolution = { w, h, fps };
                }
            },
            [&state, w = p.w, h = p.h]{ return state.camera_resolution.width == w && state.camera_resolution.height == h; }
        ));
    }

    std::vector<MenuItem> nv_menu = {
        with_desc(toggle("Enable Low-Light",
            [&state]{ return state.night_vision.nv_enabled; },
            [&state](bool v){
                state.night_vision.nv_enabled  = v;
                state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
                state.night_vision.shutter_us  = v ? 40000 : 16667;
            }),
            "Brighten dark scenes by raising exposure and lengthening the shutter. "
            "Makes the view usable in low light, but adds motion blur and image noise."),
        with_desc(toggle("Auto Low-Light",
            [&state]{ return state.night_vision.auto_nv; },
            [&state](bool v){ state.night_vision.auto_nv = v; }),
            "Turn the low-light boost on automatically when the scene gets dark "
            "(camera gain rises past the threshold below), and off again when it brightens."),
        with_desc(slider("Auto Gain Threshold", 1.5f, 16.f, 0.5f, "x",
            [&state]{ return state.night_vision.auto_nv_gain_threshold; },
            [&state](float v){ state.night_vision.auto_nv_gain_threshold = v; }),
            "How dark it must get before Auto Low-Light engages. Lower = kicks in sooner "
            "(dimmer rooms); higher = only in very dark scenes."),
        with_desc(slider("Exposure (EV)", -3.f, 3.f, 0.5f, " EV",
            [&state]{ return state.night_vision.exposure_ev; },
            [&state](float v){ state.night_vision.exposure_ev = v; }),
            "Brightness compensation. Higher EV brightens the image (better in the dark) "
            "but adds motion blur; lower darkens it for bright scenes."),
        with_desc(submenu("Shutter Speed", std::move(shutter_speeds)),
            "How long each frame is exposed. Slower gathers more light (brighter, but "
            "movement smears); faster is sharper but darker."),
    };

    // ── Per-camera controls (OWLsight CSI cameras) ───────────────────────────
    // Helper: build focus + WB items for one camera.
    // cam_ptr  — accessor returning DmaCamera*
    // focus_st — reference to the per-eye CameraFocusState in AppState
    // awb_flag — reference to the per-eye AWB bool in NightVisionState
    struct ColourTemp { const char* label; int k; };
    static const ColourTemp WB_PRESETS[] = {
        { "Tungsten (2800K)",  2800 },
        { "Warm (3500K)",      3500 },
        { "Neutral (4500K)",   4500 },
        { "Daylight (5600K)",  5600 },
        { "Cool Blue (7000K)", 7000 },
    };

    auto make_cam_menu = [&](
        std::function<DmaCamera*()>     cam_ptr,
        CameraFocusState&               focus_st,
        bool&                           awb_flag)
    {
        std::vector<MenuItem> fm = {
            leaf_sel("Manual", [cameras, cam_ptr, &focus_st]{
                focus_st.mode = CameraFocusState::Mode::MANUAL;
                if (auto* c = cam_ptr()) c->stop_autofocus();
            }, [&focus_st]{ return focus_st.mode == CameraFocusState::Mode::MANUAL; }),
            leaf_sel("Auto", [cam_ptr, &focus_st]{
                focus_st.mode = CameraFocusState::Mode::AUTO;
                if (auto* c = cam_ptr()) c->start_autofocus();
            }, [&focus_st]{ return focus_st.mode == CameraFocusState::Mode::AUTO; }),
            leaf_sel("Slave", [cam_ptr, &focus_st]{
                focus_st.mode = CameraFocusState::Mode::SLAVE;
                if (auto* c = cam_ptr()) c->stop_autofocus();
            }, [&focus_st]{ return focus_st.mode == CameraFocusState::Mode::SLAVE; }),
            leaf("Trigger AF", [cam_ptr]{
                if (auto* c = cam_ptr()) c->start_autofocus();
            }),
            slider("Focus Position", 0.f, 1000.f, 25.f, "",
                [cam_ptr]{ auto* c = cam_ptr(); return c ? (float)c->get_focus_position() : 0.f; },
                [cam_ptr, &focus_st](float v){
                    focus_st.focus_position = (int)v;
                    if (auto* c = cam_ptr()) c->set_focus_position((int)v);
                }),
        };

        std::vector<MenuItem> wbm;
        wbm.push_back(toggle("Auto WB",
            [&awb_flag]{ return awb_flag; },
            [cam_ptr, &awb_flag](bool v){
                awb_flag = v;
                if (auto* c = cam_ptr()) c->set_awb_enable(v);
            }));
        for (const auto& p : WB_PRESETS)
            wbm.push_back(leaf(p.label, [cam_ptr, k = p.k]{
                if (auto* c = cam_ptr()) c->set_colour_temp(k);
            }));

        // Rotation — applied as UV math in the NV12 vertex shader (no
        // readback, ~free). Snaps to multiples of 90°; persists on save.
        // Mounting a CSI camera sideways on the helmet is the common case.
        auto rot_item = [&](const char* label, int deg) {
            return leaf_sel(label,
                [cam_ptr, deg]{ if (auto* c = cam_ptr()) c->set_rotation(deg); },
                [cam_ptr, deg]{ auto* c = cam_ptr(); return c && c->rotation() == deg; });
        };
        std::vector<MenuItem> rm = {
            rot_item("0\xc2\xb0   (Normal)",      0),
            rot_item("90\xc2\xb0  (Clockwise)",   90),
            rot_item("180\xc2\xb0 (Upside down)", 180),
            rot_item("270\xc2\xb0 (Counterclockwise)", 270),
        };

        return std::vector<MenuItem>{
            submenu("Focus",          std::move(fm)),
            submenu("White Balance",  std::move(wbm)),
            submenu("Rotation",       std::move(rm)),
        };
    };

    auto left_cam_menu  = make_cam_menu(
        [cameras]{ return cameras ? cameras->owl_left()  : nullptr; },
        state.focus_left,  state.night_vision.csi_awb_left);
    auto right_cam_menu = make_cam_menu(
        [cameras]{ return cameras ? cameras->owl_right() : nullptr; },
        state.focus_right, state.night_vision.csi_awb_right);

    // ── Digital zoom presets (both eyes together) ─────────────────────────────
    struct ZoomPreset { const char* label; float zoom; };
    static const ZoomPreset ZOOM_PRESETS[] = {
        { "1.0× (Full)", 1.00f },
        { "1.25×",       1.25f },
        { "1.5×",        1.50f },
        { "2.0×",        2.00f },
        { "3.0×",        3.00f },
    };

    std::vector<MenuItem> zoom_level_menu;
    for (const auto& z : ZOOM_PRESETS) {
        zoom_level_menu.push_back(live(leaf_sel(
            z.label,
            [&state, zoom = z.zoom]{
                state.zoom_left.zoom  = zoom;
                state.zoom_right.zoom = zoom;
            },
            [&state, zoom = z.zoom]{ return state.zoom_left.zoom == zoom; }
        )));
    }

    struct CropPreset { const char* label; float cx, cy; };
    static const CropPreset CROP_PRESETS[] = {
        { "Center",       0.5f, 0.5f },
        { "Top",          0.5f, 0.25f },
        { "Bottom",       0.5f, 0.75f },
        { "Left",         0.25f, 0.5f },
        { "Right",        0.75f, 0.5f },
    };

    std::vector<MenuItem> crop_center_menu;
    for (const auto& c : CROP_PRESETS) {
        crop_center_menu.push_back(live(leaf_sel(
            c.label,
            [&state, cx = c.cx, cy = c.cy]{
                state.zoom_left.center_x  = cx;
                state.zoom_left.center_y  = cy;
                state.zoom_right.center_x = cx;
                state.zoom_right.center_y = cy;
            },
            [&state, cx = c.cx, cy = c.cy]{
                return state.zoom_left.center_x == cx && state.zoom_left.center_y == cy;
            }
        )));
    }

    std::vector<MenuItem> zoom_menu = {
        submenu("Zoom Level",  std::move(zoom_level_menu)),
        submenu("Crop Center", std::move(crop_center_menu)),
        leaf("Reset Zoom", [&state]{
            state.zoom_left  = ZoomCropState{};
            state.zoom_right = ZoomCropState{};
        }),
    };

    // ── Mirror Crop ────────────────────────────────────────────────────────────
    // Both eyes zoom to the same level and pan inward (nose-side crop).
    // Vertical position (top/middle/bottom) is shared. inner_bias controls how
    // far from center the crop window is placed.
    std::vector<MenuItem> mirror_crop_zoom_items = {
        leaf_sel("1.5×", [&state]{ state.mirror_crop.zoom = 1.5f; },
                         [&state]{ return state.mirror_crop.zoom == 1.5f; }),
        leaf_sel("2.0×", [&state]{ state.mirror_crop.zoom = 2.0f; },
                         [&state]{ return state.mirror_crop.zoom == 2.0f; }),
        leaf_sel("2.5×", [&state]{ state.mirror_crop.zoom = 2.5f; },
                         [&state]{ return state.mirror_crop.zoom == 2.5f; }),
        leaf_sel("3.0×", [&state]{ state.mirror_crop.zoom = 3.0f; },
                         [&state]{ return state.mirror_crop.zoom == 3.0f; }),
    };
    std::vector<MenuItem> mirror_crop_vert_items = {
        leaf_sel("Top",    [&state]{ state.mirror_crop.vertical = CropVertical::Top;    },
                           [&state]{ return state.mirror_crop.vertical == CropVertical::Top;    }),
        leaf_sel("Middle", [&state]{ state.mirror_crop.vertical = CropVertical::Middle; },
                           [&state]{ return state.mirror_crop.vertical == CropVertical::Middle; }),
        leaf_sel("Bottom", [&state]{ state.mirror_crop.vertical = CropVertical::Bottom; },
                           [&state]{ return state.mirror_crop.vertical == CropVertical::Bottom; }),
    };
    // Preview these options as the user tabs through them (live).
    for (auto& m : mirror_crop_zoom_items) m.on_highlight = m.action;
    for (auto& m : mirror_crop_vert_items) m.on_highlight = m.action;
    std::vector<MenuItem> mirror_crop_menu = {
        toggle("Enable", [&state]{ return state.mirror_crop.enabled; },
                         [&state](bool v){ state.mirror_crop.enabled = v; }),
        submenu("Zoom",           std::move(mirror_crop_zoom_items)),
        submenu("Vertical",       std::move(mirror_crop_vert_items)),
        slider("Inner Bias", 0.f, 0.40f, 0.05f, "",
            [&state]{ return state.mirror_crop.inner_bias; },
            [&state](float v){ state.mirror_crop.inner_bias = v; }),
    };

    // ── Single Camera ──────────────────────────────────────────────────────────
    // Fill one anchor region of the screen with a single camera feed.
    std::vector<MenuItem> single_cam_anchor_items = {
        leaf_sel("Full Screen", [&state]{ state.cam_single.anchor = CamSingleAnchor::Full;   },
                                [&state]{ return state.cam_single.anchor == CamSingleAnchor::Full;   }),
        leaf_sel("Top Half",    [&state]{ state.cam_single.anchor = CamSingleAnchor::Top;    },
                                [&state]{ return state.cam_single.anchor == CamSingleAnchor::Top;    }),
        leaf_sel("Bottom Half", [&state]{ state.cam_single.anchor = CamSingleAnchor::Bottom; },
                                [&state]{ return state.cam_single.anchor == CamSingleAnchor::Bottom; }),
        leaf_sel("Left Half",   [&state]{ state.cam_single.anchor = CamSingleAnchor::Left;   },
                                [&state]{ return state.cam_single.anchor == CamSingleAnchor::Left;   }),
        leaf_sel("Right Half",  [&state]{ state.cam_single.anchor = CamSingleAnchor::Right;  },
                                [&state]{ return state.cam_single.anchor == CamSingleAnchor::Right;  }),
    };
    // Preview each anchor live as the user tabs through them.
    for (auto& m : single_cam_anchor_items) if (m.get_state) m.on_highlight = m.action;

    // Single-camera layout preview: which screen region the one feed fills.
    MenuContextPanelDraw single_cam_preview =
        [&state, menu_sys_pp](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                ? (*menu_sys_pp)->accent_color() : IM_COL32(255, 255, 255, 255);
            const float fw = sz.x, fh = sz.y - 16.f;
            float rx0 = 0.f, ry0 = 0.f, rx1 = 1.f, ry1 = 1.f;
            switch (state.cam_single.anchor) {
                case CamSingleAnchor::Top:    ry1 = 0.5f; break;
                case CamSingleAnchor::Bottom: ry0 = 0.5f; break;
                case CamSingleAnchor::Left:   rx1 = 0.5f; break;
                case CamSingleAnchor::Right:  rx0 = 0.5f; break;
                default: break;  // Full
            }
            const ImVec2 a{ o.x + fw * rx0, o.y + fh * ry0 };
            const ImVec2 b{ o.x + fw * rx1, o.y + fh * ry1 };
            dl->AddRect(o, { o.x + fw, o.y + fh }, IM_COL32(120, 130, 140, 150), 0.f, 0, 1.5f);
            dl->AddRectFilled(a, b, (accent & 0x00FFFFFFu) | (70u  << 24));
            dl->AddRect      (a, b, (accent & 0x00FFFFFFu) | (220u << 24), 0.f, 0, 2.f);
            const char* cam = state.cam_single.use_right ? "Right camera" : "Left camera";
            dl->AddText({ o.x, o.y + fh + 2.f }, IM_COL32(200, 200, 200, 200), cam);
            if (!state.cam_single.enabled)
                dl->AddText({ o.x + 6.f, o.y + 6.f }, IM_COL32(220, 160, 60, 220), "(disabled)");
        };

    std::vector<MenuItem> single_cam_menu = {
        toggle("Enable", [&state]{ return state.cam_single.enabled; },
                         [&state](bool v){ state.cam_single.enabled = v; }),
        toggle("Use Right Camera", [&state]{ return state.cam_single.use_right; },
                                   [&state](bool v){ state.cam_single.use_right = v; }),
        submenu("Anchor", std::move(single_cam_anchor_items)),
    };

    // "Autofocus Both" shortcut kept for convenience — triggers AF on both cameras at once.
    std::vector<MenuItem> af_both_menu = std::move(af_triggers);

    std::vector<MenuItem> capture_menu = {
        leaf("Left Eye",   [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Left;   }),
        leaf("Right Eye",  [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Right;  }),
        leaf("Both Eyes",  [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Stereo; }),
    };

    std::vector<MenuItem> video_camera_menu = {
        leaf_sel("Left",  [&state]{ std::lock_guard lk(state.mtx); state.video_camera = VideoCamera::Left;  },
                          [&state]{ return state.video_camera == VideoCamera::Left;  }),
        leaf_sel("Right", [&state]{ std::lock_guard lk(state.mtx); state.video_camera = VideoCamera::Right; },
                          [&state]{ return state.video_camera == VideoCamera::Right; }),
        leaf_sel("Both",  [&state]{ std::lock_guard lk(state.mtx); state.video_camera = VideoCamera::Both;  },
                          [&state]{ return state.video_camera == VideoCamera::Both;  }),
    };

    std::vector<MenuItem> video_menu = {
        toggle("Record",
            [&state]{ return state.video_recording; },
            [&state](bool v){ std::lock_guard lk(state.mtx);
                              state.video_request = v ? VideoRequest::Start : VideoRequest::Stop; }),
        submenu("Camera", std::move(video_camera_menu)),
    };

    std::vector<MenuItem> qr_menu = {
        toggle("Main Cameras", [&state]{ return state.qr_scan_main; },
                               [&state](bool v){ state.qr_scan_main = v; }),
        toggle("USB Cameras",  [&state]{ return state.qr_scan_usb; },
                               [&state](bool v){ state.qr_scan_usb = v; }),
    };

    using TA = AppState::TheaterAnchor;
    std::vector<MenuItem> theater_pos_menu = {
        leaf_sel("Center",  [&state]{ state.theater_anchor = TA::Center;  }, [&state]{ return state.theater_anchor == TA::Center;  }),
        leaf_sel("Outside", [&state]{ state.theater_anchor = TA::Outside; }, [&state]{ return state.theater_anchor == TA::Outside; }),
        leaf_sel("Left",    [&state]{ state.theater_anchor = TA::Left;    }, [&state]{ return state.theater_anchor == TA::Left;    }),
        leaf_sel("Right",   [&state]{ state.theater_anchor = TA::Right;   }, [&state]{ return state.theater_anchor == TA::Right;   }),
        leaf_sel("Top",     [&state]{ state.theater_anchor = TA::Top;     }, [&state]{ return state.theater_anchor == TA::Top;     }),
        leaf_sel("Bottom",  [&state]{ state.theater_anchor = TA::Bottom;  }, [&state]{ return state.theater_anchor == TA::Bottom;  }),
        leaf("Reset",       [&state]{ state.theater_anchor = TA::Center;  }),
    };
    // Preview each eye position as the user tabs through them (live). Only the
    // selectable options (those with a selected-state), not the Reset leaf.
    for (auto& m : theater_pos_menu) if (m.get_state) m.on_highlight = m.action;

    // ── Eye source selection submenus ─────────────────────────────────────────
    // Build a 4-item radio list for one eye (CSI / USB 1 / USB 2 / USB 3).
    auto make_eye_source_menu = [&](EyeSource* src) -> std::vector<MenuItem> {
        if (!src) return {};
        return {
            leaf_sel("CSI Camera",
                [src]{ *src = EyeSource::CSI;  },
                [src]{ return *src == EyeSource::CSI;  }),
            leaf_sel("USB Cam 1",
                [src]{ *src = EyeSource::USB1; },
                [src]{ return *src == EyeSource::USB1; }),
            leaf_sel("USB Cam 2",
                [src]{ *src = EyeSource::USB2; },
                [src]{ return *src == EyeSource::USB2; }),
            leaf_sel("USB Cam 3",
                [src]{ *src = EyeSource::USB3; },
                [src]{ return *src == EyeSource::USB3; }),
        };
    };

    // ── Context-panel helpers (camera-frame visualisations) ──────────────────
    // Render a camera frame at `aspect`, fitting into the given rect, with an
    // accent-coloured outer outline and a lighter-grey inner crop rectangle.
    // inner_{x0,y0,x1,y1} are 0..1 within the frame.
    auto draw_frame_with_crop = [](ImDrawList* dl, ImVec2 fit_origin, ImVec2 fit_size,
                                   float aspect, ImU32 accent,
                                   float ix0, float iy0, float ix1, float iy1) {
        // Letterbox the frame inside the fit rect at the requested aspect.
        float fw = fit_size.x, fh = fit_size.y;
        if (fw / fh > aspect) fw = fh * aspect; else fh = fw / aspect;
        ImVec2 fmin{ fit_origin.x + (fit_size.x - fw) * 0.5f,
                     fit_origin.y + (fit_size.y - fh) * 0.5f };
        ImVec2 fmax{ fmin.x + fw, fmin.y + fh };

        // Dark fill so the inner crop reads clearly.
        dl->AddRectFilled(fmin, fmax, IM_COL32(20, 25, 30, 180), 3.f);
        // Inner crop rectangle (lighter grey fill + white outline).
        ImVec2 imin{ fmin.x + ix0 * fw, fmin.y + iy0 * fh };
        ImVec2 imax{ fmin.x + ix1 * fw, fmin.y + iy1 * fh };
        dl->AddRectFilled(imin, imax, IM_COL32(220, 220, 220, 110), 2.f);
        dl->AddRect      (imin, imax, IM_COL32(255, 255, 255, 220), 2.f, 0, 1.5f);
        // Outer outline last so it stays on top of inner fill at the edges.
        const ImU32 outline = (accent & 0x00FFFFFFu) | (235u << 24);
        dl->AddRect(fmin, fmax, outline, 3.f, 0, 2.f);
    };

    // Map a TheaterAnchor enum to the inner-crop rect (normalised) for one eye.
    // The "Outside" anchor is mirrored per-eye (left cam shows its left, right
    // cam shows its right).  All others apply identically to both eyes.
    auto theater_inner_rect = [](AppState::TheaterAnchor a, bool is_right_eye) {
        using TA = AppState::TheaterAnchor;
        // Half-frame width (each eye is half-SBS), full height by default.
        float w = 0.5f, h = 1.0f;
        float x0 = 0.25f, y0 = 0.f;   // Center
        switch (a) {
            case TA::Center:  x0 = 0.25f;                            break;
            case TA::Outside: x0 = is_right_eye ? 0.5f : 0.f;        break;
            case TA::Left:    x0 = 0.0f;                             break;
            case TA::Right:   x0 = 0.5f;                             break;
            case TA::Top:     y0 = 0.f;    h = 0.6f; x0 = 0.25f;     break;
            case TA::Bottom:  y0 = 0.4f;   h = 0.6f; x0 = 0.25f;     break;
        }
        return std::tuple<float,float,float,float>{ x0, y0, x0 + w, y0 + h };
    };

    // Eye position preview — shared by the Raw View submenu (so it shows the
    // whole time, incl. the Enable toggle) and its Position sub-level (via the
    // deep menu's walk-up). Crop box only appears when raw view is enabled.
    MenuContextPanelDraw eye_pos_preview =
            [&state, cameras, menu_sys_pp, draw_frame_with_crop, theater_inner_rect]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                const float aspect = (cameras && cameras->current_height() > 0)
                    ? float(cameras->current_width()) / float(cameras->current_height())
                    : 4.0f / 3.0f;
                const bool single = state.cam_single.enabled;
                const auto a = state.theater_anchor;

                if (single) {
                    const bool right = state.cam_single.use_right;
                    auto [x0, y0, x1, y1] = theater_inner_rect(a, right);
                    draw_frame_with_crop(dl, o, sz, aspect, accent, x0, y0, x1, y1);
                    const char* lbl = right ? "Right camera" : "Left camera";
                    dl->AddText({ o.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200), lbl);
                } else {
                    ImVec2 lsz{ sz.x * 0.5f - 6.f, sz.y - 16.f };
                    ImVec2 lpos{ o.x, o.y };
                    ImVec2 rpos{ o.x + sz.x * 0.5f + 6.f, o.y };
                    auto [lx0, ly0, lx1, ly1] = theater_inner_rect(a, false);
                    auto [rx0, ry0, rx1, ry1] = theater_inner_rect(a, true);
                    draw_frame_with_crop(dl, lpos, lsz, aspect, accent, lx0, ly0, lx1, ly1);
                    draw_frame_with_crop(dl, rpos, lsz, aspect, accent, rx0, ry0, rx1, ry1);
                    dl->AddText({ lpos.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200), "Left");
                    dl->AddText({ rpos.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200), "Right");
                }
            };

    // Digital-zoom preview — single crop window at the chosen zoom + center.
    MenuContextPanelDraw digital_zoom_preview =
            [&state, cameras, menu_sys_pp, draw_frame_with_crop]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                const float aspect = (cameras && cameras->current_height() > 0)
                    ? float(cameras->current_width()) / float(cameras->current_height())
                    : 4.0f / 3.0f;
                const float zoom = std::max(1.0f, state.zoom_left.zoom);
                const float cw = 1.0f / zoom, ch = 1.0f / zoom;
                float x0 = std::clamp(state.zoom_left.center_x - cw * 0.5f, 0.f, 1.f - cw);
                float y0 = std::clamp(state.zoom_left.center_y - ch * 0.5f, 0.f, 1.f - ch);
                draw_frame_with_crop(dl, o, sz, aspect, accent, x0, y0, x0 + cw, y0 + ch);
            };

    // Raw View groups the camera-passthrough toggle with its placement options.
    std::vector<MenuItem> raw_view_menu = {
        toggle("Enable",
            [&state]{ return state.theater_mode; },
            [&state](bool v){ state.theater_mode = v; }),
        submenu("Position", std::move(theater_pos_menu)),
    };

    // Build per-camera submenu labels from the configured model names.  When both
    // cameras share a model (e.g. default OWLsight pair) disambiguate with #1/#2
    // plus an eye hint; otherwise show "<Model> (Left/Right)".
    auto cam_label = [](std::string model, const char* eye, const char* index) {
        if (model.empty()) model = "Owlsight";
        std::string out = std::move(model);
        if (index) { out += " #"; out += index; }
        out += " (";
        out += eye;
        out += ")";
        return out;
    };
    std::string left_model  = cameras ? cameras->owl_left_model()  : std::string();
    std::string right_model = cameras ? cameras->owl_right_model() : std::string();
    std::string left_norm   = left_model.empty()  ? "Owlsight" : left_model;
    std::string right_norm  = right_model.empty() ? "Owlsight" : right_model;
    const bool same_model   = (left_norm == right_norm);
    std::string left_label  = cam_label(left_model,  "Left",  same_model ? "1" : nullptr);
    std::string right_label = cam_label(right_model, "Right", same_model ? "2" : nullptr);

    // ── Multi-Cam layout ─────────────────────────────────────────────────────
    // Each eye shows a top + bottom image (full-width, stacked); the two eyes use
    // independent sources, so side-by-side reads as four distinct feeds with no
    // duplicates. All four slots (left/right eye × top/bottom) pick from both CSI
    // cameras + the three USB cams via make_mc_src_menu below.

    // Quadrant source picker offering both CSI cameras + the three USB cams, so
    // any quadrant can show Left CSI, Right CSI or a USB feed.
    auto make_mc_src_menu = [&](EyeSource* slot) -> std::vector<MenuItem> {
        if (!slot) return {};
        auto row = [&](const char* label, EyeSource v) {
            return leaf_sel(label, [slot, v]{ *slot = v; }, [slot, v]{ return *slot == v; });
        };
        return { row("Left CSI",  EyeSource::CSI_LEFT),
                 row("Right CSI", EyeSource::CSI_RIGHT),
                 row("USB Cam 1", EyeSource::USB1),
                 row("USB Cam 2", EyeSource::USB2),
                 row("USB Cam 3", EyeSource::USB3) };
    };

    std::vector<MenuItem> multicam_menu;
    if (multicam_layout) {
        multicam_menu.push_back(toggle("Enable Multi-Cam Layout",
            [multicam_layout]{ return *multicam_layout; },
            [multicam_layout](bool v){ *multicam_layout = v; }));
        if (multicam_top_a)
            multicam_menu.push_back(with_desc(
                submenu("Left Eye Top Camera", make_mc_src_menu(multicam_top_a)),
                "Which camera fills the top half of the LEFT eye (default Left CSI)."));
        if (multicam_top_b)
            multicam_menu.push_back(with_desc(
                submenu("Right Eye Top Camera", make_mc_src_menu(multicam_top_b)),
                "Which camera fills the top half of the RIGHT eye (default Right CSI)."));
        if (multicam_usb_a)
            multicam_menu.push_back(with_desc(
                submenu("Left Eye Bottom Camera", make_mc_src_menu(multicam_usb_a)),
                "Which camera fills the bottom half of the LEFT eye (default USB 1)."));
        if (multicam_usb_b)
            multicam_menu.push_back(with_desc(
                submenu("Right Eye Bottom Camera", make_mc_src_menu(multicam_usb_b)),
                "Which camera fills the bottom half of the RIGHT eye (default USB 2)."));
    }

    std::vector<MenuItem> main_cameras_menu = {
        submenu("Left Eye Source",  make_eye_source_menu(left_eye_src)),
        submenu("Right Eye Source", make_eye_source_menu(right_eye_src)),
        with_desc(submenu("Multi-Cam Layout", std::move(multicam_menu)),
            "Each eye stacks a top + bottom camera (full width); the two eyes use "
            "independent sources, so side-by-side shows four distinct feeds with no "
            "duplicates. Each of the four slots picks any CSI or USB camera. Set CSI "
            "rotation to 90\xc2\xb0 under Left/Right Camera > Rotation if needed."),
        with_panel(
            with_desc(submenu("Raw View", std::move(raw_view_menu)),
                "Pass the camera feed straight through. Toggle Enable to show it; "
                "Position places each eye. The preview at right shows it live."),
            "Raw View Preview", eye_pos_preview),
        toggle("Swap Cameras",
            [&state]{ return state.cameras_swapped; },
            [&state](bool v){ state.cameras_swapped = v; }),
        with_panel(
            submenu("Resolution", std::move(resolution_presets)),
            "Camera Resolution",
            [&state, menu_sys_pp]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                int w = state.camera_resolution.width;
                int h = state.camera_resolution.height;
                if (w <= 0 || h <= 0) { w = 1280; h = 800; }
                float ar = float(w) / float(h);

                char buf[64];
                snprintf(buf, sizeof(buf), "%d x %d", w, h);
                dl->AddText({ o.x, o.y }, IM_COL32(255, 255, 255, 235), buf);
                snprintf(buf, sizeof(buf), "Aspect: %.2f:1 (~%s)",
                         ar,
                         (ar > 1.74f && ar < 1.80f) ? "16:9"
                         : (ar > 1.30f && ar < 1.36f) ? "4:3"
                         : (ar > 1.58f && ar < 1.62f) ? "16:10"
                         : "custom");
                dl->AddText({ o.x, o.y + 18.f }, IM_COL32(200, 200, 200, 220), buf);

                // Hint list — common Raspberry Pi camera modules grouped by AR.
                const float lh = 14.f;
                ImVec2 p{ o.x, o.y + 44.f };
                dl->AddText(p, (accent & 0x00FFFFFFu) | (200u << 24),
                            "PI CAMERA NATIVE AR");
                p.y += lh + 2.f;
                struct Row { const char* ar; const char* mods; };
                static const Row rows[] = {
                    { "4:3",  "IMX219 v2, IMX477 HQ, IMX708 v3 (bin)" },
                    { "16:9", "IMX477 HQ, IMX708 v3 (native)" },
                    { "1:1",  "IMX296, IMX290 (crop)" },
                };
                for (auto& r : rows) {
                    char line[96];
                    snprintf(line, sizeof(line), "%-5s  %s", r.ar, r.mods);
                    dl->AddText(p, IM_COL32(190, 190, 190, 200), line);
                    p.y += lh;
                }
            }),
        with_panel(
            with_desc(submenu("Digital Zoom", std::move(zoom_menu)),
                  "Magnify both eyes equally and choose where the crop is centered. "
                  "Higher zoom shows less of the scene at greater detail. Preview at right."),
            "Digital Zoom Preview", digital_zoom_preview),
        with_panel(
            with_desc(submenu("Mirror Crop", std::move(mirror_crop_menu)),
                "Zoom and pan each eye inward (nose-side crop) for a monocular/assistive "
                "setup. The preview at right shows the crop window live."),
            "Mirror Crop Preview",
            [&state, cameras, menu_sys_pp, draw_frame_with_crop]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                const float aspect = (cameras && cameras->current_height() > 0)
                    ? float(cameras->current_width()) / float(cameras->current_height())
                    : 4.0f / 3.0f;

                // Crop window: width and height = 1/zoom of the source frame.
                const float zoom = std::max(1.0f, state.mirror_crop.zoom);
                const float crop_w = 1.0f / zoom;
                const float crop_h = 1.0f / zoom;
                float crop_y0;
                switch (state.mirror_crop.vertical) {
                    case CropVertical::Top:    crop_y0 = 0.0f;                 break;
                    case CropVertical::Middle: crop_y0 = (1.0f - crop_h) * 0.5f; break;
                    case CropVertical::Bottom: crop_y0 = 1.0f - crop_h;        break;
                }
                const float bias = state.mirror_crop.inner_bias;  // 0.0 – 0.40
                // Left eye crop biased toward the right (nose); right eye crop
                // biased toward the left.  bias=0 → centered.
                auto crop_x0 = [&](bool right) {
                    float cx = right ? (0.5f - bias) : (0.5f + bias);
                    return cx - crop_w * 0.5f;
                };

                const bool single = state.cam_single.enabled;
                if (single) {
                    bool right = state.cam_single.use_right;
                    float x0 = crop_x0(right);
                    draw_frame_with_crop(dl, o, sz, aspect, accent,
                                         x0, crop_y0, x0 + crop_w, crop_y0 + crop_h);
                    dl->AddText({ o.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200),
                                right ? "Right camera" : "Left camera");
                } else {
                    ImVec2 lsz{ sz.x * 0.5f - 6.f, sz.y - 16.f };
                    ImVec2 lpos{ o.x, o.y };
                    ImVec2 rpos{ o.x + sz.x * 0.5f + 6.f, o.y };
                    float lx0 = crop_x0(false);
                    float rx0 = crop_x0(true);
                    draw_frame_with_crop(dl, lpos, lsz, aspect, accent,
                                         lx0, crop_y0, lx0 + crop_w, crop_y0 + crop_h);
                    draw_frame_with_crop(dl, rpos, lsz, aspect, accent,
                                         rx0, crop_y0, rx0 + crop_w, crop_y0 + crop_h);
                    dl->AddText({ lpos.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200), "Left");
                    dl->AddText({ rpos.x, o.y + sz.y - 14.f },
                                IM_COL32(200, 200, 200, 200), "Right");
                }
            }),
        with_panel(
            with_desc(submenu("Single Camera", std::move(single_cam_menu)),
                "Show ONE camera filling a region of the screen (full, or a half) instead "
                "of the stereo pair. Choose the camera and anchor; preview at right."),
            "Single Camera Preview", single_cam_preview),
        submenu(left_label,         std::move(left_cam_menu)),
        submenu(right_label,        std::move(right_cam_menu)),
        with_desc([&]{
            // Recover a CSI sensor that came up dark/wedged at boot without a
            // full reboot: tears down + re-enumerates + restarts both cameras.
            MenuItem m; m.type = MenuItemType::LEAF; m.label = "Reinitialize CSI Cameras";
            m.label_fn = [cameras]{
                std::string s = "Reinitialize CSI  [L:";
                s += cameras->owl_left_ok()  ? "ok" : "\xE2\x80\x94";
                s += " R:"; s += cameras->owl_right_ok() ? "ok" : "\xE2\x80\x94";
                return s + "]";
            };
            m.action = [cameras, &state]{
                const bool ok = cameras->reinit_owls();
                const bool lok = cameras->owl_left_ok(), rok = cameras->owl_right_ok();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "CSI cameras reinitialized" : "CSI reinit: no camera found";
                char b[64]; snprintf(b, sizeof(b), "Left %s  \xC2\xB7  Right %s",
                                     lok ? "OK" : "\xE2\x80\x94", rok ? "OK" : "\xE2\x80\x94");
                n.body = b; n.auto_dismiss_s = 5.f;
                state.notifs.push(std::move(n));
            };
            return m;
        }(),
            "Re-enumerate and restart the CSI (OWLsight) cameras \xE2\x80\x94 recovers an "
            "eye that came up dark/wedged at boot, without rebooting. Briefly blacks "
            "both feeds while it re-acquires."),
        submenu("Low-Light Mode",   std::move(nv_menu)),
        submenu("Autofocus Both",   std::move(af_both_menu)),
        submenu("Capture Photo",    std::move(capture_menu)),
        submenu("Record Video",     std::move(video_menu)),
        submenu("QR Scan",          std::move(qr_menu)),
    };

    // ── Headset controls ──────────────────────────────────────────────────────
    auto xr_gaze = std::make_shared<bool>(false);   // local mirror of the mode state
    auto xr_3d   = std::make_shared<bool>(false);
    std::vector<MenuItem> headset_menu = {
        with_desc(slider("Electrochromic Transparency", 0.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_dimming); },
            [xr, &state](float v){
                state.xr_dimming = static_cast<int>(v);
                if (xr) xr->set_dimming(static_cast<int>(v));
            }),
            "Darken the glasses' electrochromic film (0 = clear see-through, 9 = "
            "fully dimmed) so the display stands out against bright surroundings."),
        with_desc(slider("HUD Brightness", 1.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_hud_brightness); },
            [xr, &state](float v){
                state.xr_hud_brightness = static_cast<int>(v);
                if (xr) xr->set_hud_brightness(static_cast<int>(v));
            }),
            "Brightness of the glasses' on-board HUD layer."),
        with_desc(slider("Backlight Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
            "Display panel / backlight brightness."),
        with_desc(leaf("Recenter Display", [xr]{ if (xr) xr->recenter_tracking(); }),
            "Re-center the head-tracked view so straight-ahead is forward right now."),
        with_desc(toggle("Gaze Lock (0-DoF Mode)",
            [xr_gaze]{ return *xr_gaze; },
            [xr, xr_gaze](bool v){ *xr_gaze = v; if (xr) xr->toggle_gaze_lock(); }),
            "Lock the image to the glasses (0-DoF) instead of head-tracking it \xe2\x80\x94 "
            "the HUD stays put as you look around."),
        with_desc(toggle("3D Side-by-Side",
            [xr_3d]{ return *xr_3d; },
            [xr, xr_3d](bool v){ *xr_3d = v; if (xr) xr->set_3d_mode(v); }),
            "Switch the glasses panel to 3D side-by-side stereo mode."),
    };

    // ── Audio controls ────────────────────────────────────────────────────────
    // Beamforming / noise suppression handled by RP2350; CM5 controls output + volume.
    std::vector<MenuItem> output_menu = {
        leaf_sel("VITURE",     [audio]{ if (audio) audio->set_output(AudioOutput::VITURE);     }, [&state]{ return state.audio.output == 0; }),
        leaf_sel("Headphones", [audio]{ if (audio) audio->set_output(AudioOutput::HEADPHONES); }, [&state]{ return state.audio.output == 1; }),
        leaf_sel("HDMI",       [audio]{ if (audio) audio->set_output(AudioOutput::HDMI);       }, [&state]{ return state.audio.output == 2; }),
    };

    std::vector<MenuItem> audio_menu = {
        toggle("Audio",
            [audio]{ return audio ? audio->is_enabled() : false; },
            [audio](bool v){ if (audio) audio->set_enabled(v); }),
        slider("Volume", 25.f, 150.f, 5.f, " %",
            [audio]{ return audio ? audio->get_master_gain() * 100.f : 100.f; },
            [audio](float v){ if (audio) audio->set_master_gain(v / 100.f); }),
        submenu("Output", std::move(output_menu)),
    };

    // ── Overlay position / size helpers ───────────────────────────────────────

    // Snap position presets — sets anchor_x/y and resets any pan offset.
    auto make_position_items = [](OverlayConfig* cfg) {
        auto snap = [cfg](float ax, float ay){
            cfg->anchor_x = ax; cfg->anchor_y = ay;
            cfg->pan_x = 0.f;   cfg->pan_y = 0.f;
        };
        auto at   = [cfg](float ax, float ay){
            return std::abs(cfg->anchor_x - ax) < 0.01f &&
                   std::abs(cfg->anchor_y - ay) < 0.01f;
        };
        std::vector<MenuItem> nudge = {
            leaf("Left  -10px", [cfg]{ cfg->pan_x -= 10.f; }),
            leaf("Right +10px", [cfg]{ cfg->pan_x += 10.f; }),
            leaf("Up    -10px", [cfg]{ cfg->pan_y -= 10.f; }),
            leaf("Down  +10px", [cfg]{ cfg->pan_y += 10.f; }),
            leaf("Left  -50px", [cfg]{ cfg->pan_x -= 50.f; }),
            leaf("Right +50px", [cfg]{ cfg->pan_x += 50.f; }),
            leaf("Up    -50px", [cfg]{ cfg->pan_y -= 50.f; }),
            leaf("Down  +50px", [cfg]{ cfg->pan_y += 50.f; }),
            leaf("Reset Nudge", [cfg]{ cfg->pan_x = 0.f; cfg->pan_y = 0.f; }),
        };
        return std::vector<MenuItem>{
            leaf_sel("Top Left",      [snap]{ snap(0.0f, 0.0f); }, [cfg, at]{ return at(0.0f, 0.0f); }),
            leaf_sel("Top Center",    [snap]{ snap(0.5f, 0.0f); }, [cfg, at]{ return at(0.5f, 0.0f); }),
            leaf_sel("Top Right",     [snap]{ snap(1.0f, 0.0f); }, [cfg, at]{ return at(1.0f, 0.0f); }),
            leaf_sel("Center Left",   [snap]{ snap(0.0f, 0.5f); }, [cfg, at]{ return at(0.0f, 0.5f); }),
            leaf_sel("Center",        [snap]{ snap(0.5f, 0.5f); }, [cfg, at]{ return at(0.5f, 0.5f); }),
            leaf_sel("Center Right",  [snap]{ snap(1.0f, 0.5f); }, [cfg, at]{ return at(1.0f, 0.5f); }),
            leaf_sel("Bottom Left",   [snap]{ snap(0.0f, 1.0f); }, [cfg, at]{ return at(0.0f, 1.0f); }),
            leaf_sel("Bottom Center", [snap]{ snap(0.5f, 1.0f); }, [cfg, at]{ return at(0.5f, 1.0f); }),
            leaf_sel("Bottom Right",  [snap]{ snap(1.0f, 1.0f); }, [cfg, at]{ return at(1.0f, 1.0f); }),
            submenu("Nudge", std::move(nudge)),
        };
    };

    auto make_size_slider = [](std::string lbl, OverlayConfig* cfg) -> MenuItem {
        return slider(std::move(lbl), 15.f, 60.f, 5.f, " %",
            [cfg]{ return cfg->size * 100.f; },
            [cfg](float v){ cfg->size = v / 100.f; });
    };

    auto make_rotation_items = [](OverlayConfig* cfg) {
        using R = OverlayConfig::Rotation;
        return std::vector<MenuItem>{
            leaf_sel("Landscape",         [cfg]{ cfg->rotation = R::Landscape;        }, [cfg]{ return cfg->rotation == R::Landscape;        }),
            leaf_sel("Portrait",          [cfg]{ cfg->rotation = R::Portrait;          }, [cfg]{ return cfg->rotation == R::Portrait;          }),
            leaf_sel("Landscape Flipped", [cfg]{ cfg->rotation = R::LandscapeFlipped; }, [cfg]{ return cfg->rotation == R::LandscapeFlipped; }),
            leaf_sel("Portrait Flipped",  [cfg]{ cfg->rotation = R::PortraitFlipped;  }, [cfg]{ return cfg->rotation == R::PortraitFlipped;  }),
        };
    };

    // Schematic preview of where / how big / which orientation the PiP sits on
    // screen — like the Digital Zoom preview, no live feed (the real on-screen PiP
    // already updates live as Position/Size/Rotation change).
    auto make_pip_placement_panel = [menu_sys_pp](OverlayConfig* cfg) -> MenuContextPanelDraw {
        return [cfg, menu_sys_pp](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                ? (*menu_sys_pp)->accent_color() : IM_COL32(255, 255, 255, 255);
            // 16:9 screen outline, letterboxed into the content rect (leave a text line).
            const float availH = sz.y - 16.f;
            float sw = sz.x, sh = availH;
            const float screenAR = 16.f / 9.f;
            if (sw / sh > screenAR) sw = sh * screenAR; else sh = sw / screenAR;
            const ImVec2 smin{ o.x + (sz.x - sw) * 0.5f, o.y + (availH - sh) * 0.5f };
            const ImVec2 smax{ smin.x + sw, smin.y + sh };
            dl->AddRectFilled(smin, smax, IM_COL32(20, 25, 30, 180), 3.f);
            dl->AddRect(smin, smax, IM_COL32(120, 130, 140, 200), 3.f, 0, 1.5f);
            // PiP rect: height = size fraction of screen height; aspect by rotation.
            const bool portrait = (cfg->rotation == OverlayConfig::Rotation::Portrait ||
                                   cfg->rotation == OverlayConfig::Rotation::PortraitFlipped);
            const float pratio = portrait ? (9.f / 16.f) : (16.f / 9.f);
            float ph = sh * std::clamp(cfg->size, 0.05f, 1.0f);
            float pw = ph * pratio;
            if (pw > sw) { pw = sw; ph = pw / pratio; }
            // anchor_x/y are the PiP centre fraction (matches hud_renderer); clamp inside.
            float cx = smin.x + sw * cfg->anchor_x;
            float cy = smin.y + sh * cfg->anchor_y;
            ImVec2 pmin{ cx - pw * 0.5f, cy - ph * 0.5f };
            pmin.x = std::clamp(pmin.x, smin.x, smax.x - pw);
            pmin.y = std::clamp(pmin.y, smin.y, smax.y - ph);
            const ImVec2 pmax{ pmin.x + pw, pmin.y + ph };
            dl->AddRectFilled(pmin, pmax, (accent & 0x00FFFFFFu) | (60u << 24), 2.f);
            dl->AddRect(pmin, pmax, (accent & 0x00FFFFFFu) | (235u << 24), 2.f, 0, 2.f);
            char buf[64];
            snprintf(buf, sizeof(buf), "Size %.0f%%   %s", cfg->size * 100.f,
                     portrait ? "Portrait" : "Landscape");
            dl->AddText({ o.x, o.y + sz.y - 14.f }, IM_COL32(200, 200, 200, 200), buf);
        };
    };

    // Live-feed preview for the image settings (Brightness / Exposure / White
    // Balance / Resolution).  Sets *usb_preview_req = cam while visible so the
    // render loop opens the stream, hides the on-screen PiP, and keeps the texture
    // uploaded; draws the actual frame plus an info line read from `info`.
    auto make_usb_live_panel = [cameras, menu_sys_pp, usb_preview_req](
            int cam, GLuint* tex,
            std::function<std::string()> info) -> MenuContextPanelDraw {
        return [cameras, menu_sys_pp, usb_preview_req, cam, tex, info]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            if (usb_preview_req) *usb_preview_req = cam;
            const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                ? (*menu_sys_pp)->accent_color() : IM_COL32(255, 255, 255, 255);
            // Feed area on top, two text lines reserved below.
            const float textH = 34.f;
            const ImVec2 fa_o{ o.x, o.y };
            const ImVec2 fa_s{ sz.x, sz.y - textH };
            float ar = 16.f / 9.f;
            if (cameras) {
                UsbCamConfig c = (cam == 1) ? cameras->usb1_cfg()
                               : (cam == 2) ? cameras->usb2_cfg()
                                            : cameras->usb3_cfg();
                if (c.width > 0 && c.height > 0) ar = float(c.width) / float(c.height);
            }
            float fw = fa_s.x, fh = fa_s.y;
            if (fw / fh > ar) fw = fh * ar; else fh = fw / ar;
            const ImVec2 fmin{ fa_o.x + (fa_s.x - fw) * 0.5f, fa_o.y + (fa_s.y - fh) * 0.5f };
            const ImVec2 fmax{ fmin.x + fw, fmin.y + fh };
            const GLuint t = tex ? *tex : 0u;
            if (t) {
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(t)),
                             fmin, fmax);
            } else {
                dl->AddRectFilled(fmin, fmax, IM_COL32(20, 25, 30, 180), 3.f);
                dl->AddText({ fmin.x + 8.f, fmin.y + 8.f },
                            IM_COL32(200, 200, 200, 200), "Starting preview...");
            }
            dl->AddRect(fmin, fmax, (accent & 0x00FFFFFFu) | (235u << 24), 3.f, 0, 2.f);
            if (info) {
                const std::string s = info();
                dl->AddText({ o.x, o.y + sz.y - textH + 2.f },
                            IM_COL32(210, 210, 210, 220), s.c_str());
            }
        };
    };

    // ── USB camera menus ──────────────────────────────────────────────────────
    // Helper: build a "Resolution" submenu for one USB camera slot.
    // If the stream is currently open, close it and reopen with the new dimensions.
    auto make_resolution_items = [cameras](
        std::function<UsbCamConfig()>          get_cfg,
        std::function<void(UsbCamConfig)>      set_cfg,
        std::function<bool()>                  is_open,
        std::function<void()>                  close_fn,
        std::function<void()>                  open_fn)
    {
        auto set_res = [=](int w, int h) {
            if (!cameras) return;
            UsbCamConfig c = get_cfg(); c.width = w; c.height = h; set_cfg(c);
            if (is_open()) {
                close_fn();
                std::thread([open_fn]{ open_fn(); }).detach();
            }
        };
        return std::vector<MenuItem>{
            leaf_sel("High  1280x720", [set_res]{ set_res(1280, 720); },
                [get_cfg]{ return get_cfg().width == 1280 && get_cfg().height == 720; }),
            leaf_sel("Med    960x540", [set_res]{ set_res( 960, 540); },
                [get_cfg]{ return get_cfg().width ==  960 && get_cfg().height == 540; }),
            leaf_sel("Low    640x360", [set_res]{ set_res( 640, 360); },
                [get_cfg]{ return get_cfg().width ==  640 && get_cfg().height == 360; }),
        };
    };

    // Scan for available V4L2 capture devices once at menu-build time.
    // Results are used to populate each slot's "Select Device" submenu.
    auto usb_devs = cameras ? cameras->list_usb_devices()
                            : std::vector<CameraManager::UsbDeviceInfo>{};

    // Helper: build a "Select Device" submenu for one USB camera slot.
    // get_path  — returns the slot's current device path (for the checkmark).
    // assign_fn — called with the chosen path when the user selects an item.
    auto make_dev_select = [&](
        std::function<std::string()>          get_path,
        std::function<void(const std::string&)> assign_fn)
    {
        std::vector<MenuItem> items;
        items.push_back(leaf_sel("(none)",
            [assign_fn]{ assign_fn(""); },
            [get_path]{ return get_path().empty(); }));
        for (const auto& d : usb_devs) {
            // Label: "video17 — Logitech C920 HD Pro Webcam"
            std::string label = std::filesystem::path(d.path).filename().string()
                                + " \xe2\x80\x94 " + d.name;
            items.push_back(leaf_sel(std::move(label),
                [assign_fn, p = d.path]{ assign_fn(p); },
                [get_path,  p = d.path]{ return get_path() == p; }));
        }
        if (usb_devs.empty())
            items.push_back(leaf("(no devices found)", []{}));
        return items;
    };

    std::vector<MenuItem> usb1_pip_menu = {
        with_desc(with_panel(submenu("Window Position", make_position_items(pip_cfg1)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg1)),
                   "Snap the picture-in-picture window to a screen anchor, or nudge it pixel by pixel. Preview at right."),
        with_desc(with_panel(submenu("Window Size", std::vector<MenuItem>{ make_size_slider("Size", pip_cfg1) }),
                   "PiP Placement", make_pip_placement_panel(pip_cfg1)),
                   "Scale the PiP window as a fraction of screen height. Preview at right."),
        with_desc(with_panel(submenu("Window Rotation", make_rotation_items(pip_cfg1)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg1)),
                   "Rotate the PiP window for landscape or portrait mounting. Preview at right."),
        with_desc(with_panel(submenu("Brightness", std::vector<MenuItem>{
            toggle("Auto Brightness",
                [cameras]{ return cameras && cameras->usb1_cfg().auto_brightness; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.auto_brightness = v; cameras->update_usb1_cfg(c); }),
            slider("Auto Brightness Target", 40.f, 220.f, 5.f, "",
                [cameras]{ return cameras ? cameras->usb1_cfg().auto_brightness_target : 100.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.auto_brightness_target = v; cameras->update_usb1_cfg(c); }),
            slider("Manual Brightness", 50.f, 300.f, 25.f, " %",
                [cameras]{ return cameras ? cameras->usb1_brightness() * 100.f : 100.f; },
                [cameras](float v){ if (cameras) cameras->set_usb1_brightness(v / 100.f); }),
        }), "Brightness", make_usb_live_panel(1, tex_usb1, [cameras]{
            if (!cameras) return std::string("Brightness");
            UsbCamConfig c = cameras->usb1_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s   Target %.0f\nManual %.0f%%",
                c.auto_brightness ? "ON" : "OFF", c.auto_brightness_target, cameras->usb1_brightness() * 100.f);
            return std::string(b);
        })), "Auto-brightness holds a target image luma; or disable it and set a fixed manual gain. Live feed at right."),
        with_desc(with_panel(submenu("Exposure", std::vector<MenuItem>{
            toggle("Auto Exposure",
                [cameras]{ return !cameras || cameras->usb1_cfg().auto_exposure; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.auto_exposure = v; cameras->update_usb1_cfg(c); cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1); }),
            slider("Exposure Time", 1.f, 5000.f, 10.f, "",
                [cameras]{ return cameras ? (float)cameras->usb1_cfg().exposure_time : 157.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.exposure_time = (int)v; cameras->update_usb1_cfg(c); cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v); }),
        }), "Exposure", make_usb_live_panel(1, tex_usb1, [cameras]{
            if (!cameras) return std::string("Exposure");
            UsbCamConfig c = cameras->usb1_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTime %d", c.auto_exposure ? "ON" : "OFF", c.exposure_time);
            return std::string(b);
        })), "Auto-exposure, or a fixed exposure time. Lower time is darker with less motion blur. Live feed at right."),
        with_desc(with_panel(submenu("White Balance", std::vector<MenuItem>{
            toggle("Auto White Balance",
                [cameras]{ return !cameras || cameras->usb1_cfg().auto_wb; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.auto_wb = v; cameras->update_usb1_cfg(c); cameras->set_usb1_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0); }),
            slider("Manual White Balance", 2800.f, 6500.f, 100.f, "K",
                [cameras]{ return cameras ? (float)cameras->usb1_cfg().wb_temp : 4600.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.wb_temp = (int)v; cameras->update_usb1_cfg(c); cameras->set_usb1_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v); }),
        }), "White Balance", make_usb_live_panel(1, tex_usb1, [cameras]{
            if (!cameras) return std::string("White Balance");
            UsbCamConfig c = cameras->usb1_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTemp %dK", c.auto_wb ? "ON" : "OFF", c.wb_temp);
            return std::string(b);
        })), "Auto white balance, or a fixed colour temperature (lower K is warmer). Live feed at right."),
        with_desc(with_panel(submenu("Resolution and Framerate", std::vector<MenuItem>{
            submenu("Resolution", make_resolution_items(
                [cameras]{ return cameras->usb1_cfg(); },
                [cameras](UsbCamConfig c){ cameras->update_usb1_cfg(c); },
                [cameras]{ return cameras && cameras->usb1_ok(); },
                [cameras]{ cameras->close_usb1(); },
                [cameras]{ cameras->open_usb1(); })),
            toggle("Dynamic Framerate",
                [cameras]{ return cameras && cameras->usb1_cfg().dynamic_framerate; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb1_cfg(); c.dynamic_framerate = v; cameras->update_usb1_cfg(c); cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0); }),
        }), "Resolution", make_usb_live_panel(1, tex_usb1, [cameras]{
            if (!cameras) return std::string("Resolution");
            UsbCamConfig c = cameras->usb1_cfg();
            char b[96]; snprintf(b, sizeof(b), "%dx%d\nDynamic FPS %s", c.width, c.height, c.dynamic_framerate ? "ON" : "OFF");
            return std::string(b);
        })), "Capture resolution and dynamic framerate (lets the driver drop FPS in low light). Live feed at right."),
    };
    std::vector<MenuItem> usb1_device_menu = {
        submenu("Device Select", make_dev_select(
            [cameras]{ return cameras ? cameras->usb1_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb1(p); })),
        leaf("Scan for Devices", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb1();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb1 = ok;
            }
        }),
    };
    std::vector<MenuItem> usb_cam1_menu = {
        toggle("Open Camera",
            [cameras]{ return cameras && cameras->usb1_ok(); },
            [pip_cam1_overlay](bool v){ *pip_cam1_overlay = v; }),
        submenu("PiP Window Settings", std::move(usb1_pip_menu)),
        submenu("Device Options",      std::move(usb1_device_menu)),
    };

    std::vector<MenuItem> usb2_pip_menu = {
        with_desc(with_panel(submenu("Window Position", make_position_items(pip_cfg2)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg2)),
                   "Snap the picture-in-picture window to a screen anchor, or nudge it pixel by pixel. Preview at right."),
        with_desc(with_panel(submenu("Window Size", std::vector<MenuItem>{ make_size_slider("Size", pip_cfg2) }),
                   "PiP Placement", make_pip_placement_panel(pip_cfg2)),
                   "Scale the PiP window as a fraction of screen height. Preview at right."),
        with_desc(with_panel(submenu("Window Rotation", make_rotation_items(pip_cfg2)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg2)),
                   "Rotate the PiP window for landscape or portrait mounting. Preview at right."),
        with_desc(with_panel(submenu("Brightness", std::vector<MenuItem>{
            toggle("Auto Brightness",
                [cameras]{ return cameras && cameras->usb2_cfg().auto_brightness; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.auto_brightness = v; cameras->update_usb2_cfg(c); }),
            slider("Auto Brightness Target", 40.f, 220.f, 5.f, "",
                [cameras]{ return cameras ? cameras->usb2_cfg().auto_brightness_target : 100.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.auto_brightness_target = v; cameras->update_usb2_cfg(c); }),
            slider("Manual Brightness", 50.f, 300.f, 25.f, " %",
                [cameras]{ return cameras ? cameras->usb2_brightness() * 100.f : 100.f; },
                [cameras](float v){ if (cameras) cameras->set_usb2_brightness(v / 100.f); }),
        }), "Brightness", make_usb_live_panel(2, tex_usb2, [cameras]{
            if (!cameras) return std::string("Brightness");
            UsbCamConfig c = cameras->usb2_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s   Target %.0f\nManual %.0f%%",
                c.auto_brightness ? "ON" : "OFF", c.auto_brightness_target, cameras->usb2_brightness() * 100.f);
            return std::string(b);
        })), "Auto-brightness holds a target image luma; or disable it and set a fixed manual gain. Live feed at right."),
        with_desc(with_panel(submenu("Exposure", std::vector<MenuItem>{
            toggle("Auto Exposure",
                [cameras]{ return !cameras || cameras->usb2_cfg().auto_exposure; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.auto_exposure = v; cameras->update_usb2_cfg(c); cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1); }),
            slider("Exposure Time", 1.f, 5000.f, 10.f, "",
                [cameras]{ return cameras ? (float)cameras->usb2_cfg().exposure_time : 157.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.exposure_time = (int)v; cameras->update_usb2_cfg(c); cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v); }),
        }), "Exposure", make_usb_live_panel(2, tex_usb2, [cameras]{
            if (!cameras) return std::string("Exposure");
            UsbCamConfig c = cameras->usb2_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTime %d", c.auto_exposure ? "ON" : "OFF", c.exposure_time);
            return std::string(b);
        })), "Auto-exposure, or a fixed exposure time. Lower time is darker with less motion blur. Live feed at right."),
        with_desc(with_panel(submenu("White Balance", std::vector<MenuItem>{
            toggle("Auto White Balance",
                [cameras]{ return !cameras || cameras->usb2_cfg().auto_wb; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.auto_wb = v; cameras->update_usb2_cfg(c); cameras->set_usb2_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0); }),
            slider("Manual White Balance", 2800.f, 6500.f, 100.f, "K",
                [cameras]{ return cameras ? (float)cameras->usb2_cfg().wb_temp : 4600.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.wb_temp = (int)v; cameras->update_usb2_cfg(c); cameras->set_usb2_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v); }),
        }), "White Balance", make_usb_live_panel(2, tex_usb2, [cameras]{
            if (!cameras) return std::string("White Balance");
            UsbCamConfig c = cameras->usb2_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTemp %dK", c.auto_wb ? "ON" : "OFF", c.wb_temp);
            return std::string(b);
        })), "Auto white balance, or a fixed colour temperature (lower K is warmer). Live feed at right."),
        with_desc(with_panel(submenu("Resolution and Framerate", std::vector<MenuItem>{
            submenu("Resolution", make_resolution_items(
                [cameras]{ return cameras->usb2_cfg(); },
                [cameras](UsbCamConfig c){ cameras->update_usb2_cfg(c); },
                [cameras]{ return cameras && cameras->usb2_ok(); },
                [cameras]{ cameras->close_usb2(); },
                [cameras]{ cameras->open_usb2(); })),
            toggle("Dynamic Framerate",
                [cameras]{ return cameras && cameras->usb2_cfg().dynamic_framerate; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb2_cfg(); c.dynamic_framerate = v; cameras->update_usb2_cfg(c); cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0); }),
        }), "Resolution", make_usb_live_panel(2, tex_usb2, [cameras]{
            if (!cameras) return std::string("Resolution");
            UsbCamConfig c = cameras->usb2_cfg();
            char b[96]; snprintf(b, sizeof(b), "%dx%d\nDynamic FPS %s", c.width, c.height, c.dynamic_framerate ? "ON" : "OFF");
            return std::string(b);
        })), "Capture resolution and dynamic framerate (lets the driver drop FPS in low light). Live feed at right."),
    };
    std::vector<MenuItem> usb2_device_menu = {
        submenu("Device Select", make_dev_select(
            [cameras]{ return cameras ? cameras->usb2_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb2(p); })),
        leaf("Scan for Devices", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb2();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb2 = ok;
            }
        }),
    };
    std::vector<MenuItem> usb_cam2_menu = {
        toggle("Open Camera",
            [cameras]{ return cameras && cameras->usb2_ok(); },
            [pip_cam2_overlay](bool v){ *pip_cam2_overlay = v; }),
        submenu("PiP Window Settings", std::move(usb2_pip_menu)),
        submenu("Device Options",      std::move(usb2_device_menu)),
    };

    std::vector<MenuItem> usb3_pip_menu = {
        with_desc(with_panel(submenu("Window Position", make_position_items(pip_cfg3)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg3)),
                   "Snap the picture-in-picture window to a screen anchor, or nudge it pixel by pixel. Preview at right."),
        with_desc(with_panel(submenu("Window Size", std::vector<MenuItem>{ make_size_slider("Size", pip_cfg3) }),
                   "PiP Placement", make_pip_placement_panel(pip_cfg3)),
                   "Scale the PiP window as a fraction of screen height. Preview at right."),
        with_desc(with_panel(submenu("Window Rotation", make_rotation_items(pip_cfg3)),
                   "PiP Placement", make_pip_placement_panel(pip_cfg3)),
                   "Rotate the PiP window for landscape or portrait mounting. Preview at right."),
        with_desc(with_panel(submenu("Brightness", std::vector<MenuItem>{
            toggle("Auto Brightness",
                [cameras]{ return cameras && cameras->usb3_cfg().auto_brightness; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.auto_brightness = v; cameras->update_usb3_cfg(c); }),
            slider("Auto Brightness Target", 40.f, 220.f, 5.f, "",
                [cameras]{ return cameras ? cameras->usb3_cfg().auto_brightness_target : 100.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.auto_brightness_target = v; cameras->update_usb3_cfg(c); }),
            slider("Manual Brightness", 50.f, 300.f, 25.f, " %",
                [cameras]{ return cameras ? cameras->usb3_brightness() * 100.f : 100.f; },
                [cameras](float v){ if (cameras) cameras->set_usb3_brightness(v / 100.f); }),
        }), "Brightness", make_usb_live_panel(3, tex_usb3, [cameras]{
            if (!cameras) return std::string("Brightness");
            UsbCamConfig c = cameras->usb3_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s   Target %.0f\nManual %.0f%%",
                c.auto_brightness ? "ON" : "OFF", c.auto_brightness_target, cameras->usb3_brightness() * 100.f);
            return std::string(b);
        })), "Auto-brightness holds a target image luma; or disable it and set a fixed manual gain. Live feed at right."),
        with_desc(with_panel(submenu("Exposure", std::vector<MenuItem>{
            toggle("Auto Exposure",
                [cameras]{ return !cameras || cameras->usb3_cfg().auto_exposure; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.auto_exposure = v; cameras->update_usb3_cfg(c); cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1); }),
            slider("Exposure Time", 1.f, 5000.f, 10.f, "",
                [cameras]{ return cameras ? (float)cameras->usb3_cfg().exposure_time : 157.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.exposure_time = (int)v; cameras->update_usb3_cfg(c); cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v); }),
        }), "Exposure", make_usb_live_panel(3, tex_usb3, [cameras]{
            if (!cameras) return std::string("Exposure");
            UsbCamConfig c = cameras->usb3_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTime %d", c.auto_exposure ? "ON" : "OFF", c.exposure_time);
            return std::string(b);
        })), "Auto-exposure, or a fixed exposure time. Lower time is darker with less motion blur. Live feed at right."),
        with_desc(with_panel(submenu("White Balance", std::vector<MenuItem>{
            toggle("Auto White Balance",
                [cameras]{ return !cameras || cameras->usb3_cfg().auto_wb; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.auto_wb = v; cameras->update_usb3_cfg(c); cameras->set_usb3_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0); }),
            slider("Manual White Balance", 2800.f, 6500.f, 100.f, "K",
                [cameras]{ return cameras ? (float)cameras->usb3_cfg().wb_temp : 4600.f; },
                [cameras](float v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.wb_temp = (int)v; cameras->update_usb3_cfg(c); cameras->set_usb3_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v); }),
        }), "White Balance", make_usb_live_panel(3, tex_usb3, [cameras]{
            if (!cameras) return std::string("White Balance");
            UsbCamConfig c = cameras->usb3_cfg();
            char b[96]; snprintf(b, sizeof(b), "Auto %s\nTemp %dK", c.auto_wb ? "ON" : "OFF", c.wb_temp);
            return std::string(b);
        })), "Auto white balance, or a fixed colour temperature (lower K is warmer). Live feed at right."),
        with_desc(with_panel(submenu("Resolution and Framerate", std::vector<MenuItem>{
            submenu("Resolution", make_resolution_items(
                [cameras]{ return cameras->usb3_cfg(); },
                [cameras](UsbCamConfig c){ cameras->update_usb3_cfg(c); },
                [cameras]{ return cameras && cameras->usb3_ok(); },
                [cameras]{ cameras->close_usb3(); },
                [cameras]{ cameras->open_usb3(); })),
            toggle("Dynamic Framerate",
                [cameras]{ return cameras && cameras->usb3_cfg().dynamic_framerate; },
                [cameras](bool v){ if (!cameras) return; UsbCamConfig c = cameras->usb3_cfg(); c.dynamic_framerate = v; cameras->update_usb3_cfg(c); cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0); }),
        }), "Resolution", make_usb_live_panel(3, tex_usb3, [cameras]{
            if (!cameras) return std::string("Resolution");
            UsbCamConfig c = cameras->usb3_cfg();
            char b[96]; snprintf(b, sizeof(b), "%dx%d\nDynamic FPS %s", c.width, c.height, c.dynamic_framerate ? "ON" : "OFF");
            return std::string(b);
        })), "Capture resolution and dynamic framerate (lets the driver drop FPS in low light). Live feed at right."),
    };
    std::vector<MenuItem> usb3_device_menu = {
        submenu("Device Select", make_dev_select(
            [cameras]{ return cameras ? cameras->usb3_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb3(p); })),
        leaf("Scan for Devices", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb3();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb3 = ok;
            }
        }),
    };
    std::vector<MenuItem> usb_cam3_menu = {
        toggle("Open Camera",
            [cameras]{ return cameras && cameras->usb3_ok(); },
            [pip_cam3_overlay](bool v){ *pip_cam3_overlay = v; }),
        submenu("PiP Window Settings", std::move(usb3_pip_menu)),
        submenu("Device Options",      std::move(usb3_device_menu)),
    };

    // Build USB cam submenu items with visibility predicates so slots without a
    // connected or configured camera are hidden from the menu.
    auto make_usb_cam_item = [&](const char* label, std::vector<MenuItem> menu,
                                  std::function<bool()> vis) -> MenuItem {
        auto item = submenu(label, std::move(menu));
        item.visible_fn = std::move(vis);
        return item;
    };

    std::vector<MenuItem> usb_cameras_menu = {
        make_usb_cam_item("USB Cam 1", std::move(usb_cam1_menu),
            [cameras]{ return cameras && (cameras->usb1_ok() || !cameras->usb1_cfg().device.empty()); }),
        make_usb_cam_item("USB Cam 2", std::move(usb_cam2_menu),
            [cameras]{ return cameras && (cameras->usb2_ok() || !cameras->usb2_cfg().device.empty()); }),
        make_usb_cam_item("USB Cam 3", std::move(usb_cam3_menu),
            [cameras]{ return cameras && (cameras->usb3_ok() || !cameras->usb3_cfg().device.empty()); }),
        toggle("Auto-Reconnect",
            [cameras]{ return cameras &&
                              cameras->usb1_reconnect_enabled() &&
                              cameras->usb2_reconnect_enabled() &&
                              cameras->usb3_reconnect_enabled(); },
            [cameras](bool v){
                if (!cameras) return;
                cameras->set_usb1_reconnect(v);
                cameras->set_usb2_reconnect(v);
                cameras->set_usb3_reconnect(v);
            }),
    };

    std::vector<MenuItem> cameras_menu = {
        submenu("CSI Camera Controls", std::move(main_cameras_menu)),
        submenu("USB Cameras",         std::move(usb_cameras_menu)),
    };

    // ── Android mirror ────────────────────────────────────────────────────────
    std::vector<MenuItem> android_menu = {
        toggle("Mirror",
            [android_mirror]{ return android_mirror->is_running(); },
            [android_mirror](bool v){
                if (v) std::thread([android_mirror]{ android_mirror->start(); }).detach();
                else   android_mirror->stop();
            }),
        toggle("Show Overlay",
            [android_overlay]{ return *android_overlay; },
            [android_overlay](bool v){ *android_overlay = v; }),
        submenu("Position", make_position_items(android_cfg)),
        make_size_slider("Size", android_cfg),
    };

    // ── ProtoTracer (Teensy-driven LED matrix) submenu ────────────────────────
    std::vector<MenuItem> prototracer_inner_menu = {
        submenu("Faces",              std::move(effects)),
        submenu("Color",              std::move(colors)),
        submenu("ProtoTracer Palette", std::move(proto_colors)),
        with_panel(submenu("Animations", std::move(gifs)),
                   "GIF Preview", draw_gif_preview),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
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
                layer["viscosity"]  = L.viscosity;
                layer["pitch_fill"] = L.pitch_fill;
                if (L.bubbles > 0) {
                    layer["bubbles"]     = L.bubbles;
                    layer["bubble_mode"] = L.bubble_mode;
                }
            }
            if (L.effect == "lightning") {
                layer["branches"] = L.branches;
                if (L.arc) layer["arc"] = true;
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
            L.viscosity = jl.value("viscosity", 0.3f);
            L.pitch_fill = jl.value("pitch_fill", 0.0f);
            L.bubbles = jl.value("bubbles", 0);
            L.bubble_mode = jl.value("bubble_mode", std::string("rise"));
            L.arc = jl.value("arc", false);
            L.branches = jl.value("branches", 0.35f);
        }
    };

    // Live Preview — when on, edits in the builder/effect-settings apply
    // continuously. ParticleSystem::set_effect updates params in place when the
    // layer structure is unchanged, so tweaks don't reset the particle sim.
    auto pf_live = std::make_shared<bool>(false);
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
            // Viscosity — only for "water": higher = slower, resists sloshing.
            ([&]{
                MenuItem vis = slider("Viscosity", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->viscosity * 100.f; },
                    [L](float v){ L->viscosity = v / 100.f; });
                vis.visible_fn = [L]{ return L->effect == "water"; };
                return vis;
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
            // Lightning extras — arc mode + fork density (lightning only).
            ([&]{
                MenuItem t = toggle("Arc Mode",
                    [L]{ return L->arc; }, [L](bool v){ L->arc = v; });
                t.visible_fn = [L]{ return L->effect == "lightning"; };
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
    {
        // Legacy Teensy/ProtoTracer single-effect ids (only meaningful on the
        // Teensy face backend) — tucked into their own page.
        const char* pf_effect_names[] = {
            "None","Sparkle","Embers","Rain","Snow","Confetti","Rings","Fireflies",
            "Fire","Aurora","Blizzard","Sonar","Plasma","Celebration","Galaxy","Party",
            "Clouds","Nebula",
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

        std::vector<MenuItem> gdir_items = {
            leaf_sel("Horizontal",
                [G, apply_grad]{ G->direction = "horizontal"; apply_grad(); },
                [G]{ return G->direction != "vertical"; }),
            leaf_sel("Vertical",
                [G, apply_grad]{ G->direction = "vertical"; apply_grad(); },
                [G]{ return G->direction == "vertical"; }),
        };
        grad_items.push_back(submenu("Direction", std::move(gdir_items)));

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

            // Load — pre-allocated 16 slot rows; each one's label_fn pulls the
            // i-th map entry and the action loads it. Hidden when i is past
            // the current map size. Matches the profile-slots pattern.
            constexpr int kLayoutSlots = 16;
            std::vector<MenuItem> load_items;
            for (int i = 0; i < kLayoutSlots; ++i) {
                MenuItem li;
                li.type  = MenuItemType::LEAF;
                li.label = "layout";
                li.label_fn = [nth_name, A, i]{
                    const std::string nm = nth_name(i);
                    if (nm.empty()) return std::string();
                    return (*A == nm) ? (nm + "  *") : nm;
                };
                li.visible_fn = [M, i]{ return i < static_cast<int>(M->size()); };
                li.action = [H, M, A, layout_changed, nth_name, i]{
                    const std::string nm = nth_name(i);
                    if (nm.empty() || *A == nm) return;
                    (*M)[*A] = *H;
                    *A = nm;
                    *H = (*M)[nm];
                    if (layout_changed) layout_changed();
                };
                load_items.push_back(std::move(li));
            }
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

    std::vector<MenuItem> pf_hardware_menu = {
        with_desc(submenu("Backend", std::move(pf_backend_items)),
                  "What LED hardware Protoface paints. Switching tears down "
                  "the running renderer and brings up a new one with the new "
                  "backend; the HUD keeps running through the transition. "
                  "Persists to config.json so the next launch starts here."),
    };
    if (pf_restart_renderer) {
        MenuItem rr = with_desc(leaf("Restart Face Renderer", [pf_restart_renderer, state_ptr]{
            pf_restart_renderer();
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            Notification n; n.type = NotifType::App;
            n.title = "Face renderer restarted";
            n.body  = "Relaunched the HUB75 panel driver";
            n.auto_dismiss_s = 4.f;
            state_ptr->notifs.push(std::move(n));
        }),
            "Kill + relaunch the HUB75 panel pusher to recover the face feed "
            "(e.g. after the live GPIO read stole a panel pin), without "
            "restarting ProtoHUD.");
        rr.visible_fn = [pf_backend_p]{ return !pf_backend_p || *pf_backend_p == "hub75"; };
        pf_hardware_menu.push_back(std::move(rr));
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
        gated(submenu("Effects",        std::move(pf_effects)),  visible_for_hub75),
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
            gi.push_back(toggle("Glitch",
                [G]{ return G->enabled; },
                [G, pf_anim_push](bool v){ G->enabled = v; if (pf_anim_push) pf_anim_push(); }));
            gi.push_back(slider("Intensity", 0.f, 200.f, 5.f, "%",
                [G]{ return static_cast<float>(G->intensity * 100.0); },
                [G, pf_anim_push](float v){ G->intensity = v / 100.0; if (pf_anim_push) pf_anim_push(); }));
            gi.push_back(slider("Burst Rate", 0.f, 3.f, 0.1f, "/s",
                [G]{ return static_cast<float>(G->burst_rate); },
                [G, pf_anim_push](float v){ G->burst_rate = v; if (pf_anim_push) pf_anim_push(); }));
            gi.push_back(slider("Burst Min", 0.02f, 1.f, 0.02f, "s",
                [G]{ return static_cast<float>(G->burst_min); },
                [G, pf_anim_push](float v){ G->burst_min = v; if (pf_anim_push) pf_anim_push(); }));
            gi.push_back(slider("Burst Max", 0.05f, 2.f, 0.05f, "s",
                [G]{ return static_cast<float>(G->burst_max); },
                [G, pf_anim_push](float v){ G->burst_max = v; if (pf_anim_push) pf_anim_push(); }));
            // Per-component amounts (0 = off). Each is an independent variable.
            auto comp = [&](const char* name, double face::GlitchConfig::* member) {
                gi.push_back(slider(name, 0.f, 100.f, 5.f, "%",
                    [G, member]{ return static_cast<float>((G->*member) * 100.0); },
                    [G, member, pf_anim_push](float v){ G->*member = v / 100.0; if (pf_anim_push) pf_anim_push(); }));
            };
            comp("Chromatic Split",    &face::GlitchConfig::chromatic);
            comp("Band Tearing",       &face::GlitchConfig::tearing);
            comp("Block Shuffle",      &face::GlitchConfig::blocks);
            comp("Bitcrush",           &face::GlitchConfig::bitcrush);
            comp("Dropout Bars",       &face::GlitchConfig::dropout);
            comp("Datamosh",           &face::GlitchConfig::datamosh);
            comp("Eyes/Mouth Desync",  &face::GlitchConfig::region_desync);
            comp("Expression Flicker", &face::GlitchConfig::expr_flicker);
            return with_desc(submenu("Glitch", std::move(gi)),
                "Digital glitch corruption of the face. Master Intensity and Burst "
                "Rate gate the look (Burst Rate 0 = constant); each component below "
                "is an independent amount.");
        })(),
        gated(with_panel(submenu("GIFs", std::move(pf_gifs)),
                         "GIF Preview", draw_gif_preview), visible_for_hub75),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
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

    // ── HUD settings ──────────────────────────────────────────────────────────

    auto make_color_items = [](
            std::vector<std::pair<const char*, ImU32>> presets,
            std::function<void(ImU32)> apply,
            std::function<ImU32()> get_fn = nullptr) {
        std::vector<MenuItem> v;
        for (auto& [lbl, col] : presets) {
            if (get_fn)
                v.push_back(leaf_sel(lbl, [apply, col]{ apply(col); },
                                     [get_fn, col]{ return get_fn() == col; }));
            else
                v.push_back(leaf(lbl, [apply, col]{ apply(col); }));
        }
        return v;
    };

    // ── Text Options ──────────────────────────────────────────────────────────
    std::vector<MenuItem> text_color_menu = make_color_items({
        { "White",  IM_COL32(255, 255, 255, 255) },
        { "Cyan",   IM_COL32(  0, 240, 220, 255) },
        { "Orange", IM_COL32(255, 200, 100, 255) },
        { "Amber",  IM_COL32(255, 190,  50, 255) },
        { "Green",  IM_COL32(100, 255, 160, 255) },
        { "Yellow", IM_COL32(255, 240, 100, 255) },
        { "Red",    IM_COL32(255, 100, 100, 255) },
        { "Purple", IM_COL32(200, 130, 255, 255) },
        { "Blue",   IM_COL32(100, 160, 255, 255) },
        { "Pink",   IM_COL32(255, 130, 200, 255) },
    }, [hud_col](ImU32 c){ hud_col->text_fill = c; },
    [hud_col]{ return hud_col->text_fill; });

    // ── Indicator Options ─────────────────────────────────────────────────────
    std::vector<MenuItem> ind_good_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_good = c; },
    [hud_col]{ return hud_col->ind_good; });

    std::vector<MenuItem> ind_inactive_color_menu = make_color_items({
        { "Gray",   IM_COL32(120, 120, 120, 255) },
        { "Blue",   IM_COL32( 60,  80, 160, 255) },
        { "Dim",    IM_COL32( 80,  80,  80, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_inactive = c; },
    [hud_col]{ return hud_col->ind_inactive; });

    std::vector<MenuItem> ind_fail_color_menu = make_color_items({
        { "Red",    IM_COL32(255,  60,  60, 255) },
        { "Orange", IM_COL32(255, 120,   0, 255) },
        { "Yellow", IM_COL32(240, 220,   0, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_fail = c; },
    [hud_col]{ return hud_col->ind_fail; });

    // ── Compass ───────────────────────────────────────────────────────────────
    // Onboard MPU-9250 backup compass (moved from root "Backup Compass")
    std::vector<MenuItem> mpu_mount_menu = {
        leaf_sel("0° — Default",
            [mpu9250]{ if (mpu9250) mpu9250->set_mount_rotation(0); },
            [mpu9250]{ return mpu9250 && mpu9250->get_mount_rotation() == 0; }),
        leaf_sel("90° CCW",
            [mpu9250]{ if (mpu9250) mpu9250->set_mount_rotation(1); },
            [mpu9250]{ return mpu9250 && mpu9250->get_mount_rotation() == 1; }),
        leaf_sel("180°",
            [mpu9250]{ if (mpu9250) mpu9250->set_mount_rotation(2); },
            [mpu9250]{ return mpu9250 && mpu9250->get_mount_rotation() == 2; }),
        leaf_sel("270° CCW",
            [mpu9250]{ if (mpu9250) mpu9250->set_mount_rotation(3); },
            [mpu9250]{ return mpu9250 && mpu9250->get_mount_rotation() == 3; }),
    };

    std::vector<MenuItem> mpu_axes_menu = {
        leaf_sel("0 — XY (chip flat, default)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(0); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 0; }),
        leaf_sel("1 — ZY (chip face-forward)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(1); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 1; }),
        leaf_sel("2 — XZ (chip on left side)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(2); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 2; }),
        leaf_sel("3 — ZX (chip face-forward 90°)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(3); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 3; }),
        leaf_sel("4 — YX (chip on right side)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(4); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 4; }),
        leaf_sel("5 — YZ (chip face-up alt)",
            [mpu9250]{ if (mpu9250) mpu9250->set_heading_axes(5); },
            [mpu9250]{ return mpu9250 && mpu9250->get_heading_axes() == 5; }),
    };

    std::vector<MenuItem> onboard_compass_menu = {
        toggle("Active",
            [mpu9250]{ return mpu9250 && mpu9250->is_running(); },
            [mpu9250](bool v){
                if (!mpu9250) return;
                mpu9250->set_enabled(v);   // update gating flag (also persisted on exit)
                if (v) mpu9250->start(); else mpu9250->stop();
            }),
        toggle("Calibrate",
            [mpu9250]{ return mpu9250 && mpu9250->is_calibrating(); },
            [mpu9250](bool v){
                if (!mpu9250) return;
                if (v) mpu9250->begin_calibration();
                else   mpu9250->end_calibration();
            }),
        submenu("Mounting Orientation", std::move(mpu_mount_menu)),
        submenu("Heading Axes",         std::move(mpu_axes_menu)),
    };

    std::vector<MenuItem> compass_bg_color_menu = make_color_items({
        { "Default", IM_COL32(  8,  12,  18, 255) },
        { "Teal",    IM_COL32(  5,  30,  25, 255) },
        { "Purple",  IM_COL32( 18,   8,  28, 255) },
        { "Blue",    IM_COL32( 10,  10,  40, 255) },
        { "Black",   IM_COL32(  0,   0,   0, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_bg_color = c; },
    [hud_col]{ return hud_col->compass_bg_color; });


    std::vector<MenuItem> compass_tick_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_tick = c; },
    [hud_col]{ return hud_col->compass_tick; });

    std::vector<MenuItem> compass_glow_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_glow = c; },
    [hud_col]{ return hud_col->compass_glow; });

    // ── Tagged Radio Colors (per LoRa node compass markers) ──────────────────
    static const struct { const char* name; ImU32 col; } kNodeColors[] = {
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Yellow", IM_COL32(255, 220,   0, 255) },
        { "Purple", IM_COL32(220,  30, 220, 255) },
        { "Red",    IM_COL32(255,  60,  60, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    };
    std::vector<MenuItem> tagged_radio_colors_menu;
    for (int n = 0; n < 8; n++) {
        std::vector<MenuItem> node_presets;
        for (int c = 0; c < 8; c++) {
            ImU32 cv = kNodeColors[c].col;
            node_presets.push_back(leaf_sel(kNodeColors[c].name,
                [n, cv, &state]{ state.lora_node_colors[n] = cv; },
                [n, cv, &state]{ return state.lora_node_colors[n] == cv; }));
        }
        char lbl[16]; snprintf(lbl, sizeof(lbl), "Node %d", n + 1);
        tagged_radio_colors_menu.push_back(submenu(lbl, std::move(node_presets)));
    }

    using CA = AppState::CompassAxis;
    std::vector<MenuItem> imu_axis_menu = {
        leaf_sel("Roll",  [&state]{ state.compass_axis = CA::Roll;  }, [&state]{ return state.compass_axis == CA::Roll;  }),
        leaf_sel("Pitch", [&state]{ state.compass_axis = CA::Pitch; }, [&state]{ return state.compass_axis == CA::Pitch; }),
        leaf_sel("Yaw",   [&state]{ state.compass_axis = CA::Yaw;   }, [&state]{ return state.compass_axis == CA::Yaw;   }),
        toggle("Invert Direction",
            [&state]{ return state.compass_invert; },
            [&state](bool v){ state.compass_invert = v; }),
    };

    // ── IMU source picker ──────────────────────────────────────────────────
    // Which sensor drives the HUD compass. Auto walks BNO055 > MPU9250 >
    // Viture, picking the highest-priority FRESH source per frame. Pinning
    // to a specific one forces it even if others are also publishing.
    struct ImuSourceOpt { const char* label; AppState::ImuSource value; };
    const ImuSourceOpt imu_source_opts[] = {
        { "Auto (BNO055 > MPU9250 > Viture)", AppState::ImuSource::Auto    },
        { "BNO055 (Adafruit 9-DOF, on-chip fusion)",
                                              AppState::ImuSource::Bno055  },
        { "MPU-9250 (I\xc2\xb2""C compass)",  AppState::ImuSource::Mpu9250 },
        { "VITURE glasses (built-in IMU)",    AppState::ImuSource::Viture  },
        { "None (freeze heading)",            AppState::ImuSource::None    },
    };
    std::vector<MenuItem> imu_source_menu;
    for (const auto& opt : imu_source_opts) {
        const auto v = opt.value;
        imu_source_menu.push_back(leaf_sel(opt.label,
            [&state, v]{ std::lock_guard<std::mutex> lk(state.mtx); state.imu_source = v; },
            [&state, v]{ return state.imu_source == v; }));
    }

    std::vector<MenuItem> compass_menu = {
        toggle("Compass Tape",
            [&state]{ return state.compass_tape; },
            [&state](bool v){ state.compass_tape = v; }),
        with_desc(submenu("IMU Source", std::move(imu_source_menu)),
                  "Which sensor drives the HUD compass. Auto walks "
                  "BNO055 > MPU9250 > Viture and picks the highest-priority "
                  "fresh source each frame; explicit choices force their "
                  "source even if stale."),
        submenu("IMU Axis",            std::move(imu_axis_menu)),
        [&]() -> MenuItem {
            MenuItem m = leaf("Save IMU Calibration",
                [bno055]{ if (bno055) bno055->request_calibration_save(); });
            m.label_fn = [bno055]{
                const int s = bno055 ? bno055->calib_sys() : 0;
                return std::string("Save IMU Calibration  [sys ") +
                       std::to_string(s) + "/3]";
            };
            m.visible_fn = [bno055]{ return bno055 && bno055->connected(); };
            return with_desc(std::move(m),
                "Store the BNO055's current calibration so it loads on boot. "
                "Best when calibration reads 3/3 — rotate the head through "
                "several orientations and a figure-8 for the magnetometer.");
        }(),
        submenu("Onboard Compass",     std::move(onboard_compass_menu)),
        slider("Tick Length", 8.f, 48.f, 2.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_tick_length); },
            [hud_cfg](float v){ hud_cfg->compass_tick_length = static_cast<int>(v); }),
        submenu("Tagged Radio Colors", std::move(tagged_radio_colors_menu)),
        slider("Tape Height", 50.f, 120.f, 5.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_height); },
            [hud_cfg](float v){ hud_cfg->compass_height = static_cast<int>(v); }),
    };

    // ── Clock & Timers ────────────────────────────────────────────────────────
    std::vector<MenuItem> custom_timer_menu = {
        slider("Minutes", 0.f, 99.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.custom_timer_min); },
            [&state](float v){ state.timer_alarm.custom_timer_min = static_cast<int>(v); }),
        slider("Seconds", 0.f, 59.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.custom_timer_sec); },
            [&state](float v){ state.timer_alarm.custom_timer_sec = static_cast<int>(v); }),
        leaf("Start", [&state]{
            int secs = state.timer_alarm.custom_timer_min * 60
                     + state.timer_alarm.custom_timer_sec;
            if (secs > 0) {
                state.timer_alarm.timer_active = true;
                state.timer_alarm.timer_end    = time(nullptr) + secs;
            }
        }),
    };

    std::vector<MenuItem> timer_presets_menu = {
        submenu("Custom", std::move(custom_timer_menu)),
        leaf_sel("5 min",  [&state]{ state.timer_alarm.timer_active = true;
                                      state.timer_alarm.timer_end = time(nullptr) + 300; },
                 [&state]{ return state.timer_alarm.timer_active
                              && (state.timer_alarm.timer_end - time(nullptr)) <= 300
                              && (state.timer_alarm.timer_end - time(nullptr)) > 0; }),
        leaf_sel("10 min", [&state]{ state.timer_alarm.timer_active = true;
                                      state.timer_alarm.timer_end = time(nullptr) + 600; },
                 [&state]{ return state.timer_alarm.timer_active
                              && (state.timer_alarm.timer_end - time(nullptr)) <= 600
                              && (state.timer_alarm.timer_end - time(nullptr)) > 300; }),
        leaf_sel("30 min", [&state]{ state.timer_alarm.timer_active = true;
                                      state.timer_alarm.timer_end = time(nullptr) + 1800; },
                 [&state]{ return state.timer_alarm.timer_active
                              && (state.timer_alarm.timer_end - time(nullptr)) <= 1800
                              && (state.timer_alarm.timer_end - time(nullptr)) > 600; }),
        leaf_sel("60 min", [&state]{ state.timer_alarm.timer_active = true;
                                      state.timer_alarm.timer_end = time(nullptr) + 3600; },
                 [&state]{ return state.timer_alarm.timer_active
                              && (state.timer_alarm.timer_end - time(nullptr)) > 1800; }),
        leaf("Cancel Timer", [&state]{ state.timer_alarm.timer_active = false; }),
    };

    std::vector<MenuItem> alarm_picker_menu = {
        slider("Hour",   0.f, 23.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.alarm_hour); },
            [&state](float v){ state.timer_alarm.alarm_hour = static_cast<int>(v); }),
        slider("Minute", 0.f, 59.f, 1.f, "",
            [&state]{ return static_cast<float>(state.timer_alarm.alarm_minute); },
            [&state](float v){ state.timer_alarm.alarm_minute = static_cast<int>(v); }),
        leaf("Confirm", [&state]{
            time_t now = time(nullptr);
            struct tm t = *localtime(&now);
            t.tm_hour = state.timer_alarm.alarm_hour;
            t.tm_min  = state.timer_alarm.alarm_minute;
            t.tm_sec  = 0;
            time_t fire = mktime(&t);
            if (fire <= now) fire += 86400;
            state.timer_alarm.alarm_active  = true;
            state.timer_alarm.alarm_fire_at = fire;
        }),
        leaf("Delete Alarm", [&state]{
            state.timer_alarm.alarm_active    = false;
            state.timer_alarm.alarm_triggered = false;
        }),
    };

    std::vector<MenuItem> timers_alarm_menu = {
        submenu("Set Timer",   std::move(timer_presets_menu)),
        submenu("Set Alarm",   std::move(alarm_picker_menu)),
        leaf("Dismiss Alarm", [&state]{ state.timer_alarm.alarm_triggered = false; }),
    };

    // ── Color Options ─────────────────────────────────────────────────────────

    // HUD > Borders & Lines
    std::vector<MenuItem> borders_lines_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Yellow", IM_COL32(255, 240,  50, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
        { "Red",    IM_COL32(255,  50,  50, 255) },
    }, [hud_col](ImU32 c){ hud_col->glow_base = c; },
    [hud_col]{ return hud_col->glow_base; });

    // Themes — apply a coordinated set of HudColors + MenuSystem styles
    auto apply_theme = [hud_col, menu_sys_pp](
            ImU32 glow, ImU32 glow_col, ImU32 text,
            ImU32 tick, ImU32 tick_glow,
            bool border, float thickness, SelectionStyle style,
            bool menu_glow, bool menu_bold,
            ImU32 menu_accent) {
        hud_col->glow_base    = glow;
        hud_col->glow_color   = glow_col;
        hud_col->text_fill    = text;
        hud_col->compass_tick = tick;
        hud_col->compass_glow = tick_glow;
        if (*menu_sys_pp) {
            (*menu_sys_pp)->set_accent_color(menu_accent);
            (*menu_sys_pp)->set_border_enabled(border);
            (*menu_sys_pp)->set_border_color(menu_accent);
            (*menu_sys_pp)->set_border_thickness(thickness);
            (*menu_sys_pp)->set_selection_style(style);
            (*menu_sys_pp)->set_glow_enabled(menu_glow);
            (*menu_sys_pp)->set_bold_text(menu_bold);
        }
    };

    // Built-in coordinated themes — reusable so the same quick-apply list can live
    // under HUD (with Effects) and under System → HUD/Menu Presets.
    auto make_builtin_theme_leaves = [apply_theme]() -> std::vector<MenuItem> {
        std::vector<MenuItem> v;
        v.push_back(leaf("Halo", [apply_theme]{
            apply_theme(IM_COL32(255,255,255,255), IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255), IM_COL32(255,255,255,180),
                        true, 5.f, SelectionStyle::FILLED_ROW,
                        false, true,
                        IM_COL32(255,255,255,255));
        }));
        v.push_back(leaf("Solar", [apply_theme]{
            apply_theme(IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
                        IM_COL32(255,160, 32,255));
        }));
        v.push_back(leaf("Fallout", [apply_theme]{
            apply_theme(IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        IM_COL32(  0,255, 80,255),
                        IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
                        IM_COL32(  0,200, 50,255));
        }));
        v.push_back(leaf("Space", [apply_theme]{
            apply_theme(IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        IM_COL32(200,220,255,255),
                        IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
                        IM_COL32( 80,100,255,255));
        }));
        return v;
    };

    std::vector<MenuItem> themes_menu = make_builtin_theme_leaves();

    // Effects — particle overlays with palette options
    std::vector<MenuItem> fx_palette_menu = {
        leaf_sel("Match Theme", [&state]{ state.effects_cfg.palette = EffectPalette::Theme;   },
                               [&state]{ return state.effects_cfg.palette == EffectPalette::Theme;   }),
        leaf_sel("Halo",        [&state]{ state.effects_cfg.palette = EffectPalette::Halo;    },
                               [&state]{ return state.effects_cfg.palette == EffectPalette::Halo;    }),
        leaf_sel("Solar",       [&state]{ state.effects_cfg.palette = EffectPalette::Solar;   },
                               [&state]{ return state.effects_cfg.palette == EffectPalette::Solar;   }),
        leaf_sel("Fallout",     [&state]{ state.effects_cfg.palette = EffectPalette::Fallout; },
                               [&state]{ return state.effects_cfg.palette == EffectPalette::Fallout; }),
        leaf_sel("Space",       [&state]{ state.effects_cfg.palette = EffectPalette::Space;   },
                               [&state]{ return state.effects_cfg.palette == EffectPalette::Space;   }),
    };

    std::vector<MenuItem> effects_menu = {
        leaf_sel("None",               [&state]{ state.effects_cfg.effect = EffectType::None;               },
                                       [&state]{ return state.effects_cfg.effect == EffectType::None;               }),
        leaf_sel("Arm Glints",         [&state]{ state.effects_cfg.effect = EffectType::ArmGlints;          },
                                       [&state]{ return state.effects_cfg.effect == EffectType::ArmGlints;          }),
        leaf_sel("Corner Drift",       [&state]{ state.effects_cfg.effect = EffectType::CornerDrift;        },
                                       [&state]{ return state.effects_cfg.effect == EffectType::CornerDrift;        }),
        leaf_sel("Popup Burst",        [&state]{ state.effects_cfg.effect = EffectType::PopupBurst;         },
                                       [&state]{ return state.effects_cfg.effect == EffectType::PopupBurst;         }),
        leaf_sel("Compass Turbulence", [&state]{ state.effects_cfg.effect = EffectType::CompassTurbulence;  },
                                       [&state]{ return state.effects_cfg.effect == EffectType::CompassTurbulence;  }),
        leaf_sel("Nebula Edge",        [&state]{ state.effects_cfg.effect = EffectType::NebulaEdge;         },
                                       [&state]{ return state.effects_cfg.effect == EffectType::NebulaEdge;         }),
        leaf_sel("Dark Vignette",      [&state]{ state.effects_cfg.effect = EffectType::DarkVignette;       },
                                       [&state]{ return state.effects_cfg.effect == EffectType::DarkVignette;       }),
        submenu("Color Palette", std::move(fx_palette_menu)),
    };

    themes_menu.push_back(submenu("Effects", std::move(effects_menu)));

    // Backgrounds
    std::vector<MenuItem> menu_bg_color_menu = make_color_items({
        { "Dark",   IM_COL32( 10,  15,  20, 225) },
        { "Teal",   IM_COL32(  5,  25,  22, 225) },
        { "Purple", IM_COL32( 20,   8,  28, 225) },
        { "Navy",   IM_COL32(  8,   8,  35, 225) },
        { "Black",  IM_COL32(  0,   0,   0, 230) },
    }, [menu_sys_pp](ImU32 c){ if (*menu_sys_pp) (*menu_sys_pp)->set_bg_color(c); },
    [menu_sys_pp]{ return *menu_sys_pp ? (*menu_sys_pp)->bg_color() : IM_COL32(10,15,20,225); });

    std::vector<MenuItem> hud_bg_color_menu = make_color_items({
        { "Dark",   IM_COL32( 10,  15,  20, 210) },
        { "Navy",   IM_COL32(  5,  10,  25, 210) },
        { "Black",  IM_COL32(  0,   0,   0, 220) },
        { "Teal",   IM_COL32(  5,  20,  18, 210) },
        { "Green",  IM_COL32(  0,  18,   5, 210) },
        { "Space",  IM_COL32(  4,   4,  22, 210) },
    }, [hud_col](ImU32 c){
        hud_col->background       = c;
        hud_col->compass_bg_color = c;
    },
    [hud_col]{ return hud_col->background; });

    std::vector<MenuItem> backgrounds_menu = {
        toggle("Indicator Background",
            [hud_cfg]{ return hud_cfg->indicator_bg_enabled; },
            [hud_cfg](bool v){ hud_cfg->indicator_bg_enabled = v; }),
        toggle("Compass Background",
            [&state]{ return state.compass_bg_enabled; },
            [&state](bool v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.compass_bg_enabled = v;
            }),
        submenu("Compass Background Color", std::move(compass_bg_color_menu)),
        toggle("Menu Background",
            [menu_sys_pp]{ return *menu_sys_pp && (*menu_sys_pp)->bg_enabled(); },
            [menu_sys_pp](bool v){ if (*menu_sys_pp) (*menu_sys_pp)->set_bg_enabled(v); }),
        submenu("HUD Background Color",  std::move(hud_bg_color_menu)),
        submenu("Menu Background Color", std::move(menu_bg_color_menu)),
    };

    // Indicators
    std::vector<MenuItem> indicators_menu = {
        submenu("Active Color",   std::move(ind_good_color_menu)),
        submenu("Inactive Color", std::move(ind_inactive_color_menu)),
        submenu("Fail Color",     std::move(ind_fail_color_menu)),
    };

    // Glow
    std::vector<MenuItem> glow_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
        { "Yellow", IM_COL32(255, 240,  50, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "Red",    IM_COL32(255,  50,  50, 255) },
    }, [hud_col](ImU32 c){ hud_col->glow_color = c; },
    [hud_col]{ return hud_col->glow_color; });

    std::vector<MenuItem> glow_menu = {
        toggle("Text Glow",
            [hud_cfg]{ return hud_cfg->glow_enabled; },
            [hud_cfg](bool v){ hud_cfg->glow_enabled = v; }),
        toggle("Tick Glow",
            [hud_cfg]{ return hud_cfg->compass_tick_glow; },
            [hud_cfg](bool v){ hud_cfg->compass_tick_glow = v; }),
        slider("Glow Intensity", 0.f, 2.f, 0.05f, "",
            [hud_cfg]{ return hud_cfg->glow_intensity; },
            [hud_cfg](float v){ hud_cfg->glow_intensity = v; }),
        submenu("Glow Color", std::move(glow_color_menu)),
    };

    std::vector<MenuItem> menu_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
    }, [menu_sys_pp](ImU32 c){ if (*menu_sys_pp) (*menu_sys_pp)->set_accent_color(c); },
    [menu_sys_pp]{ return *menu_sys_pp ? (*menu_sys_pp)->accent_color() : IM_COL32(255,160,32,255); });

    std::vector<MenuItem> menu_border_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Yellow", IM_COL32(255, 240,  50, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
        { "Red",    IM_COL32(255,  50,  50, 255) },
    }, [menu_sys_pp](ImU32 c){ if (*menu_sys_pp) (*menu_sys_pp)->set_border_color(c); },
    [menu_sys_pp]{ return *menu_sys_pp ? (*menu_sys_pp)->border_color() : IM_COL32(255,160,32,255); });

    std::vector<MenuItem> menu_border_menu = {
        toggle("Border",
            [menu_sys_pp]{ return *menu_sys_pp && (*menu_sys_pp)->border_enabled(); },
            [menu_sys_pp](bool v){ if (*menu_sys_pp) (*menu_sys_pp)->set_border_enabled(v); }),
        slider("Thickness", 0.5f, 8.f, 0.5f, "px",
            [menu_sys_pp]{ return *menu_sys_pp ? (*menu_sys_pp)->border_thickness() : 1.5f; },
            [menu_sys_pp](float v){ if (*menu_sys_pp) (*menu_sys_pp)->set_border_thickness(v); }),
        submenu("Border Color", std::move(menu_border_color_menu)),
    };

    std::vector<MenuItem> color_options_menu = {
        submenu("Borders & Lines",    std::move(borders_lines_menu)),
        submenu("Themes and Effects", std::move(themes_menu)),
        submenu("Compass Tick Color", std::move(compass_tick_color_menu)),
        submenu("Compass Glow Color", std::move(compass_glow_color_menu)),
        submenu("Text Color",         std::move(text_color_menu)),
        submenu("Menu Color",         std::move(menu_color_menu)),
        submenu("Menu Border",        std::move(menu_border_menu)),
        submenu("Backgrounds",        std::move(backgrounds_menu)),
        submenu("Indicators",         std::move(indicators_menu)),
        submenu("Glow",               std::move(glow_menu)),
    };

    std::vector<MenuItem> clock_offset_menu = {
        leaf("+1 hour",   [&state]{ state.clock_cfg.manual_offset_s += 3600; }),
        leaf("-1 hour",   [&state]{ state.clock_cfg.manual_offset_s -= 3600; }),
        leaf("+1 minute", [&state]{ state.clock_cfg.manual_offset_s +=   60; }),
        leaf("-1 minute", [&state]{ state.clock_cfg.manual_offset_s -=   60; }),
        leaf("Reset",     [&state]{ state.clock_cfg.manual_offset_s  =    0; }),
    };
    std::vector<MenuItem> clock_menu = {
        toggle("24-Hour",
            [&state]{ return state.clock_cfg.use_24h; },
            [&state](bool v){ state.clock_cfg.use_24h = v; }),
        toggle("Seconds",
            [&state]{ return state.clock_cfg.show_seconds; },
            [&state](bool v){ state.clock_cfg.show_seconds = v; }),
        toggle("Show Date",
            [&state]{ return state.clock_cfg.show_date; },
            [&state](bool v){ state.clock_cfg.show_date = v; }),
        slider("Font Size", 1.f, 3.f, 0.25f, "x",
            [&state]{ return state.clock_cfg.font_scale; },
            [&state](float v){ state.clock_cfg.font_scale = v; }),
        submenu("Time Offset", std::move(clock_offset_menu)),
    };

    std::vector<MenuItem> menu_position_menu = {
        leaf_sel("Top Left",
            [menu_sys_pp]{ (*menu_sys_pp)->set_anchor(MenuAnchor::TopLeft); },
            [menu_sys_pp]{ return (*menu_sys_pp)->anchor() == MenuAnchor::TopLeft; }),
        leaf_sel("Top Right",
            [menu_sys_pp]{ (*menu_sys_pp)->set_anchor(MenuAnchor::TopRight); },
            [menu_sys_pp]{ return (*menu_sys_pp)->anchor() == MenuAnchor::TopRight; }),
        leaf_sel("Bottom Left",
            [menu_sys_pp]{ (*menu_sys_pp)->set_anchor(MenuAnchor::BottomLeft); },
            [menu_sys_pp]{ return (*menu_sys_pp)->anchor() == MenuAnchor::BottomLeft; }),
        leaf_sel("Bottom Right",
            [menu_sys_pp]{ (*menu_sys_pp)->set_anchor(MenuAnchor::BottomRight); },
            [menu_sys_pp]{ return (*menu_sys_pp)->anchor() == MenuAnchor::BottomRight; }),
    };

    // ── Map Overlay ───────────────────────────────────────────────────────────
    // Scan map directory for PNG/JPG files at menu-build time.
    auto scan_map_dir = [](const std::string& dir) {
        std::vector<std::string> paths;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            auto ext = e.path().extension().string();
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                ext == ".PNG" || ext == ".JPG" || ext == ".JPEG")
                paths.push_back(e.path().string());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    };
    auto map_files = scan_map_dir(map_dir);

    std::vector<MenuItem> map_select_items;
    for (auto& path : map_files) {
        std::string name = std::filesystem::path(path).stem().string();
        map_select_items.push_back(
            leaf_sel(name,
                [&state, path]{ std::lock_guard<std::mutex> lk(state.mtx); state.map_overlay.map_path = path; },
                [&state, path]{ return state.map_overlay.map_path == path; }));
    }
    if (map_select_items.empty())
        map_select_items.push_back(leaf("(no maps in " + map_dir + ")", []{}));

    // Anchor presets snap the map anchor point to a screen position and clear
    // the pan offset so the map lands exactly at the chosen location.
    struct MapAnchorPreset { const char* label; float ax, ay; };
    static const MapAnchorPreset MAP_ANCHORS[] = {
        { "Top Left",      0.00f, 0.00f },
        { "Top Center",    0.50f, 0.00f },
        { "Top Right",     1.00f, 0.00f },
        { "Center",        0.50f, 0.50f },
        { "Bottom Left",   0.00f, 1.00f },
        { "Bottom Center", 0.50f, 1.00f },
        { "Bottom Right",  1.00f, 1.00f },
    };
    std::vector<MenuItem> map_snap_items;
    for (const auto& a : MAP_ANCHORS) {
        map_snap_items.push_back(leaf_sel(a.label,
            [&state, ax = a.ax, ay = a.ay]{
                state.map_overlay.anchor_x = ax;
                state.map_overlay.anchor_y = ay;
                state.map_overlay.pan_x    = 0.f;
                state.map_overlay.pan_y    = 0.f;
            },
            [&state, ax = a.ax, ay = a.ay]{
                return state.map_overlay.anchor_x == ax &&
                       state.map_overlay.anchor_y == ay;
            }));
    }
    std::vector<MenuItem> map_move_menu = {
        submenu("Snap To",    std::move(map_snap_items)),
        leaf("Left  -20px",  [&state]{ state.map_overlay.pan_x -= 20.f; }),
        leaf("Right +20px",  [&state]{ state.map_overlay.pan_x += 20.f; }),
        leaf("Up    -20px",  [&state]{ state.map_overlay.pan_y -= 20.f; }),
        leaf("Down  +20px",  [&state]{ state.map_overlay.pan_y += 20.f; }),
        leaf("Reset",        [&state]{
            state.map_overlay.anchor_x = 0.5f; state.map_overlay.anchor_y = 0.5f;
            state.map_overlay.pan_x    = 0.f;  state.map_overlay.pan_y    = 0.f;
        }),
    };

    std::vector<MenuItem> map_rotate_menu = {
        leaf("CCW  -90°",       [&state]{ state.map_overlay.image_rotate_deg -= 90.f; }),
        leaf("CCW  -45°",       [&state]{ state.map_overlay.image_rotate_deg -= 45.f; }),
        leaf("CCW  -15°",       [&state]{ state.map_overlay.image_rotate_deg -= 15.f; }),
        leaf("CCW   -5°",       [&state]{ state.map_overlay.image_rotate_deg -=  5.f; }),
        leaf("CW    +5°",       [&state]{ state.map_overlay.image_rotate_deg +=  5.f; }),
        leaf("CW   +15°",       [&state]{ state.map_overlay.image_rotate_deg += 15.f; }),
        leaf("CW   +45°",       [&state]{ state.map_overlay.image_rotate_deg += 45.f; }),
        leaf("CW   +90°",       [&state]{ state.map_overlay.image_rotate_deg += 90.f; }),
        leaf("Reset Rotation",  [&state]{ state.map_overlay.image_rotate_deg =  0.f; }),
    };


    std::vector<MenuItem> map_size_menu = {
        leaf_sel("Tiny   (100px)", [&state]{ state.map_overlay.size_px = 100.f; }, [&state]{ return state.map_overlay.size_px == 100.f; }),
        leaf_sel("Mini   (150px)", [&state]{ state.map_overlay.size_px = 150.f; }, [&state]{ return state.map_overlay.size_px == 150.f; }),
        leaf_sel("Small  (200px)", [&state]{ state.map_overlay.size_px = 200.f; }, [&state]{ return state.map_overlay.size_px == 200.f; }),
        leaf_sel("Medium (300px)", [&state]{ state.map_overlay.size_px = 300.f; }, [&state]{ return state.map_overlay.size_px == 300.f; }),
        leaf_sel("Large  (450px)", [&state]{ state.map_overlay.size_px = 450.f; }, [&state]{ return state.map_overlay.size_px == 450.f; }),
        leaf_sel("Full   (600px)", [&state]{ state.map_overlay.size_px = 600.f; }, [&state]{ return state.map_overlay.size_px == 600.f; }),
    };

    // ── Mini-Map Module ───────────────────────────────────────────────────────
    // Always enabled (apply_hud_dock forces it on). Module Controls toggles the
    // overlay chrome; Map Options handles the underlying map image.
    auto ipw = [](InfoWidget w) { return static_cast<int>(w); };
    std::vector<MenuItem> module_controls_menu = {
        toggle("Compass Ring",
            [&state]{ return state.map_overlay.compass_ring; },
            [&state](bool v){ state.map_overlay.compass_ring = v; }),
        toggle("Battery Arc",
            [&state]{ return state.map_overlay.battery_arc; },
            [&state](bool v){ state.map_overlay.battery_arc = v; }),
        toggle("Gauge: CPU/GPU",
            [&state]{ return state.map_overlay.system_debug; },
            [&state](bool v){ state.map_overlay.system_debug = v; }),
        toggle("Clock",
            [&state]{ return state.map_overlay.clock; },
            [&state](bool v){ state.map_overlay.clock = v; }),
        toggle("Date",
            [&state]{ return state.map_overlay.clock_date; },
            [&state](bool v){ state.map_overlay.clock_date = v; }),
        toggle("Protoface Preview",
            [&state]{ return state.map_overlay.portrait; },
            [&state](bool v){ state.map_overlay.portrait = v; }),
        toggle("Preview: Right Half",
            [&state]{ return state.map_overlay.portrait_right_half; },
            [&state](bool v){ state.map_overlay.portrait_right_half = v; }),
        slider("Preview Size", 0.5f, 2.5f, 0.1f, "x",
            [&state]{ return state.map_overlay.portrait_scale; },
            [&state](float v){ state.map_overlay.portrait_scale = v; }),
    };
    std::vector<MenuItem> map_options_menu = {
        toggle("Circle Window",
            [&state]{ return state.map_overlay.circle_window; },
            [&state](bool v){ state.map_overlay.circle_window = v; }),
        submenu("Select Map",   std::move(map_select_items)),
        submenu("Move Map",     std::move(map_move_menu)),
        submenu("Rotate Map",   std::move(map_rotate_menu)),
        submenu("Size",         std::move(map_size_menu)),
        slider("Zoom", 1.f, 4.f, 0.1f, "x",
            [&state]{ return state.map_overlay.zoom; },
            [&state](float v){ state.map_overlay.zoom = v; }),
        slider("Transparency", 0.f, 1.f, 0.05f, "",
            [&state]{ return state.map_overlay.opacity; },
            [&state](float v){ state.map_overlay.opacity = v; }),
        toggle("Rotate with Heading",
            [&state]{ return state.map_overlay.rotate_with_heading; },
            [&state](bool v){ state.map_overlay.rotate_with_heading = v; }),
        leaf("Set My Direction", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.map_north_deg = state.compass_heading;
            state.map_overlay.calibrated    = true;
        }),
        leaf("Expand Map (Pan/Zoom)", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.expanded   = true;
            state.map_overlay.view_zoom  = 1.f;
            state.map_overlay.view_pan_x = 0.f;
            state.map_overlay.view_pan_y = 0.f;
        }),
        with_desc(toggle("Expanded: Debug Window",
            [&state]{ return state.expanded_show_debug; },
            [&state](bool v){ state.expanded_show_debug = v; }),
            "In the expanded map view, also show the debug/system panel, opened to "
            "the right of the info sidebar."),
        with_desc(toggle("Expanded: Hide Info Panel",
            [&state]{ return state.expanded_hide_info; },
            [&state](bool v){ state.expanded_hide_info = v; }),
            "Hide the cycling info panel while the map is expanded (its weather / "
            "schedule / time already appear in the sidebar)."),
    };
    // Mini-Map Module: the module-control toggles sit directly at the top (no
    // longer nested under a "Module Controls" leaf), with the map image controls
    // folded into a single "Map Options" submenu (Circle Window lives there too).
    std::vector<MenuItem> mini_map_menu = std::move(module_controls_menu);
    mini_map_menu.push_back(submenu("Map Options", std::move(map_options_menu)));

    // ── Info-Panel Module ─────────────────────────────────────────────────────
    // Live preview of the selected analog clock face for the Clock Face context
    // pane. Mirrors draw_info_panel's face logic, drawn with ImDrawList.
    auto draw_clock_preview = [&state, hud_col](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
        const float PI = 3.14159265358979f, TWO_PI = 2.f * PI, HALF_PI = PI * 0.5f;
        const float cx = o.x + sz.x * 0.5f;
        const float cy = o.y + sz.y * 0.5f - 6.f;          // leave a line for the name
        const float cr = std::min(sz.x, sz.y) * 0.40f;
        const int   face = state.info_panel.clock_face;
        auto with_a = [](ImU32 c, unsigned a){ return (c & 0x00FFFFFFu) | (a << 24); };

        enum { M_TICKS, M_NUMBERS, M_QUARTERS };
        int   markers = M_TICKS, signature = 0;
        ImU32 markCol = IM_COL32(200, 220, 230, 150);
        ImU32 handCol = hud_col ? hud_col->text_fill : IM_COL32(255, 255, 255, 255);
        const ImU32 secCol = IM_COL32(235, 80, 70, 235);
        const char* name = "Ticks";
        switch (face) {
            case 1: markers = M_NUMBERS;  markCol = with_a(handCol, 220); name = "Numbers"; break;
            case 2: markers = M_QUARTERS; name = "Minimal"; break;
            case 3: markers = M_TICKS;    signature = 1; markCol = IM_COL32(120,205,235,200); handCol = IM_COL32(150,235,255,255); name = "Halo"; break;
            case 4: markers = M_TICKS;    signature = 2; markCol = IM_COL32(255,160, 32,210); handCol = IM_COL32(255,160, 32,255); name = "Solar"; break;
            case 5: markers = M_NUMBERS;  signature = 3; markCol = IM_COL32(  0,255, 80,230); handCol = IM_COL32(  0,255, 80,255); name = "Fallout"; break;
            case 6: markers = M_QUARTERS; signature = 4; markCol = IM_COL32( 80,100,255,210); handCol = IM_COL32(200,220,255,255); name = "Space"; break;
            case 7: markers = M_TICKS;    markCol = with_a(hud_col ? hud_col->compass_tick : IM_COL32(255,255,255,255), 200);
                    handCol = hud_col ? hud_col->text_fill : IM_COL32(255,255,255,255); name = "Auto (theme)"; break;
            default: break;
        }

        dl->AddCircleFilled({cx, cy}, cr * 1.18f, IM_COL32(10, 16, 22, 180), 48);
        dl->AddCircle({cx, cy}, cr * 1.18f, with_a(handCol, 150), 48, 1.6f);

        if (markers == M_NUMBERS) {
            for (int h = 1; h <= 12; ++h) {
                const float a = h / 12.f * TWO_PI - HALF_PI;
                char nb[4]; snprintf(nb, sizeof(nb), "%d", h);
                const ImVec2 ts = ImGui::CalcTextSize(nb);
                dl->AddText({cx + std::cos(a) * cr * 0.82f - ts.x * 0.5f,
                             cy + std::sin(a) * cr * 0.82f - ts.y * 0.5f}, markCol, nb);
            }
        } else {
            const int step = (markers == M_QUARTERS) ? 3 : 1;
            for (int i = 0; i < 12; i += step) {
                const float a = i / 12.f * TWO_PI - HALF_PI;
                dl->AddLine({cx + std::cos(a) * cr * 0.86f, cy + std::sin(a) * cr * 0.86f},
                            {cx + std::cos(a) * cr * 0.99f, cy + std::sin(a) * cr * 0.99f},
                            markCol, (i % 3 == 0) ? 2.5f : 1.f);
            }
        }

        if (signature == 1) {                  // Halo — ring + bright top arc + hub ring
            dl->AddCircle({cx, cy}, cr * 1.05f, IM_COL32(150,235,255,110), 48, 1.4f);
            dl->PathArcTo({cx, cy}, cr * 1.05f, -HALF_PI - 1.2f, -HALF_PI + 1.2f, 24);
            dl->PathStroke(IM_COL32(150,235,255, 80), 0, 6.f);
            dl->PathArcTo({cx, cy}, cr * 1.05f, -HALF_PI - 1.2f, -HALF_PI + 1.2f, 24);
            dl->PathStroke(IM_COL32(190,245,255,220), 0, 2.f);
            dl->AddCircle({cx, cy}, cr * 0.32f, IM_COL32(120,205,235,110), 32, 1.f);
        } else if (signature == 2) {           // Solar — diagonal rays
            for (int k = 0; k < 4; ++k) {
                const float a = (k / 4.f) * TWO_PI + HALF_PI * 0.5f;
                dl->AddLine({cx + std::cos(a) * cr * 1.02f, cy + std::sin(a) * cr * 1.02f},
                            {cx + std::cos(a) * cr * 1.16f, cy + std::sin(a) * cr * 1.16f},
                            IM_COL32(255,160,32,200), 2.f);
            }
        } else if (signature == 3) {           // Fallout — indicator triangle at 12
            const float ty = cy - cr * 0.99f;
            dl->AddTriangleFilled({cx, ty + cr * 0.10f}, {cx - cr * 0.06f, ty},
                                  {cx + cr * 0.06f, ty}, IM_COL32(0,255,80,220));
        } else if (signature == 4) {           // Space — 4-point star at 12
            const float sx = cx, sy = cy - cr * 0.84f, sr = cr * 0.14f;
            const ImU32 stc = IM_COL32(200,220,255,230);
            ImVec2 vd[4] = {{sx, sy - sr}, {sx + sr*0.28f, sy}, {sx, sy + sr}, {sx - sr*0.28f, sy}};
            ImVec2 hd[4] = {{sx - sr, sy}, {sx, sy - sr*0.28f}, {sx + sr, sy}, {sx, sy + sr*0.28f}};
            dl->AddConvexPolyFilled(vd, 4, stc);
            dl->AddConvexPolyFilled(hd, 4, stc);
        }

        const time_t now = std::time(nullptr) + static_cast<time_t>(state.clock_cfg.manual_offset_s);
        struct tm tmv; localtime_r(&now, &tmv);
        auto hand = [&](float frac, float len, float w, ImU32 c) {
            const float a = frac * TWO_PI - HALF_PI;
            dl->AddLine({cx, cy}, {cx + std::cos(a) * len, cy + std::sin(a) * len}, c, w);
        };
        const float hh = (tmv.tm_hour % 12) + tmv.tm_min / 60.f;
        const float mm = tmv.tm_min + tmv.tm_sec / 60.f;
        hand(hh / 12.f, cr * 0.52f, 3.f, handCol);
        hand(mm / 60.f, cr * 0.80f, 2.f, handCol);
        hand(tmv.tm_sec / 60.f, cr * 0.90f, 1.f, secCol);
        dl->AddCircleFilled({cx, cy}, 3.f, secCol, 12);

        const ImVec2 ns = ImGui::CalcTextSize(name);
        dl->AddText({cx - ns.x * 0.5f, o.y + sz.y - ns.y}, with_a(handCol, 230), name);
    };

    std::vector<MenuItem> clock_face_menu = {
        live(leaf_sel("Ticks",        [&state]{ state.info_panel.clock_face = 0; },
                                      [&state]{ return state.info_panel.clock_face == 0; })),
        live(leaf_sel("Numbers",      [&state]{ state.info_panel.clock_face = 1; },
                                      [&state]{ return state.info_panel.clock_face == 1; })),
        live(leaf_sel("Minimal",      [&state]{ state.info_panel.clock_face = 2; },
                                      [&state]{ return state.info_panel.clock_face == 2; })),
        live(leaf_sel("Halo",         [&state]{ state.info_panel.clock_face = 3; },
                                      [&state]{ return state.info_panel.clock_face == 3; })),
        live(leaf_sel("Solar",        [&state]{ state.info_panel.clock_face = 4; },
                                      [&state]{ return state.info_panel.clock_face == 4; })),
        live(leaf_sel("Fallout",      [&state]{ state.info_panel.clock_face = 5; },
                                      [&state]{ return state.info_panel.clock_face == 5; })),
        live(leaf_sel("Space",        [&state]{ state.info_panel.clock_face = 6; },
                                      [&state]{ return state.info_panel.clock_face == 6; })),
        live(leaf_sel("Auto (theme)", [&state]{ state.info_panel.clock_face = 7; },
                                      [&state]{ return state.info_panel.clock_face == 7; })),
    };
    std::vector<MenuItem> ip_clock_menu = {
        toggle("Show Clock",
            [&state, ipw]{ return state.info_panel.show[ipw(InfoWidget::Clock)]; },
            [&state, ipw](bool v){ state.info_panel.show[ipw(InfoWidget::Clock)] = v; }),
        with_panel(
            with_desc(submenu("Clock Face", std::move(clock_face_menu)),
                      "Pick the analog clock style. The preview at right updates as you "
                      "scroll, and the panel clock changes live too."),
            "Clock Preview", draw_clock_preview),
    };
    std::vector<MenuItem> ip_notif_menu = {
        toggle("Show Notifications",
            [&state, ipw]{ return state.info_panel.show[ipw(InfoWidget::Notifications)]; },
            [&state, ipw](bool v){ state.info_panel.show[ipw(InfoWidget::Notifications)] = v; }),
    };
    std::vector<MenuItem> ip_schedule_menu = {
        toggle("Show Schedule",
            [&state, ipw]{ return state.info_panel.show[ipw(InfoWidget::Schedule)]; },
            [&state, ipw](bool v){ state.info_panel.show[ipw(InfoWidget::Schedule)] = v; }),
        slider("Reminder Lead", 0.f, 60.f, 5.f, "min",
            [&state]{ return static_cast<float>(state.scheduler_lead_min); },
            [&state](float v){ state.scheduler_lead_min = static_cast<int>(v); }),
        // Push the scheduler web page link to the paired phone over KDE Connect.
        with_desc(leaf("Send Link to Phone", [&state, kdc_p]{
                std::string url;
                { std::lock_guard<std::mutex> lk(state.mtx); url = state.scheduler_status.web_url; }
                const bool ok = kdc_p && kdc_p->device_ready() && !url.empty()
                                && kdc_p->send_ping("ProtoHUD scheduler: " + url);
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "Link sent to phone"
                             : (url.empty() ? "Scheduler link not ready" : "No phone connected");
                n.body  = ok ? url : "Connect a KDE Connect device and wait for the "
                                     "scheduler daemon to publish its URL.";
                n.auto_dismiss_s = 6.f; state.notifs.push(std::move(n));
            }),
            "Send a tappable notification with the scheduler web-page link to your "
            "phone over KDE Connect (it won't auto-open)."),
        with_desc(toggle("Send Link on Startup",
            [&state]{ return state.sched_send_link_startup; },
            [&state, cfg_root](bool v){
                state.sched_send_link_startup = v;
                if (cfg_root) (*cfg_root)["scheduler"]["send_link_on_startup"] = v;
            }),
            "Each boot, automatically push the scheduler web link to the paired "
            "phone once its URL and the phone are both ready."),
    };
    std::vector<MenuItem> ip_weather_menu = {
        toggle("Show Current Page",
            [&state, ipw]{ return state.info_panel.show[ipw(InfoWidget::Weather)]; },
            [&state, ipw](bool v){
                state.info_panel.show[ipw(InfoWidget::Weather)] = v;
                if (v) state.weather_cfg.enabled = true;   // showing it starts the fetcher
                state.weather_refresh = true;
            }),
        toggle("Show Precip Page",
            [&state, ipw]{ return state.info_panel.show[ipw(InfoWidget::WeatherPrecip)]; },
            [&state, ipw](bool v){
                state.info_panel.show[ipw(InfoWidget::WeatherPrecip)] = v;
                if (v) state.weather_cfg.enabled = true;
                state.weather_refresh = true;
            }),
        toggle("Auto-Locate (IP)",
            [&state]{ return state.weather_cfg.auto_locate; },
            [&state](bool v){ state.weather_cfg.auto_locate = v; state.weather_refresh = true; }),
        toggle("Units: Metric",
            [&state]{ return state.weather_cfg.metric; },
            [&state](bool v){ state.weather_cfg.metric = v; state.weather_refresh = true; }),
        leaf("Refresh Now", [&state]{ state.weather_refresh = true; }),
    };
    std::vector<MenuItem> info_panel_menu = {
        // (Always visible for now — enable is forced on in apply_hud_dock, like the minimap.)
        slider("Cycle Seconds", 2.f, 30.f, 1.f, "s",
            [&state]{ return state.info_panel.cycle_sec; },
            [&state](float v){ state.info_panel.cycle_sec = v; }),
        submenu("Clock",         std::move(ip_clock_menu)),
        submenu("Notifications", std::move(ip_notif_menu)),
        with_panel(
            submenu("Schedule", std::move(ip_schedule_menu)),
            "Schedule",
            [state_ptr](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                if (!state_ptr) return;
                const float lh = 18.f;
                ImVec2 p = o;
                auto line = [&](const std::string& s, ImU32 c) {
                    dl->AddText({ p.x, p.y }, c, s.c_str()); p.y += lh;
                };
                SchedulerStatus st;
                { std::lock_guard<std::mutex> lk(state_ptr->mtx);
                  st = state_ptr->scheduler_status; }
                const ImU32 dim = IM_COL32(180, 180, 180, 200);
                const ImU32 ok  = IM_COL32(160, 220, 160, 220);
                const ImU32 bad = IM_COL32(220, 160,  90, 220);
                const ImU32 wht = IM_COL32(230, 230, 230, 230);
                line(std::string("Daemon: ") + (st.daemon_ok ? "online" : "offline"),
                     st.daemon_ok ? ok : bad);
                line(std::string("Web: ") +
                         (st.web_url.empty() ? "(starting...)" : st.web_url),
                     st.web_url.empty() ? dim : wht);
                line("Scan this QR on your phone:", dim);
                line(std::string("Google: ") + st.gcal_state,
                     st.gcal_state == "connected" ? ok : dim);
                if (st.gcal_state == "pending" && !st.gcal_user_code.empty()) {
                    line(std::string("  code: ") + st.gcal_user_code, wht);
                    line(std::string("  at: ")   + st.gcal_verify_url, dim);
                }

                // QR code of the web URL — encoded once per URL, drawn as modules.
                if (!st.web_url.empty()) {
                    static std::string qr_url;
                    static std::vector<std::vector<bool>> qr_mods;
                    if (qr_url != st.web_url) {
                        qr_url = st.web_url;
                        qr_mods.clear();
                        try {
                            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
                                st.web_url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                            const int qn = qr.getSize();
                            qr_mods.assign(qn, std::vector<bool>(qn, false));
                            for (int yy = 0; yy < qn; ++yy)
                                for (int xx = 0; xx < qn; ++xx)
                                    qr_mods[yy][xx] = qr.getModule(xx, yy);
                        } catch (...) { qr_mods.clear(); }
                    }
                    if (!qr_mods.empty()) {
                        const int   qn    = static_cast<int>(qr_mods.size());
                        const int   quiet = 3;
                        const float avail = std::min(sz.x, sz.y - (p.y - o.y) - 6.f);
                        const float box   = std::max(60.f, std::min(190.f, avail));
                        const float mod   = box / (qn + quiet * 2);
                        const float qx = o.x, qy = p.y + 4.f;
                        const float full = mod * (qn + quiet * 2);
                        dl->AddRectFilled({qx, qy}, {qx + full, qy + full},
                                          IM_COL32(255, 255, 255, 255));
                        for (int yy = 0; yy < qn; ++yy)
                            for (int xx = 0; xx < qn; ++xx)
                                if (qr_mods[yy][xx]) {
                                    const float mx = qx + mod * (xx + quiet);
                                    const float my = qy + mod * (yy + quiet);
                                    dl->AddRectFilled({mx, my}, {mx + mod, my + mod},
                                                      IM_COL32(0, 0, 0, 255));
                                }
                    }
                }
            }),
        submenu("Weather",       std::move(ip_weather_menu)),
    };

    // ── Location — shared dock for both the minimap and info panel ─────────────
    std::vector<MenuItem> location_menu = {
        leaf_sel("Top",
            [&state]{ state.hud_dock.bottom = false; apply_hud_dock(state); },
            [&state]{ return !state.hud_dock.bottom; }),
        leaf_sel("Bottom",
            [&state]{ state.hud_dock.bottom = true; apply_hud_dock(state); },
            [&state]{ return state.hud_dock.bottom; }),
        leaf("Move Up (+5)",
            [&state]{ state.hud_dock.v_offset -= 5.f; apply_hud_dock(state); }),
        leaf("Move Down (-5)",
            [&state]{ state.hud_dock.v_offset += 5.f; apply_hud_dock(state); }),
    };

    // Legacy HUD group: the master toggle plus the legacy-only chrome settings
    // (Flip to Top + the compass tape). Everything except the toggle is hidden
    // until Legacy HUD is on, so the modular-HUD user never sees dead options.
    std::vector<MenuItem> legacy_hud_menu;
    legacy_hud_menu.push_back(with_desc(toggle("Legacy HUD",
        [&state]{ return state.legacy_hud; },
        [&state](bool v){ state.legacy_hud = v; }),
        "ON: show the legacy edge/corner HUD (compass tape, health indicators, "
        "face indicator, corner clock/timer, LoRa messages). OFF: show only the "
        "modular HUD \xe2\x80\x94 the minimap and info panel."));
    {
        MenuItem flip = with_desc(toggle("Flip to Top",
            [hud_cfg]{ return hud_cfg->hud_flip_vertical; },
            [hud_cfg](bool v){ hud_cfg->hud_flip_vertical = v; }),
            "Mirror the legacy HUD chrome to the top edge instead of the bottom.");
        flip.visible_fn = [&state]{ return state.legacy_hud; };
        legacy_hud_menu.push_back(std::move(flip));
        MenuItem comp = submenu("Compass", std::move(compass_menu));
        comp.visible_fn = [&state]{ return state.legacy_hud; };
        legacy_hud_menu.push_back(std::move(comp));
    }

    std::vector<MenuItem> hud_menu = {
        submenu("Mini-Map Module",  std::move(mini_map_menu)),
        submenu("Info-Panel Module",std::move(info_panel_menu)),
        submenu("Location",         std::move(location_menu)),
        submenu("Clock",            std::move(clock_menu)),
        submenu("Color",            std::move(color_options_menu)),
        submenu("Menu Position",    std::move(menu_position_menu)),
        submenu("Legacy HUD",       std::move(legacy_hud_menu)),
    };

    // ── Vision Assist (post-processing depth cues) ────────────────────────────

    std::vector<MenuItem> edge_strength_menu = {
        leaf_sel("10%",  [&state]{ state.pp_cfg.edge_strength = 0.10f; }, [&state]{ return state.pp_cfg.edge_strength == 0.10f; }),
        leaf_sel("30%",  [&state]{ state.pp_cfg.edge_strength = 0.30f; }, [&state]{ return state.pp_cfg.edge_strength == 0.30f; }),
        leaf_sel("50%",  [&state]{ state.pp_cfg.edge_strength = 0.50f; }, [&state]{ return state.pp_cfg.edge_strength == 0.50f; }),
        leaf_sel("70%",  [&state]{ state.pp_cfg.edge_strength = 0.70f; }, [&state]{ return state.pp_cfg.edge_strength == 0.70f; }),
        leaf_sel("100%", [&state]{ state.pp_cfg.edge_strength = 1.00f; }, [&state]{ return state.pp_cfg.edge_strength == 1.00f; }),
    };

    std::vector<MenuItem> edge_color_menu = {
        leaf_sel("Orange", [&state]{ state.pp_cfg.edge_color = IM_COL32(255, 160,  32, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32(255, 160,  32, 255); }),
        leaf_sel("Teal",   [&state]{ state.pp_cfg.edge_color = IM_COL32(  0, 220, 180, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32(  0, 220, 180, 255); }),
        leaf_sel("Cyan",   [&state]{ state.pp_cfg.edge_color = IM_COL32(  0, 180, 255, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32(  0, 180, 255, 255); }),
        leaf_sel("Green",  [&state]{ state.pp_cfg.edge_color = IM_COL32( 30, 220,  60, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32( 30, 220,  60, 255); }),
        leaf_sel("White",  [&state]{ state.pp_cfg.edge_color = IM_COL32(255, 255, 255, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32(255, 255, 255, 255); }),
        leaf_sel("Black",  [&state]{ state.pp_cfg.edge_color = IM_COL32(  0,   0,   0, 255); }, [&state]{ return state.pp_cfg.edge_color == IM_COL32(  0,   0,   0, 255); }),
    };

    std::vector<MenuItem> edge_detail_menu = {
        leaf_sel("Ultra Fine (0.5x)", [&state]{ state.pp_cfg.edge_scale = 0.5f; }, [&state]{ return state.pp_cfg.edge_scale == 0.5f; }),
        leaf_sel("Fine (1x)",         [&state]{ state.pp_cfg.edge_scale = 1.0f; }, [&state]{ return state.pp_cfg.edge_scale == 1.0f; }),
        leaf_sel("Standard (2x)",     [&state]{ state.pp_cfg.edge_scale = 2.0f; }, [&state]{ return state.pp_cfg.edge_scale == 2.0f; }),
        leaf_sel("Coarse (3x)",       [&state]{ state.pp_cfg.edge_scale = 3.0f; }, [&state]{ return state.pp_cfg.edge_scale == 3.0f; }),
        leaf_sel("Silhouette (5x)",   [&state]{ state.pp_cfg.edge_scale = 5.0f; }, [&state]{ return state.pp_cfg.edge_scale == 5.0f; }),
    };

    std::vector<MenuItem> edge_threshold_menu = {
        leaf_sel("None",   [&state]{ state.pp_cfg.edge_threshold = 0.00f; }, [&state]{ return state.pp_cfg.edge_threshold == 0.00f; }),
        leaf_sel("Low",    [&state]{ state.pp_cfg.edge_threshold = 0.15f; }, [&state]{ return state.pp_cfg.edge_threshold == 0.15f; }),
        leaf_sel("Medium", [&state]{ state.pp_cfg.edge_threshold = 0.30f; }, [&state]{ return state.pp_cfg.edge_threshold == 0.30f; }),
        leaf_sel("High",   [&state]{ state.pp_cfg.edge_threshold = 0.50f; }, [&state]{ return state.pp_cfg.edge_threshold == 0.50f; }),
    };

    std::vector<MenuItem> desat_strength_menu = {
        leaf_sel("25%",  [&state]{ state.pp_cfg.desat_strength = 0.25f; }, [&state]{ return state.pp_cfg.desat_strength == 0.25f; }),
        leaf_sel("50%",  [&state]{ state.pp_cfg.desat_strength = 0.50f; }, [&state]{ return state.pp_cfg.desat_strength == 0.50f; }),
        leaf_sel("75%",  [&state]{ state.pp_cfg.desat_strength = 0.75f; }, [&state]{ return state.pp_cfg.desat_strength == 0.75f; }),
        leaf_sel("100%", [&state]{ state.pp_cfg.desat_strength = 1.00f; }, [&state]{ return state.pp_cfg.desat_strength == 1.00f; }),
    };

    std::vector<MenuItem> bg_threshold_menu = {
        leaf_sel("Subtle (0.25)",     [&state]{ state.pp_cfg.contrast_threshold = 0.25f; }, [&state]{ return state.pp_cfg.contrast_threshold == 0.25f; }),
        leaf_sel("Medium (0.15)",     [&state]{ state.pp_cfg.contrast_threshold = 0.15f; }, [&state]{ return state.pp_cfg.contrast_threshold == 0.15f; }),
        leaf_sel("Aggressive (0.07)", [&state]{ state.pp_cfg.contrast_threshold = 0.07f; }, [&state]{ return state.pp_cfg.contrast_threshold == 0.07f; }),
    };

    std::vector<MenuItem> focus_blend_menu = {
        leaf_sel("Off",    [&state]{ state.pp_cfg.focus_str = 0.0f; }, [&state]{ return state.pp_cfg.focus_str == 0.0f; }),
        leaf_sel("Low",    [&state]{ state.pp_cfg.focus_str = 0.3f; }, [&state]{ return state.pp_cfg.focus_str == 0.3f; }),
        leaf_sel("Medium", [&state]{ state.pp_cfg.focus_str = 0.6f; }, [&state]{ return state.pp_cfg.focus_str == 0.6f; }),
        leaf_sel("Full",   [&state]{ state.pp_cfg.focus_str = 1.0f; }, [&state]{ return state.pp_cfg.focus_str == 1.0f; }),
    };

    std::vector<MenuItem> size_filter_menu = {
        leaf_sel("Off",           [&state]{ state.pp_cfg.edge_gate_scale =  0.0f; }, [&state]{ return state.pp_cfg.edge_gate_scale ==  0.0f; }),
        leaf_sel("Tiny (5x)",     [&state]{ state.pp_cfg.edge_gate_scale =  5.0f; }, [&state]{ return state.pp_cfg.edge_gate_scale ==  5.0f; }),
        leaf_sel("Small (10x)",   [&state]{ state.pp_cfg.edge_gate_scale = 10.0f; }, [&state]{ return state.pp_cfg.edge_gate_scale == 10.0f; }),
        leaf_sel("Medium (20x)",  [&state]{ state.pp_cfg.edge_gate_scale = 20.0f; }, [&state]{ return state.pp_cfg.edge_gate_scale == 20.0f; }),
        leaf_sel("Large (40x)",   [&state]{ state.pp_cfg.edge_gate_scale = 40.0f; }, [&state]{ return state.pp_cfg.edge_gate_scale == 40.0f; }),
    };

    std::vector<MenuItem> motion_sensitivity_menu = {
        leaf_sel("Low",       [&state]{ state.pp_cfg.motion_thresh = 0.10f; }, [&state]{ return state.pp_cfg.motion_thresh == 0.10f; }),
        leaf_sel("Medium",    [&state]{ state.pp_cfg.motion_thresh = 0.04f; }, [&state]{ return state.pp_cfg.motion_thresh == 0.04f; }),
        leaf_sel("High",      [&state]{ state.pp_cfg.motion_thresh = 0.02f; }, [&state]{ return state.pp_cfg.motion_thresh == 0.02f; }),
        leaf_sel("Very High", [&state]{ state.pp_cfg.motion_thresh = 0.01f; }, [&state]{ return state.pp_cfg.motion_thresh == 0.01f; }),
    };

    std::vector<MenuItem> motion_mode_menu = {
        leaf_sel("Fine Line", [&state]{ state.pp_cfg.motion_line = 1.0f; }, [&state]{ return state.pp_cfg.motion_line == 1.0f; }),
        leaf_sel("Fill",      [&state]{ state.pp_cfg.motion_line = 0.0f; }, [&state]{ return state.pp_cfg.motion_line == 0.0f; }),
    };

    std::vector<MenuItem> motion_spread_menu = {
        leaf_sel("Tight (2px)",  [&state]{ state.pp_cfg.motion_radius =  2.0f; }, [&state]{ return state.pp_cfg.motion_radius ==  2.0f; }),
        leaf_sel("Close (4px)",  [&state]{ state.pp_cfg.motion_radius =  4.0f; }, [&state]{ return state.pp_cfg.motion_radius ==  4.0f; }),
        leaf_sel("Medium (8px)", [&state]{ state.pp_cfg.motion_radius =  8.0f; }, [&state]{ return state.pp_cfg.motion_radius ==  8.0f; }),
        leaf_sel("Wide (16px)",  [&state]{ state.pp_cfg.motion_radius = 16.0f; }, [&state]{ return state.pp_cfg.motion_radius == 16.0f; }),
    };

    std::vector<MenuItem> motion_color_menu = {
        leaf_sel("Green",  [&state]{ state.pp_cfg.motion_color = IM_COL32(  0, 255, 100, 255); }, [&state]{ return state.pp_cfg.motion_color == IM_COL32(  0, 255, 100, 255); }),
        leaf_sel("Cyan",   [&state]{ state.pp_cfg.motion_color = IM_COL32(  0, 200, 255, 255); }, [&state]{ return state.pp_cfg.motion_color == IM_COL32(  0, 200, 255, 255); }),
        leaf_sel("Yellow", [&state]{ state.pp_cfg.motion_color = IM_COL32(255, 220,   0, 255); }, [&state]{ return state.pp_cfg.motion_color == IM_COL32(255, 220,   0, 255); }),
        leaf_sel("White",  [&state]{ state.pp_cfg.motion_color = IM_COL32(255, 255, 255, 255); }, [&state]{ return state.pp_cfg.motion_color == IM_COL32(255, 255, 255, 255); }),
        leaf_sel("Orange", [&state]{ state.pp_cfg.motion_color = IM_COL32(255, 140,   0, 255); }, [&state]{ return state.pp_cfg.motion_color == IM_COL32(255, 140,   0, 255); }),
    };

    // EMA update rate: how fast the reference frame tracks the scene.
    // Lower = reference lags further behind = outline lingers longer after motion stops.
    std::vector<MenuItem> motion_trail_menu = {
        leaf_sel("Instant (1 frame)",  [&state]{ state.pp_cfg.motion_update_rate = 1.00f; }, [&state]{ return state.pp_cfg.motion_update_rate == 1.00f; }),
        leaf_sel("Short  (~3 frames)", [&state]{ state.pp_cfg.motion_update_rate = 0.50f; }, [&state]{ return state.pp_cfg.motion_update_rate == 0.50f; }),
        leaf_sel("Medium (~8 frames)", [&state]{ state.pp_cfg.motion_update_rate = 0.20f; }, [&state]{ return state.pp_cfg.motion_update_rate == 0.20f; }),
        leaf_sel("Long   (~15 frames)",[&state]{ state.pp_cfg.motion_update_rate = 0.08f; }, [&state]{ return state.pp_cfg.motion_update_rate == 0.08f; }),
        leaf_sel("Extended (~30fr)",   [&state]{ state.pp_cfg.motion_update_rate = 0.03f; }, [&state]{ return state.pp_cfg.motion_update_rate == 0.03f; }),
    };

    std::vector<MenuItem> edge_menu = {
        toggle("Edge Highlight",
            [&state]{ return state.pp_cfg.edge_enabled; },
            [&state](bool v){ state.pp_cfg.edge_enabled = v; }),
        submenu("Strength",  std::move(edge_strength_menu)),
        submenu("Color",     std::move(edge_color_menu)),
        submenu("Detail",    std::move(edge_detail_menu)),
        submenu("Threshold", std::move(edge_threshold_menu)),
        submenu("Size Filter", std::move(size_filter_menu)),
    };

    std::vector<MenuItem> motion_menu = {
        toggle("Motion Highlight",
            [&state]{ return state.pp_cfg.motion_enabled; },
            [&state](bool v){ state.pp_cfg.motion_enabled = v; }),
        submenu("Mode",        std::move(motion_mode_menu)),
        submenu("Sensitivity", std::move(motion_sensitivity_menu)),
        submenu("Spread",      std::move(motion_spread_menu)),
        submenu("Trail",       std::move(motion_trail_menu)),
        submenu("Color",       std::move(motion_color_menu)),
    };

    std::vector<MenuItem> desat_menu = {
        toggle("Bg Desaturate",
            [&state]{ return state.pp_cfg.desat_enabled; },
            [&state](bool v){ state.pp_cfg.desat_enabled = v; }),
        submenu("Strength",    std::move(desat_strength_menu)),
        submenu("BG Threshold", std::move(bg_threshold_menu)),
        submenu("Focus Blend", std::move(focus_blend_menu)),
        slider("Color Protect", 0.f, 100.f, 10.f, " %",
            [&state]{ return state.pp_cfg.color_protect * 100.f; },
            [&state](float v){ state.pp_cfg.color_protect = v / 100.f; }),
        slider("Edge Expand", 0.f, 3.f, 0.5f, "",
            [&state]{ return state.pp_cfg.edge_dilate; },
            [&state](float v){ state.pp_cfg.edge_dilate = v; }),
    };

    std::vector<MenuItem> vision_menu = {
        toggle("Edge Highlight",
            [&state]{ return state.pp_cfg.edge_enabled; },
            [&state](bool v){ state.pp_cfg.edge_enabled = v; }),
        toggle("Motion Highlight",
            [&state]{ return state.pp_cfg.motion_enabled; },
            [&state](bool v){ state.pp_cfg.motion_enabled = v; }),
        toggle("Bg Desaturate",
            [&state]{ return state.pp_cfg.desat_enabled; },
            [&state](bool v){ state.pp_cfg.desat_enabled = v; }),
        submenu("Edge Highlight", std::move(edge_menu)),
        submenu("Motion Highlight", std::move(motion_menu)),
        submenu("Bg Desaturate",  std::move(desat_menu)),
    };

    cameras_menu.push_back(submenu("Android Mirror", std::move(android_menu)));
    cameras_menu.push_back(submenu("Vision Assist",  std::move(vision_menu)));

    // ── LoRa menu ─────────────────────────────────────────────────────────────

    // Helper: push a notification to the live queue (thread-safe).
    // Defined here so both the LoRa menu and the Demo Mode menu can capture it.
    auto push_notif = [state_ptr](NotifType type, std::string title, std::string body,
                                   float auto_dismiss_s,
                                   std::vector<NotifAction> actions = {}) {
        if (!state_ptr) return;
        Notification n;
        n.type           = type;
        n.title          = std::move(title);
        n.body           = std::move(body);
        n.timestamp      = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
        n.auto_dismiss_s = auto_dismiss_s;
        n.actions        = std::move(actions);
        std::lock_guard<std::mutex> lk(state_ptr->mtx);
        state_ptr->notifs.push(std::move(n));
    };

    // Node lookup: one leaf per possible node slot (1–8); hidden when no data.
    // Selecting pushes an App toast with live telemetry at that moment.
    std::vector<MenuItem> lora_nodes_menu;
    for (int n = 1; n <= 8; ++n) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "Node %d", n);
        lora_nodes_menu.push_back({
            .label      = lbl,
            .type       = MenuItemType::LEAF,
            .action     = [n, state_ptr, push_notif]{
                if (!state_ptr) return;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                for (const auto& nd : state_ptr->lora_nodes) {
                    if (nd.local_id != static_cast<uint8_t>(n)) continue;
                    char body[128];
                    char since[32] = "never";
                    if (nd.last_seen) {
                        int secs = static_cast<int>(time(nullptr) - nd.last_seen);
                        if (secs < 60)       snprintf(since, sizeof(since), "%ds ago", secs);
                        else if (secs < 3600) snprintf(since, sizeof(since), "%dm ago", secs/60);
                        else                  snprintf(since, sizeof(since), "%dh ago", secs/3600);
                    }
                    snprintf(body, sizeof(body),
                             "%.0fm  %.0f\xC2\xB0  RSSI %d dBm  SNR %d  %s",
                             static_cast<double>(nd.distance_m),
                             static_cast<double>(nd.heading_deg),
                             static_cast<int>(nd.rssi),
                             static_cast<int>(nd.snr),
                             since);
                    std::string title = nd.name.empty()
                                        ? std::string("Node ") + std::to_string(n)
                                        : nd.name;
                    push_notif(NotifType::LoRa, std::move(title), body, 6.f);
                    return;
                }
                // Node slot known to menu but not yet seen
                push_notif(NotifType::App, std::string("Node ") + std::to_string(n),
                           "No data received yet", 4.f);
            },
            .visible_fn = [n, state_ptr]{
                if (!state_ptr) return false;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                for (const auto& nd : state_ptr->lora_nodes)
                    if (nd.local_id == static_cast<uint8_t>(n)) return true;
                return false;
            },
        });
    }

    std::vector<MenuItem> lora_menu = {
        leaf("Clear Messages",      [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->lora_messages.clear();
        }),
        leaf("Clear Notifications", [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->notifs.dismiss_all();
        }),
        submenu("Lookup Node",      std::move(lora_nodes_menu)),
    };

    // ── System / dev menu ─────────────────────────────────────────────────────

    std::vector<MenuItem> demo_menu = {
        leaf("Trigger Alarm", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->timer_alarm.alarm_triggered = true; }
            push_notif(NotifType::Alarm, "Test Alarm", "Demo alarm fired", 0.f,
                {{"DISMISS", [](AppState& s){ s.timer_alarm.alarm_triggered = false; }}});
        }),
        leaf("Trigger Timer Done", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->timer_alarm.timer_triggered = true; }
            push_notif(NotifType::Timer, "Timer Done", "Demo 5:00 timer expired", 0.f,
                {{"DISMISS", [](AppState& s){ s.timer_alarm.timer_triggered = false; }},
                 {"+2 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+120; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }},
                 {"+5 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+300; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }}});
        }),
        leaf("LoRa Message (Node-2)", [state_ptr, push_notif]{
            if (!state_ptr) return;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx);
              state_ptr->push_lora_message({2, time(nullptr), "Demo: en route to objective"}); }
            push_notif(NotifType::LoRa, "Node-2", "Demo: en route to objective", 8.f);
        }),
        leaf("App Toast", [push_notif]{
            push_notif(NotifType::App, "ProtoHUD", "Demo app notification", 4.f);
        }),
        leaf("Toast Stack (x4)", [push_notif]{
            push_notif(NotifType::Alarm, "Alarm",  "Demo alarm",  0.f);
            push_notif(NotifType::Timer, "Timer",  "Demo timer",  0.f);
            push_notif(NotifType::LoRa,  "Node-3", "Demo LoRa",  8.f);
            push_notif(NotifType::App,   "System", "Demo app",   4.f);
        }),
        leaf("Clear All", [state_ptr]{
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->notifs.dismiss_all();
            state_ptr->timer_alarm.alarm_triggered = false;
            state_ptr->timer_alarm.timer_triggered = false;
        }),
    };

    // Software gets its contents (Updates, Demo Mode, Profiles & Backup) appended
    // below once those menus are built; the old "Check for Updates" / "Pull &&
    // Rebuild" shell shortcuts were removed in favour of the in-HUD Updater.
    std::vector<MenuItem> software_menu;

    std::vector<MenuItem> fps_interval_menu = {
        leaf_sel("1 second",  [state_ptr]{ state_ptr->fps_avg_interval_s = 1;  }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 1;  }),
        leaf_sel("5 seconds", [state_ptr]{ state_ptr->fps_avg_interval_s = 5;  }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 5;  }),
        leaf_sel("10 seconds",[state_ptr]{ state_ptr->fps_avg_interval_s = 10; }, [state_ptr]{ return state_ptr->fps_avg_interval_s == 10; }),
    };

    std::vector<MenuItem> i2c_bus_menu = {
        leaf_sel("/dev/i2c-0", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-0"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-0"; }),
        leaf_sel("/dev/i2c-1", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-1"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-1"; }),
        leaf_sel("/dev/i2c-2", [state_ptr]{ std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_bus = "/dev/i2c-2"; }, [state_ptr]{ return state_ptr->i2c_scan_bus == "/dev/i2c-2"; }),
    };

    std::vector<MenuItem> diagnostics_menu = {
        leaf("Scan I2C Bus", [state_ptr]{
            bool already;
            { std::lock_guard<std::mutex> lk(state_ptr->mtx); already = state_ptr->i2c_scan_busy; }
            if (!already) {
                { std::lock_guard<std::mutex> lk(state_ptr->mtx); state_ptr->i2c_scan_busy = true; state_ptr->i2c_scan_results.clear(); }
                std::thread(run_i2c_scan, state_ptr).detach();
            }
        }),
        submenu("I2C Bus",    std::move(i2c_bus_menu)),
    };

    // ── HUD / Menu Presets ───────────────────────────────────────────────────────
    // Visual-only presets (HUD colors + menu accent/border/selection/scale). Built-in
    // coordinated themes apply instantly; custom ones are saved/loaded live (no
    // restart, unlike full Profiles). Load/delete lists are dynamic (label_fn reads
    // the manager live). kProfileSlots = max dynamic rows shown for save/load lists.
    constexpr int kProfileSlots = 16;
    std::vector<MenuItem> hud_preset_delete_menu;
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.visible_fn  = [hud_presets, i]{ return hud_presets && i < hud_presets->count(); };
        m.description = "Delete this HUD/menu preset permanently.";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_delete_name = nm;
        };
        hud_preset_delete_menu.push_back(std::move(m));
    }

    std::vector<MenuItem> hud_presets_menu;
    // Built-in coordinated themes (instant apply).
    for (auto& t : make_builtin_theme_leaves())
        hud_presets_menu.push_back(with_desc(std::move(t),
            "Built-in theme: applies a coordinated HUD color + menu style instantly."));
    // Save current look as a named preset (on-screen keyboard).
    hud_presets_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Preset Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->hud_preset_save_name = name;
                });
        }),
        "Save the current HUD colors + menu style (accent, border, selection, UI "
        "scale) as a named preset. Applies live when loaded — no restart."));
    // Dynamic load slots.
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.visible_fn  = [hud_presets, i]{ return hud_presets && i < hud_presets->count(); };
        m.description = "Apply this HUD/menu preset (live, no restart).";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_load_name = nm;
        };
        hud_presets_menu.push_back(std::move(m));
    }
    hud_presets_menu.push_back(with_desc(submenu("Delete Preset", std::move(hud_preset_delete_menu)),
        "Remove a saved HUD/menu preset permanently."));

    // ── GPIO Visualizer ──────────────────────────────────────────────────────
    // Renders the Pi 5 / CM5 40-pin header in the context pane as a vertical
    // 2x20 grid, colour-coded per Pi Foundation pinout. Each pin has a list
    // of claimants (the things in cfg that use it); pins with one claimant
    // get a green outline, pins with two or more get a red "conflict"
    // outline, idle pins stay grey. Hover any pin to see its claimants,
    // bus speed (SPI), I²C address list, and any user note assigned to it.
    struct GpioVizState {
        bool show_pin_num    = true;
        bool show_primary    = true;
        bool show_secondary  = true;
        bool show_tertiary   = false;
        bool show_inactive   = true;
        bool show_user_notes = true;   // 4th column when cfg["gpio_user_notes"] is set
        bool show_live_state = false;  // small H/L badge from /dev/gpiochip0 readings
        int  filter_kind     = -1;     // -1 = no filter, else PinKind enum value
    };
    static GpioVizState gpio_viz;

    // (BCM) → list of human-readable claimants (e.g. "MAX7219 left.spi",
    // "BNO055 IMU", "HUB75 R1"). Pins with >1 claimant are conflicts.
    struct PinClaims {
        std::map<int, std::vector<std::string>> claimants;
        std::map<int, int>                      spi_speed_hz;   // BCM → bus speed (for MOSI lines)
    };
    auto gpio_pin_claims = [cfg_root, gpio_pins_p, gpio_slot_count]() -> PinClaims {
        PinClaims pc;
        if (!cfg_root) return pc;
        const json& cfg = *cfg_root;
        auto add = [&pc](int bcm, std::string who) {
            if (bcm >= 0) pc.claimants[bcm].push_back(std::move(who));
        };
        // Configurable GPIO switch map — claim each assigned pin, labelled with
        // its (short-press, else long-press) function. Reads the live slots so
        // unsaved menu edits show up immediately.
        if (gpio_pins_p) {
            for (int i = 0; i < gpio_slot_count; ++i) {
                const auto& s = gpio_pins_p[i];
                const bool used = s.short_fn != input::GpioFunc::None ||
                                  s.long_fn  != input::GpioFunc::None;
                if (s.gpio >= 0 && used) {
                    const auto fn = (s.short_fn != input::GpioFunc::None) ? s.short_fn : s.long_fn;
                    add(s.gpio, std::string("GPIO: ") + input::gpio_func_name(fn));
                }
            }
        }

        // I²C peripherals — each enabled sensor on /dev/i2c-1 claims SDA1/SCL1.
        auto add_i2c = [&](const char* key, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            add(2, label);
            add(3, label);
        };
        add_i2c("bno055",       "BNO055 IMU");
        add_i2c("mpu9250",      "MPU9250 IMU");
        add_i2c("boop",         "MPR121 boop");
        add_i2c("light_sensor", "BH1750 light");

        // SPI chains — every chain on /dev/spidev0.x claims SPI0 pins,
        // /dev/spidev1.x claims SPI1 pins. Record per-bus speed for the
        // hover tooltip and pick up GPIO-transport CS pins.
        auto walk_chains = [&](const json& jchains, const char* tag) {
            if (!jchains.is_array()) return;
            for (const auto& jc : jchains) {
                if (!jc.is_object()) continue;
                const std::string dev = jc.value("spi_device", std::string());
                const std::string name = jc.value("name", std::string("chain"));
                const std::string who  = std::string(tag) + " " + name;
                const int speed = jc.value("speed_hz", 1'000'000);
                if (dev.find("/dev/spidev0") == 0) {
                    add(10, who + " (MOSI)"); add(9,  who + " (MISO)");
                    add(11, who + " (SCLK)"); add(8,  who + " (CE0)");
                    add(7,  who + " (CE1)");
                    pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed);
                }
                if (dev.find("/dev/spidev1") == 0) {
                    add(20, who + " (MOSI)"); add(19, who + " (MISO)");
                    add(21, who + " (SCLK)"); add(16, who + " (CE2)");
                    add(17, who + " (CE1)");
                    pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed);
                }
                add(jc.value("gpio_cs_pin", -1), who + " (GPIO CS)");
            }
        };
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            if (jpf.contains("max7219") && jpf["max7219"].is_object()) {
                const auto& jm = jpf["max7219"];
                walk_chains(jm.contains("chains") ? jm["chains"] : json::array(),
                            "MAX7219");
                add(jm.value("gpio_din_pin", -1), "MAX7219 GPIO bus DIN");
                add(jm.value("gpio_clk_pin", -1), "MAX7219 GPIO bus CLK");
            }
            if (jpf.contains("rgb_matrix") && jpf["rgb_matrix"].is_object()
                && jpf["rgb_matrix"].contains("chains"))
                walk_chains(jpf["rgb_matrix"]["chains"], "RGB matrix");

            // HUB75 + piomatter — uses the Adafruit RGB HAT pinout by default
            // (the most common shipping config). We tag these so a HUB75 build
            // shows the full pin claim, conflicts included.
            const std::string be = jpf.value("backend", std::string("hub75"));
            if (be == "hub75") {
                struct HubPin { int bcm; const char* name; };
                static constexpr HubPin kHub75[] = {
                    { 5,"R1"},{13,"G1"},{ 6,"B1"},{12,"R2"},{16,"G2"},{23,"B2"},
                    {22,"A" },{26,"B" },{27,"C" },{20,"D" },{24,"E" },
                    { 4,"OE"},{17,"CLK"},{21,"STB"},
                };
                for (const auto& h : kHub75)
                    add(h.bcm, std::string("HUB75 ") + h.name);
            }
        }
        // Accessory LEDs (WS2812) — single MOSI line on their chosen spidev.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            const std::string dev = jal.value("spi_device", std::string());
            const int speed = jal.value("speed_hz", 2'400'000);
            if (dev.find("/dev/spidev0") == 0) { add(10, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed); }
            if (dev.find("/dev/spidev1") == 0) { add(20, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed); }
        }
        return pc;
    };

    // I²C peripheral list for the SDA/SCL hover tooltip. (addr, label).
    auto i2c_peripherals = [cfg_root]() -> std::vector<std::pair<int, std::string>> {
        std::vector<std::pair<int, std::string>> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        auto add_if = [&](const char* key, int default_addr, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            const int addr = jk.value("i2c_addr", default_addr);
            out.push_back({addr, label});
        };
        add_if("bno055",       0x28, "BNO055 IMU");
        add_if("mpu9250",      0x68, "MPU9250 IMU");
        add_if("boop",         0x5A, "MPR121 boop");
        add_if("light_sensor", 0x23, "BH1750 light");
        return out;
    };

    // User notes — free-form labels per BCM line via cfg["gpio_user_notes"]
    // ("17": "My LED strip"). Returned as a copy so the draw lambda doesn't
    // hold a json& back into main()'s cfg longer than necessary.
    auto user_notes = [cfg_root]() -> std::map<int, std::string> {
        std::map<int, std::string> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        if (!cfg.contains("gpio_user_notes") || !cfg["gpio_user_notes"].is_object())
            return out;
        for (const auto& [k, v] : cfg["gpio_user_notes"].items()) {
            try {
                const int bcm = std::stoi(k);
                if (v.is_string()) out[bcm] = v.get<std::string>();
            } catch (...) { /* skip non-int keys */ }
        }
        return out;
    };

    // Per-rail current estimate (mA) from known device draws. Anything not
    // in the table doesn't contribute — better silently zero than wildly
    // off-by-orders.
    auto rail_currents_mA = [cfg_root]() -> std::pair<int, int> {   // {3v3, 5v}
        int rail3 = 0, rail5 = 0;
        if (!cfg_root) return {rail3, rail5};
        const json& cfg = *cfg_root;
        auto enabled = [&cfg](const char* key) {
            return cfg.contains(key) && cfg[key].is_object()
                && cfg[key].value("enabled", false);
        };
        if (enabled("bno055"))        rail3 +=  12;
        if (enabled("mpu9250"))       rail3 +=   4;
        if (enabled("boop"))          rail3 +=  30;
        if (enabled("light_sensor"))  rail3 +=   1;
        // Accessory LEDs (WS2812) — assume ~20mA/LED at moderate brightness.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            int total = 0;
            if (jal.contains("zones") && jal["zones"].is_array())
                for (const auto& z : jal["zones"]) total += z.value("count", 0);
            rail5 += total * 20;
        }
        // MAX7219 chains — ~80 mA per module at full brightness.
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            const std::string be = jpf.value("backend", std::string("hub75"));
            int max7219_mods = 0;
            if (be == "max7219" && jpf.contains("max7219") && jpf["max7219"].is_object()
                && jpf["max7219"].contains("chains") && jpf["max7219"]["chains"].is_array()) {
                for (const auto& jc : jpf["max7219"]["chains"]) {
                    if (!jc.is_object()) continue;
                    if (jc.contains("module_positions") && jc["module_positions"].is_array())
                        max7219_mods += static_cast<int>(jc["module_positions"].size());
                    else
                        max7219_mods += jc.value("cols_chips", 1) * jc.value("rows_chips", 1);
                }
            }
            rail5 += max7219_mods * 80;
        }
        return {rail3, rail5};
    };

    // The MenuContextPanelDraw lambda holds all of build_menu's GPIO helpers by
    // value (each captures cfg by reference, owned by main() which outlives
    // the menu system). gpio_viz has static storage so we capture its address.
    GpioVizState* gpvz_draw = &gpio_viz;
    auto draw_gpio_visualizer =
        [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
         gpvz_draw]() -> MenuContextPanelDraw {
        return [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
                gpvz_draw]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            const PinClaims claims = gpio_pin_claims();
            const auto i2c         = i2c_peripherals();
            const auto notes       = user_notes();
            const auto [ma3, ma5]  = rail_currents_mA();
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            // Live-state reader — lazily opens /dev/gpiochip0 the first time
            // the toggle is enabled. Lines already claimed by another consumer
            // (SPI/I²C driver, piomatter, MAX7219 chain) silently fail to
            // claim, and read() returns -1 for those. Static so the fds
            // persist across draws.
            static sys::GpioInputReader reader;
            static bool reader_attempted = false;
            if (gpvz_draw->show_live_state && !reader_attempted) {
                // Only request lines NOT already claimed by another subsystem.
                // Requesting a pin piomatter is driving (HUB75 panels) steals it
                // and blanks the face — so skip anything in the claims map.
                std::vector<int> wanted;
                for (int i = 0; i < 28; ++i)
                    if (claims.claimants.find(i) == claims.claimants.end())
                        wanted.push_back(i);  // free BCM line
                reader.open(wanted);
                reader_attempted = true;
            }

            // Light rounded card + white border behind the visualizer, then
            // inset the working area so content never touches the edges.
            {
                const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
                const ImU32 card_border = IM_COL32(255, 255, 255, 255);
                dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
                dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            }
            o.x += 8.f; o.y += 6.f; sz.x -= 16.f; sz.y -= 12.f;
            const ImU32 text_dark = IM_COL32(22, 26, 32, 255);   // body text on the light card
            const ImU32 text_dim  = IM_COL32(78, 86, 96, 255);   // secondary text

            // Layout: a dedicated text column on the left (title, rail
            // estimate, hovered-pin details — whatever used to live in the
            // floating tooltip) and the 2x20 grid pushed to the right so
            // the two never overlap. Text column width is a fraction of the
            // pane that scales sensibly between docked + expanded views.
            const float label_fs   = std::max(9.f, fs * 0.72f);
            const float info_w_min = 180.f;
            const float info_w     = std::clamp(sz.x * 0.34f, info_w_min,
                                                 sz.x * 0.55f);
            const float info_x     = o.x;
            const float grid_pane_x = o.x + info_w + 8.f;
            const float grid_pane_w = std::max(120.f, sz.x - info_w - 8.f);

            // Left text column: title + rail-current estimate (top), then a
            // "Pin info" block that follows the hovered pin.
            dl->AddText(font, fs * 1.05f, {info_x, o.y},
                        text_dark, "GPIO Header");
            dl->AddText(font, fs * 0.7f, {info_x, o.y + fs * 1.05f},
                        text_dim, "Pi 5 / CM5 — 40-pin");
            char rail_buf[96];
            std::snprintf(rail_buf, sizeof(rail_buf),
                          "3.3V ~%d mA   5V ~%d mA", ma3, ma5);
            const ImU32 rail_col = (ma5 > 2500 || ma3 > 500)
                ? IM_COL32(200, 110,  20, 255) : text_dim;
            dl->AddText(font, fs * 0.75f, {info_x, o.y + fs * 1.85f},
                        text_dim, "Est. rail draw:");
            dl->AddText(font, fs * 0.85f, {info_x, o.y + fs * 2.65f},
                        rail_col, rail_buf);

            const float info_block_top = o.y + fs * 4.0f;

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, sz.y - 2.f * vmargin);
            const float row_h   = std::max(14.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = o.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;

            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 3.f;
            const float pin_right_x = center_x + 3.f;

            const ImU32 outline_active   = IM_COL32( 40, 170,  60, 255);
            const ImU32 outline_conflict = IM_COL32(220,  50,  35, 255);
            const ImU32 outline_inactive = IM_COL32( 95, 105, 115, 220);
            const ImU32 live_high_col    = IM_COL32( 40, 170,  60, 255);
            const ImU32 live_low_col     = IM_COL32(110, 120, 130, 230);

            // Hover state for the tooltip.
            const ImVec2 mp = ImGui::GetMousePos();
            int           hovered_bcm = -1;
            sys::PinKind  hovered_kind = sys::PinKind::Gpio;
            std::string   hovered_primary;
            ImVec2        hover_pos{};

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            auto note_for = [&notes](int bcm) -> const char* {
                if (bcm < 0) return nullptr;
                auto it = notes.find(bcm);
                return (it == notes.end()) ? nullptr : it->second.c_str();
            };
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                std::string fn = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (p.tertiary && p.tertiary[0]) { if (!fn.empty()) fn += " / "; fn += p.tertiary; }
                c2[idx] = fn;
                std::string cl;
                if (p.bcm >= 0) {
                    auto c = claims.claimants.find(p.bcm);
                    if (c != claims.claimants.end() && !c->second.empty()) cl = c->second.front();
                }
                const char* note = gpvz_draw->show_user_notes ? note_for(p.bcm) : nullptr;
                if (!cl.empty()) {
                    c3[idx]    = cl;
                    c3col[idx] = (cl.rfind("GPIO: ", 0) == 0) ? IM_COL32(120, 170, 235, 255)  // GPIO map
                                                              : IM_COL32(235, 180,  90, 255); // hardware
                } else if (note) {
                    c3[idx]    = note;
                    c3col[idx] = IM_COL32(150, 195, 235, 255);                                 // user note
                }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float side_w_l = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col, bool dim) {
                if (t.empty() || w < 6.f) return;
                ImU32 bg = desc_bg;
                if (dim) { bg = (bg & 0x00FFFFFFu) | (110u << 24); col = (col & 0x00FFFFFFu) | (110u << 24); }
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left, bool dim) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col, dim);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col, dim); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx], dim);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col, dim);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col, dim);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx], dim);
                }
            };

            for (int row = 0; row < 20; ++row) {
                const sys::GpioPin& pl = sys::kPi40Pins[row * 2];     // odd phys (left col)
                const sys::GpioPin& pr = sys::kPi40Pins[row * 2 + 1]; // even phys (right col)
                const float y = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;

                auto draw_pin = [&](float x, const sys::GpioPin& p, bool right_side) {
                    const bool dim_by_filter = gpvz_draw->filter_kind >= 0 &&
                        static_cast<int>(p.kind) != gpvz_draw->filter_kind;
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (dim_by_filter)
                        fill = (fill & 0x00FFFFFFu) | (90u << 24);

                    auto it = (p.bcm >= 0) ? claims.claimants.find(p.bcm)
                                           : claims.claimants.end();
                    const bool is_active   = it != claims.claimants.end();
                    const bool is_conflict = is_active && it->second.size() > 1;
                    const ImU32 outline = is_conflict ? outline_conflict
                                       : is_active   ? outline_active
                                       : (gpvz_draw->show_inactive
                                          ? outline_inactive : 0);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    if (outline)
                        dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz},
                                    outline, 3.f, 0,
                                    is_conflict ? 3.f : (is_active ? 2.5f : 1.5f));
                    if (gpvz_draw->show_pin_num) {
                        char buf[6];
                        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                        const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, buf);
                        const float tx = x + (pin_sz - ts.x) * 0.5f;
                        const float ty = cy + (pin_sz - ts.y) * 0.5f;
                        // Bold white with an 8-direction black outline.
                        const ImU32 blk = IM_COL32(0, 0, 0, 235);
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -1; dy <= 1; ++dy)
                                if (dx || dy) dl->AddText(font, label_fs, {tx + dx, ty + dy}, blk, buf);
                        dl->AddText(font, label_fs, {tx, ty}, IM_COL32(255, 255, 255, 255), buf);
                    }
                    // Live state badge — small H/L dot inside the square's
                    // corner. Skipped for non-GPIO pins (power / GND / ID).
                    if (gpvz_draw->show_live_state && p.bcm >= 0) {
                        const int v = reader.read(p.bcm);
                        if (v >= 0) {
                            const ImU32 col = v ? live_high_col : live_low_col;
                            dl->AddCircleFilled({x + pin_sz - 4.f, cy + 4.f}, 2.5f, col);
                        }
                    }
                    // Hover detection (over square OR its description columns).
                    const float side_w = w1 + (w2 > 0.f ? w2 + colgap : 0.f)
                                            + (w3 > 0.f ? w3 + colgap : 0.f) + 6.f;
                    const float hit_l = right_side ? x : (x - side_w);
                    const float hit_r = right_side ? (x + pin_sz + side_w)
                                                   : (x + pin_sz);
                    if (mp.x >= hit_l && mp.x < hit_r &&
                        mp.y >= cy && mp.y < cy + pin_sz) {
                        hovered_bcm     = p.bcm;
                        hovered_kind    = p.kind;
                        hovered_primary = p.primary;
                        hover_pos       = {x + pin_sz * 0.5f, cy + pin_sz};
                    }
                };
                draw_pin(pin_left_x,  pl, false);
                draw_pin(pin_right_x, pr, true);

                const bool dim_l = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pl.kind) != gpvz_draw->filter_kind;
                const bool dim_r = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pr.kind) != gpvz_draw->filter_kind;

                // Three description columns on each side.
                draw_cols(row * 2,     cy, true,  dim_l);
                draw_cols(row * 2 + 1, cy, false, dim_r);
            }

            // Pin info — used to be a floating tooltip near the cursor;
            // now lives in the left text column so it never overlaps the
            // grid and shows even without active hover. Default copy
            // points the user at the grid.
            const bool   have_hover = hovered_bcm >= 0 || !hovered_primary.empty();
            const ImU32  hint_col   = text_dim;
            const ImU32  body_col   = text_dark;
            const float  info_lh    = label_fs + 2.f;
            float        info_y     = info_block_top;
            // Section header — coloured by conflict state when applicable.
            auto cit = claims.claimants.find(hovered_bcm);
            const bool conflict = (cit != claims.claimants.end()
                                   && cit->second.size() > 1);
            const ImU32 head_col = conflict ? outline_conflict
                                            : IM_COL32(40, 95, 165, 255);
            dl->AddText(font, fs * 0.8f, {info_x, info_y},
                        head_col, have_hover ? "Pin Info" : "Hover");
            info_y += fs * 1.0f;
            if (!have_hover) {
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "Hover a pin to see its");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "claimants, bus speed,");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "I\xc2\xb2""C peripherals, etc.");
            } else {
                std::vector<std::string> lines;
                lines.push_back(hovered_primary.empty() ? std::string("(unknown)")
                                                       : hovered_primary);
                if (cit != claims.claimants.end()) {
                    lines.push_back(conflict ? "CONFLICT — claimants:"
                                             : "Used by:");
                    for (const auto& w : cit->second) lines.push_back("  " + w);
                } else if (hovered_bcm >= 0) {
                    lines.push_back("(idle)");
                }
                auto sit = claims.spi_speed_hz.find(hovered_bcm);
                if (sit != claims.spi_speed_hz.end()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Bus speed: %.2f MHz",
                                  sit->second / 1e6);
                    lines.push_back(buf);
                }
                if (hovered_kind == sys::PinKind::I2c) {
                    lines.push_back("");
                    lines.push_back("I\xc2\xb2""C /dev/i2c-1");
                    if (i2c.empty()) lines.push_back("  (none enabled)");
                    else for (const auto& [addr, name] : i2c) {
                        char buf[80];
                        std::snprintf(buf, sizeof(buf), "  0x%02X  %s",
                                      addr, name.c_str());
                        lines.push_back(buf);
                    }
                }
                auto nit = notes.find(hovered_bcm);
                if (nit != notes.end()) {
                    lines.push_back("");
                    lines.push_back("Note: " + nit->second);
                }
                // Render top-down, stop if we'd overrun the pane.
                const float info_max_y = o.y + sz.y - info_lh;
                for (const auto& l : lines) {
                    if (info_y > info_max_y) break;
                    dl->AddText(font, label_fs, {info_x, info_y},
                                body_col, l.c_str());
                    info_y += info_lh;
                }
            }
            // Suppress the now-unused float-tooltip anchor (kept hover_pos
            // around in case we ever bring back an alt tooltip).
            (void)hover_pos;
        };
    };

    // Pinout export — dumps the current header view to a text file the user
    // can paste into a build journal. Path is /tmp/protohud_pinout.txt for
    // simplicity (no menu picker for the location).
    auto export_pinout = [gpio_pin_claims, i2c_peripherals,
                          user_notes, rail_currents_mA]() {
        const PinClaims pc       = gpio_pin_claims();
        const auto      i2c      = i2c_peripherals();
        const auto      notes    = user_notes();
        const auto [ma3, ma5]    = rail_currents_mA();
        const std::string path   = "/tmp/protohud_pinout.txt";
        std::ofstream f(path);
        if (!f) {
            std::fprintf(stderr, "[gpio] could not open %s for write\n", path.c_str());
            return;
        }
        f << "ProtoHUD GPIO header snapshot\n";
        f << "Estimated rail draw: 3.3V ~" << ma3 << " mA   5V ~" << ma5 << " mA\n\n";
        f << "Phys  BCM  Name      Alt1        Alt2       Used by / notes\n";
        f << "----  ---  --------  ----------  ---------  ----------------------------\n";
        for (const auto& p : sys::kPi40Pins) {
            char line[256];
            std::snprintf(line, sizeof(line), "%4d  %3d  %-8s  %-10s  %-9s  ",
                          static_cast<int>(p.physical),
                          static_cast<int>(p.bcm),
                          p.primary, p.secondary, p.tertiary);
            f << line;
            auto it = (p.bcm >= 0) ? pc.claimants.find(p.bcm) : pc.claimants.end();
            if (it != pc.claimants.end()) {
                if (it->second.size() > 1) f << "[CONFLICT] ";
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i) f << " | ";
                    f << it->second[i];
                }
            }
            auto nit = notes.find(p.bcm);
            if (nit != notes.end()) {
                if (it != pc.claimants.end()) f << "  ";
                f << "note=\"" << nit->second << "\"";
            }
            f << "\n";
        }
        if (!i2c.empty()) {
            f << "\nI2C bus /dev/i2c-1:\n";
            for (const auto& [addr, name] : i2c) {
                char ab[80];
                std::snprintf(ab, sizeof(ab), "  0x%02X  %s\n", addr, name.c_str());
                f << ab;
            }
        }
        f.close();
        std::cerr << "[gpio] pinout exported to " << path << "\n";
    };

    GpioVizState* gpvz = &gpio_viz;   // static storage — capture by pointer is safe

    // Filter chip — single-select radio. -1 = no filter (everything in colour).
    auto filter_radio = [gpvz](const char* lbl, int v) -> MenuItem {
        return leaf_sel(lbl,
            [gpvz, v]{ gpvz->filter_kind = v; },
            [gpvz, v]{ return gpvz->filter_kind == v; });
    };
    std::vector<MenuItem> filter_menu = {
        filter_radio("None",   -1),
        filter_radio("GPIO",    static_cast<int>(sys::PinKind::Gpio)),
        filter_radio("I\xc2\xb2""C",  static_cast<int>(sys::PinKind::I2c)),
        filter_radio("SPI",     static_cast<int>(sys::PinKind::Spi)),
        filter_radio("UART",    static_cast<int>(sys::PinKind::Uart)),
        filter_radio("PWM",     static_cast<int>(sys::PinKind::Pwm)),
        filter_radio("Clock",   static_cast<int>(sys::PinKind::Gpclk)),
        filter_radio("3.3V",    static_cast<int>(sys::PinKind::Power3V3)),
        filter_radio("5V",      static_cast<int>(sys::PinKind::Power5V)),
        filter_radio("Ground",  static_cast<int>(sys::PinKind::Ground)),
    };

    std::vector<MenuItem> gpio_viz_menu = {
        toggle("Show Pin Numbers",
            [gpvz]{ return gpvz->show_pin_num; },
            [gpvz](bool v){ gpvz->show_pin_num = v; }),
        toggle("Show Primary Function",
            [gpvz]{ return gpvz->show_primary; },
            [gpvz](bool v){ gpvz->show_primary = v; }),
        toggle("Show Secondary Function",
            [gpvz]{ return gpvz->show_secondary; },
            [gpvz](bool v){ gpvz->show_secondary = v; }),
        toggle("Show Tertiary Function",
            [gpvz]{ return gpvz->show_tertiary; },
            [gpvz](bool v){ gpvz->show_tertiary = v; }),
        toggle("Show User Notes",
            [gpvz]{ return gpvz->show_user_notes; },
            [gpvz](bool v){ gpvz->show_user_notes = v; }),
        toggle("Outline Inactive Pins",
            [gpvz]{ return gpvz->show_inactive; },
            [gpvz](bool v){ gpvz->show_inactive = v; }),
        toggle("Show Live State (H/L)",
            [gpvz]{ return gpvz->show_live_state; },
            [gpvz](bool v){ gpvz->show_live_state = v; }),
        with_desc(submenu("Filter by Function", std::move(filter_menu)),
                  "Dim every pin whose function family doesn't match the "
                  "selected filter — quick way to spot all SPI lines or "
                  "all PWM-capable pins."),
        leaf("Export Pinout...", export_pinout),
    };
    MenuItem gpio_viz_item = with_panel(
        submenu("GPIO Visualizer", std::move(gpio_viz_menu)),
        "Pi 40-pin Header", draw_gpio_visualizer());
    gpio_viz_item.description =
        "Pi Foundation–style 2x20 view of the GPIO header.\n"
        "  • Squares are colour-coded by function family.\n"
        "  • Green outline = pin claimed by one thing in config.json. "
        "Red outline = conflict (multiple claimants).\n"
        "  • Hover a pin for its claimants, SPI bus speed, I\xc2\xb2""C addresses, "
        "and any user note assigned to it.\n"
        "  • Live State badge (toggle) reads /dev/gpiochip0 for pins not "
        "already claimed by another driver.\n"
        "  • User notes live at cfg[\"gpio_user_notes\"][\"<bcm>\"] = "
        "\"label\" and surface as a 4th column.\n"
        "  • Export Pinout… dumps the current view to /tmp/protohud_pinout.txt.\n"
        "  • Power-rail estimate (top of pane) sums known device current draws.\n"
        "  • Filter by Function dims everything not matching the selected family.";

    // ── GPIO pin picker (visual header page) ──────────────────────────────────
    // Helpers shared by the picker page + the slot list:
    //   gpio_hw_claimants  — non-GPIO peripherals holding a pin (HUB75/SPI/I²C…).
    //   gpio_other_slot    — another in-use slot bound to a pin (-1 = none).
    //   gpio_pin_selectable— a real GPIO line not held by hardware. Pins already
    //                        used by another *slot* stay selectable (override
    //                        allowed); the clash is flagged afterwards.
    //   gpio_slot_conflict — a slot whose pin collides with another slot or with
    //                        hardware; the slot list paints it red.
    auto gpio_hw_claimants = [gpio_pin_claims](int bcm) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (bcm < 0) return out;
        PinClaims pc = gpio_pin_claims();
        auto it = pc.claimants.find(bcm);
        if (it != pc.claimants.end())
            for (const auto& who : it->second)
                if (who.rfind("GPIO: ", 0) != 0) out.push_back(who);   // skip GPIO-map self-claims
        return out;
    };
    auto gpio_other_slot = [gpio_pins_p, gpio_slot_count](int bcm, int self_slot) -> int {
        if (bcm < 0 || !gpio_pins_p) return -1;
        for (int j = 0; j < gpio_slot_count; ++j) {
            if (j == self_slot) continue;
            const auto& s = gpio_pins_p[j];
            const bool used = s.short_fn != input::GpioFunc::None ||
                              s.long_fn  != input::GpioFunc::None;
            if (used && s.gpio == bcm) return j;
        }
        return -1;
    };
    auto gpio_pin_selectable = [gpio_hw_claimants](int bcm) -> bool {
        return bcm >= 0 && gpio_hw_claimants(bcm).empty();
    };
    auto gpio_slot_conflict = [gpio_pins_p, gpio_other_slot, gpio_hw_claimants](int i) -> bool {
        if (!gpio_pins_p) return false;
        const auto& s = gpio_pins_p[i];
        const bool used = s.short_fn != input::GpioFunc::None ||
                          s.long_fn  != input::GpioFunc::None;
        if (!used || s.gpio < 0) return false;
        return gpio_other_slot(s.gpio, i) >= 0 || !gpio_hw_claimants(s.gpio).empty();
    };

    // Context-panel renderer for the picker page. Mirrors the GPIO Visualizer's
    // look — a left info column plus a centred 2x20 header grid — on a light
    // rounded card with a white border. Each pin carries an easy-to-read label:
    // its claimant ("HUB75 R1"), "Slot N" if another slot uses it, else its
    // GPIO/alt-function name. Marking: green outline = this slot's pin, blue =
    // another slot, amber = hardware peripheral, dark = the cursor.
    auto draw_pin_picker_panel =
        [gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
        (std::shared_ptr<int> focus, int self_slot,
         input::GpioPinCfg* slot_p) -> MenuContextPanelDraw {
        return [focus, self_slot, slot_p, gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs       = ImGui::GetFontSize();
            const float label_fs = std::max(9.f, fs * 0.72f);
            const int   fbcm     = focus  ? *focus      : -1;
            const int   assigned = slot_p ? slot_p->gpio : -1;

            // Light rounded card + white border behind the picker content.
            const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
            const ImU32 card_border = IM_COL32(255, 255, 255, 255);
            const ImU32 text_dark   = IM_COL32( 22,  26,  32, 255);
            const ImU32 text_dim    = IM_COL32( 78,  86,  96, 255);
            dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
            dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            const float cpad = 9.f;
            const ImVec2 co{o.x + cpad, o.y + cpad};
            const ImVec2 cs{sz.x - cpad * 2.f, sz.y - cpad * 2.f};

            // Per-pin state + claimant label, computed once.
            bool selectable[40]; int otherslot[40]; bool hardware[40];
            std::string claim[40];
            for (int idx = 0; idx < 40; ++idx) {
                const int b = sys::kPi40Pins[idx].bcm;
                auto hw = (b >= 0) ? gpio_hw_claimants(b) : std::vector<std::string>{};
                hardware[idx]   = !hw.empty();
                selectable[idx] = b >= 0 && !hardware[idx];
                otherslot[idx]  = (b >= 0) ? gpio_other_slot(b, self_slot) : -1;
                claim[idx]      = hw.empty() ? std::string() : hw.front();
            }

            const float info_w      = std::clamp(cs.x * 0.30f, 110.f, cs.x * 0.5f);
            const float info_x      = co.x;
            const float grid_pane_x = co.x + info_w + 6.f;
            const float grid_pane_w = std::max(120.f, cs.x - info_w - 6.f);

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, cs.y - 2.f * vmargin);
            const float row_h   = std::max(13.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = co.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;
            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 2.f;
            const float pin_right_x = center_x + 2.f;
            const float side_w_l    = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r    = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);

            const ImU32 out_focus    = IM_COL32( 15,  18,  24, 255);   // dark = cursor (pops on light)
            const ImU32 out_assigned = IM_COL32( 40, 170,  60, 255);   // this slot
            const ImU32 out_other    = IM_COL32( 40, 110, 210, 255);   // another slot
            const ImU32 out_hardware = IM_COL32(205, 120,  20, 255);   // hardware peripheral

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                c2[idx] = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (hardware[idx])            { c3[idx] = claim[idx];
                                                c3col[idx] = IM_COL32(235, 180, 90, 255); }
                else if (otherslot[idx] >= 0) { c3[idx] = "Slot " + std::to_string(otherslot[idx] + 1);
                                                c3col[idx] = IM_COL32(120, 170, 235, 255); }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {   // shrink to fit the side gap if the three columns are too wide
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col) {
                if (t.empty() || w < 6.f) return;
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, desc_bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx]);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx]);
                }
            };

            // 8-direction black outline — kept for the white pin numbers so they
            // read on bright cells (descriptions use the grey box instead).
            auto otext = [&](float x, float y, ImU32 col, const char* s) {
                const ImU32 blk = IM_COL32(0, 0, 0, 225);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (dx || dy) dl->AddText(font, label_fs, {x + dx, y + dy}, blk, s);
                dl->AddText(font, label_fs, {x, y}, col, s);
            };
            auto draw_pin_num = [&](float bx, float by, const char* s) {
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, s);
                otext(bx - ts.x * 0.5f, by - ts.y * 0.5f, IM_COL32(255, 255, 255, 255), s);
            };

            for (int row = 0; row < 20; ++row) {
                const int il = row * 2, ir = row * 2 + 1;
                const float y  = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;
                auto draw_box = [&](float x, int idx) {
                    const sys::GpioPin& p = sys::kPi40Pins[idx];
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (p.bcm >= 0 && !selectable[idx]) fill = (fill & 0x00FFFFFFu) | (120u << 24);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    ImU32 oc = 0; float ow = 2.f;
                    if      (p.bcm >= 0 && p.bcm == assigned) { oc = out_assigned; ow = 2.5f; }
                    else if (hardware[idx])                   { oc = out_hardware; }
                    else if (otherslot[idx] >= 0)             { oc = out_other; }
                    if (oc) dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz}, oc, 3.f, 0, ow);
                    if (p.bcm >= 0 && p.bcm == fbcm)
                        dl->AddRect({x - 1.5f, cy - 1.5f}, {x + pin_sz + 1.5f, cy + pin_sz + 1.5f},
                                    out_focus, 3.f, 0, 3.f);
                    char buf[6]; std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                    draw_pin_num(x + pin_sz * 0.5f, cy + pin_sz * 0.5f, buf);
                };
                draw_box(pin_left_x,  il);
                draw_box(pin_right_x, ir);

                draw_cols(il, cy, true);
                draw_cols(ir, cy, false);
            }

            // Info column (dark text on the light card): focused pin + legend.
            dl->AddText(font, fs * 0.7f, {info_x, co.y}, text_dim, "Pi 5 / CM5 — 40-pin header");
            int fidx = -1;
            for (int idx = 0; idx < 40; ++idx)
                if (sys::kPi40Pins[idx].bcm >= 0 && sys::kPi40Pins[idx].bcm == fbcm) { fidx = idx; break; }
            float iy = co.y + fs * 1.5f;
            if (fidx >= 0) {
                const sys::GpioPin& fp = sys::kPi40Pins[fidx];
                char hdr[24]; std::snprintf(hdr, sizeof(hdr), "GPIO %d", fp.bcm);
                dl->AddText(font, fs * 0.95f, {info_x, iy}, text_dark, hdr);
                iy += fs * 1.1f;
                std::string fns = fp.primary;
                if (fp.secondary && fp.secondary[0]) fns += " / " + std::string(fp.secondary);
                if (fp.tertiary  && fp.tertiary[0])  fns += " / " + std::string(fp.tertiary);
                dl->AddText(font, fs * 0.7f, {info_x, iy}, text_dim, fns.c_str());
                iy += fs * 1.15f;
                if (hardware[fidx]) {
                    dl->AddText(font, fs * 0.74f, {info_x, iy}, out_hardware, "In use (hardware):");
                    iy += fs * 0.92f;
                    for (const auto& w : gpio_hw_claimants(fp.bcm)) {
                        dl->AddText(font, fs * 0.7f, {info_x + 6.f, iy}, text_dark, w.c_str());
                        iy += fs * 0.86f;
                    }
                } else if (fp.bcm == assigned) {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_assigned, "This slot's pin");
                    iy += fs * 1.0f;
                } else if (otherslot[fidx] >= 0) {
                    char b[44]; std::snprintf(b, sizeof(b), "Also on Slot %d", otherslot[fidx] + 1);
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_other, b);
                    iy += fs * 0.9f;
                    dl->AddText(font, fs * 0.66f, {info_x, iy}, text_dim, "override → fix later");
                    iy += fs * 1.0f;
                } else {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, IM_COL32(30, 150, 50, 255), "Available");
                    iy += fs * 1.0f;
                }
                iy += fs * 0.4f;
            }
            auto legend = [&](ImU32 c, const char* t) {
                dl->AddRect({info_x, iy + 2.f}, {info_x + 11.f, iy + 13.f}, c, 2.f, 0, 2.f);
                dl->AddText(font, fs * 0.66f, {info_x + 16.f, iy}, IM_COL32(20, 24, 30, 255), t);
                iy += fs * 0.98f;
            };
            legend(out_assigned, "this slot");
            legend(out_other,    "other slot");
            legend(out_hardware, "hardware");
            legend(out_focus,    "cursor");
        };
    };

    // ── GPIO Buttons (configurable pin → function map) ────────────────────────
    // One submenu per assignable slot: BCM pin, short/long-press function, pull
    // bias, polarity, and long-press threshold. Edits the live gpio_pins array;
    // persisted to cfg["gpio"]["pins"] and applied on the next launch.
    MenuItem gpio_buttons_item;
    if (gpio_pins_p) {
        input::GpioPinCfg* GP = gpio_pins_p;
        // Build a function-picker submenu (all GpioFunc options) writing *slot.
        auto fn_picker = [](input::GpioFunc* slot) {
            std::vector<MenuItem> items;
            for (int i = 0; i < input::gpio_func_count(); ++i) {
                const auto f = static_cast<input::GpioFunc>(i);
                items.push_back(leaf_sel(input::gpio_func_name(f),
                    [slot, f]{ *slot = f; },
                    [slot, f]{ return *slot == f; }));
            }
            return items;
        };

        std::vector<MenuItem> gpio_btn_menu;
        if (gpio_inputs_enabled_p)
            gpio_btn_menu.push_back(with_desc(toggle("Enabled",
                [gpio_inputs_enabled_p]{ return *gpio_inputs_enabled_p; },
                [gpio_inputs_enabled_p, gpio_reload](bool v){
                    *gpio_inputs_enabled_p = v;
                    if (gpio_reload && *gpio_reload) (*gpio_reload)();  // apply now
                }),
                "Master switch for the GPIO button map. Applies immediately."));
        if (gpio_reload)
            gpio_btn_menu.push_back(with_desc(
                leaf("Apply Changes Now",
                     [gpio_reload]{ if (*gpio_reload) (*gpio_reload)(); }),
                "Rebuild the GPIO poller from the current slots so pin / "
                "function / pull / polarity edits take effect right away."));

        // Optional button/switch coprocessor (RP2350/RP2040). When enabled, an
        // external MCU debounces the switches and streams presses to the Pi,
        // dispatched through the same functions as the GPIO slots above. The
        // slots stay live unless replace_local_gpio is set in config.
        if (coproc_enabled_p) {
            std::vector<MenuItem> coproc_menu;
            coproc_menu.push_back(with_desc(toggle("Enabled",
                [coproc_enabled_p]{ return *coproc_enabled_p; },
                [coproc_enabled_p, coproc_reload](bool v){
                    *coproc_enabled_p = v;
                    if (coproc_reload && *coproc_reload) (*coproc_reload)();
                }),
                "Use an external button coprocessor (USB/I\xC2\xB2""C). Applies "
                "immediately. GPIO slots above stay active too unless "
                "replace_local_gpio is set in config.json."));
            if (coproc_status) {
                MenuItem st = leaf("Status", []{});
                st.label_fn = [coproc_status]{
                    return std::string("Status: ") +
                           (*coproc_status ? (*coproc_status)() : std::string("n/a"));
                };
                coproc_menu.push_back(with_desc(std::move(st),
                    "Connection state reported by the coprocessor link "
                    "(connected / offline). Button mapping is edited in "
                    "config.json under inputs.coprocessor.buttons."));
            }
            gpio_btn_menu.push_back(with_desc(
                submenu("Button Coprocessor", std::move(coproc_menu)),
                "Optional external MCU (RP2350/RP2040) that handles button "
                "debounce and streams presses to the Pi \xE2\x80\x94 frees GPIO "
                "and offloads timing. See docs/coprocessor-input.md."));
        }

        for (int i = 0; i < gpio_slot_count; ++i) {
            input::GpioPinCfg* p = &GP[i];

            // Pin picker page for this slot: an "(unused)" entry plus one leaf per
            // header GPIO line, gated by gpio_pin_available so only free pins show.
            // Selecting a pin sets it and pops back here to the slot's options.
            auto picker_focus = std::make_shared<int>(p->gpio);
            std::vector<MenuItem> picker_items;
            picker_items.push_back(leaf_sel("(Unused / disable)",
                [p, menu_sys_pp]{ p->gpio = -1;
                                  if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); },
                [p]{ return p->gpio < 0; }));
            for (const auto& gp : sys::kPi40Pins) {
                if (gp.bcm < 0) continue;   // skip power / ground / ID pins
                const int bcm = gp.bcm;
                const std::string alt = (gp.secondary && gp.secondary[0]) ? gp.secondary : "";
                MenuItem pm;
                pm.type         = MenuItemType::LEAF;
                pm.label        = "GPIO " + std::to_string(bcm);
                pm.label_fn     = [bcm, alt, i, gpio_other_slot]{
                    std::string s = "GPIO " + std::to_string(bcm);
                    if (!alt.empty()) s += "   (" + alt + ")";
                    const int other = gpio_other_slot(bcm, i);
                    if (other >= 0) s += "   ! Slot " + std::to_string(other + 1);
                    return s;
                };
                pm.get_state    = [p, bcm]{ return p->gpio == bcm; };
                pm.on_highlight = [picker_focus, bcm]{ *picker_focus = bcm; };
                // List every GPIO line not held by hardware. Pins already on
                // another slot stay listed (override allowed) and are flagged red.
                pm.visible_fn   = [bcm, gpio_pin_selectable]{ return gpio_pin_selectable(bcm); };
                pm.warn_fn      = [bcm, i, gpio_other_slot]{ return gpio_other_slot(bcm, i) >= 0; };
                pm.action       = [p, bcm, menu_sys_pp]{
                    p->gpio = bcm;
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
                };
                picker_items.push_back(std::move(pm));
            }
            MenuItem pin_item = with_panel(submenu("Pin", std::move(picker_items)),
                                           "Select GPIO Pin",
                                           draw_pin_picker_panel(picker_focus, i, p));
            pin_item.action   = [picker_focus, p]{ *picker_focus = p->gpio; };  // sync focus on enter
            pin_item.label_fn = [p]{
                return p->gpio < 0 ? std::string("Pin: (unused)")
                                   : std::string("Pin: GPIO ") + std::to_string(p->gpio);
            };
            pin_item.description =
                "Pick which GPIO pin drives this slot (header view shown "
                "alongside). Pins held by hardware (HUB75/SPI/I\xC2\xB2""C…) are "
                "hidden; a pin already used by another slot stays selectable but "
                "is flagged \xE2\x80\x94 overriding it marks both slots red until "
                "you fix the clash. Selecting a pin returns here to set functions.";

            std::vector<MenuItem> pull_items = {
                leaf_sel("Pull Up",   [p]{ p->pull = 1; }, [p]{ return p->pull == 1; }),
                leaf_sel("Pull Down", [p]{ p->pull = 2; }, [p]{ return p->pull == 2; }),
                leaf_sel("None",      [p]{ p->pull = 0; }, [p]{ return p->pull == 0; }),
            };
            std::vector<MenuItem> items = {
                pin_item,
                submenu("Short Press", fn_picker(&p->short_fn)),
                submenu("Long Press",  fn_picker(&p->long_fn)),
                slider("Long-press Time", 200.f, 2000.f, 50.f, " ms",
                    [p]{ return static_cast<float>(p->long_ms); },
                    [p](float v){ p->long_ms = static_cast<int>(v); }),
                submenu("Pull", std::move(pull_items)),
                toggle("Active Low",
                    [p]{ return p->active_low; },
                    [p](bool v){ p->active_low = v; }),
            };
            MenuItem m = submenu(std::string("Slot ") + std::to_string(i + 1), std::move(items));
            m.label_fn = [p, i, gpio_slot_conflict]{
                std::string s = "Slot " + std::to_string(i + 1) + "  ";
                if (p->gpio < 0) return s + "(unused)";
                if (gpio_slot_conflict(i)) s = "! " + s;   // leading marker + red (warn_fn)
                s += "GPIO" + std::to_string(p->gpio) + " \xE2\x86\x92 ";
                s += input::gpio_func_name(p->short_fn);
                return s;
            };
            // Paint the row red when this slot's pin collides with another slot
            // or a hardware peripheral, so the user knows to go back and fix it.
            m.warn_fn = [i, gpio_slot_conflict]{ return gpio_slot_conflict(i); };
            gpio_btn_menu.push_back(std::move(m));
        }

        gpio_buttons_item = with_desc(
            submenu("GPIO Buttons", std::move(gpio_btn_menu)),
            "Assign functions to GPIO switches. Each slot's Pin page shows the "
            "40-pin header and offers only the pins free of other peripherals; "
            "pick one to set its Short-press and optional Long-press function, "
            "pull bias, and polarity (Active Low = switch wired to GND with a "
            "pull-up). Toggling Enabled applies instantly; after editing slots "
            "choose \"Apply Changes Now\" to reload the poller. Saved to "
            "cfg[\"gpio\"][\"pins\"].");
    } else {
        gpio_buttons_item.visible_fn = []{ return false; };
    }

    // ── Pi Settings helpers ──────────────────────────────────────────────────
    // Most of these shell out to systemd/raspi tools. The ones that mutate
    // system state (hostname, timezone, NTP, apt, power) need sudo; ship
    // /etc/sudoers.d/protohud (chmod 440) with:
    //
    //   <user> ALL=(ALL) NOPASSWD: /usr/bin/hostnamectl, /usr/bin/timedatectl, \
    //       /usr/bin/apt-get, /usr/sbin/poweroff, /usr/sbin/reboot, \
    //       /usr/bin/systemctl restart protohud.service
    //
    // …so the menu calls succeed without an interactive password prompt.
    auto run_sh = [push_notif](std::string title, std::string cmd) {
        std::thread([push_notif, title=std::move(title), cmd=std::move(cmd)]() {
            const std::string full = cmd + " 2>&1 | logger -t protohud";
            const int rc = std::system(full.c_str());
            push_notif(NotifType::App, title,
                       rc == 0 ? std::string("OK")
                               : ("Failed (rc " + std::to_string(rc) + ")"),
                       5.f);
        }).detach();
    };
    // Capture popen output synchronously (small reads only — used for
    // hostname, current timezone, df output).
    auto sh_read = [](const char* cmd) -> std::string {
        std::string out;
        FILE* f = ::popen(cmd, "r");
        if (!f) return out;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) out += buf;
        ::pclose(f);
        // Trim trailing whitespace/newlines.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    };
    // get_state / get_toggle / context-panel hooks run for every visible row
    // every frame — raw sh_read() in one of those forks a process per row per
    // draw (the Timezone list alone was 11 forks/frame while open). A short
    // TTL keeps the shown values honest without the fork storm. Menu hooks
    // only run on the render thread, so the cache needs no lock.
    auto sh_read_cached = [sh_read](const char* cmd, double ttl_s) -> std::string {
        struct Entry { double t = -1e9; std::string out; };
        static std::map<std::string, Entry> cache;
        Entry& e = cache[cmd];
        const double now = glfwGetTime();
        if (now - e.t >= ttl_s) { e.out = sh_read(cmd); e.t = now; }
        return e.out;
    };

    // ── Pi Settings submenu ──────────────────────────────────────────────────
    std::vector<MenuItem> tz_items;
    {
        // A small curated set of common zones plus UTC. The user can add
        // more by hand-editing /etc/timezone; this list is just menu polish.
        static const char* kZones[] = {
            "UTC",
            "America/Los_Angeles", "America/Denver",
            "America/Chicago",     "America/New_York",
            "Europe/London",       "Europe/Berlin",
            "Europe/Paris",        "Asia/Tokyo",
            "Asia/Shanghai",       "Australia/Sydney",
        };
        for (const char* z : kZones) {
            tz_items.push_back(leaf_sel(z,
                [run_sh, z]{
                    run_sh(std::string("Timezone → ") + z,
                           std::string("sudo timedatectl set-timezone ") + z);
                },
                [sh_read_cached, z]{
                    return sh_read_cached("timedatectl show -p Timezone --value", 2.0) == z;
                }));
        }
    }
    std::vector<MenuItem> pi_settings_items = {
        // Hostname — current value shown in the context panel; descend to change it.
        with_panel(
            submenu("Hostname", std::vector<MenuItem>{
                with_desc(leaf("Set Hostname\xE2\x80\xA6",
                    [menu_sys_pp, run_sh]{
                        if (!menu_sys_pp || !*menu_sys_pp) return;
                        char host[256] = {0};
                        if (::gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';
                        (*menu_sys_pp)->open_keyboard("Hostname", host,
                            [run_sh](const std::string& nm){
                                if (nm.empty()) return;
                                std::string safe;   // keep only hostname-legal chars
                                for (char c : nm)
                                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                        || (c >= '0' && c <= '9') || c == '-' || c == '.')
                                        safe.push_back(c);
                                if (safe.empty()) return;
                                run_sh("Hostname", "sudo hostnamectl set-hostname " + safe);
                            });
                    }),
                    "Change the Pi's hostname (hostnamectl). Applies to new logins."),
            }),
            "Hostname",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                (void)sz;
                char host[256] = {0};
                const char* hn = (::gethostname(host, sizeof(host) - 1) == 0 && host[0])
                                   ? host : "(unknown)";
                ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 0.95f, {o.x, o.y},
                            IM_COL32(150, 160, 170, 210), "Current hostname");
                dl->AddText(font, fs * 1.4f, {o.x, o.y + fs * 1.4f},
                            IM_COL32(225, 235, 245, 240), hn);
            }),
        // Time — timezone, automatic sync, and clock display settings together.
        with_desc(submenu("Time", std::vector<MenuItem>{
            with_desc(submenu("Timezone", std::move(tz_items)),
                "System timezone (timedatectl). Affects the clock, log timestamps "
                "and scheduled tasks."),
            with_desc(toggle("Auto Time Sync (NTP)",
                [sh_read_cached]{ return sh_read_cached("timedatectl show -p NTP --value", 2.0) == "yes"; },
                [run_sh](bool v){
                    run_sh(v ? "NTP On" : "NTP Off",
                           std::string("sudo timedatectl set-ntp ") + (v ? "true" : "false"));
                }),
                "Automatic time sync via systemd-timesyncd. Off to set time manually."),
            submenu("Clock", std::vector<MenuItem>{
                toggle("24-Hour",   [&state]{ return state.clock_cfg.use_24h; },
                                    [&state](bool v){ state.clock_cfg.use_24h = v; }),
                toggle("Seconds",   [&state]{ return state.clock_cfg.show_seconds; },
                                    [&state](bool v){ state.clock_cfg.show_seconds = v; }),
                toggle("Show Date", [&state]{ return state.clock_cfg.show_date; },
                                    [&state](bool v){ state.clock_cfg.show_date = v; }),
                slider("Font Size", 1.f, 3.f, 0.25f, "x",
                    [&state]{ return state.clock_cfg.font_scale; },
                    [&state](float v){ state.clock_cfg.font_scale = v; }),
            }),
        }), "Timezone, automatic NTP sync, and clock display settings."),
        with_panel(
            // Storage usage context panel — df refreshes every few seconds,
            // not once per draw.
            submenu("Storage", std::vector<MenuItem>{}),
            "Disk Usage",
            [sh_read_cached](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 1.0f, {o.x, o.y},
                            IM_COL32(230, 235, 240, 255), "Disk Usage");
                const std::string out = sh_read_cached("df -h /", 5.0);
                float y = o.y + fs * 1.5f;
                size_t pos = 0;
                while (pos < out.size()) {
                    size_t nl = out.find('\n', pos);
                    if (nl == std::string::npos) nl = out.size();
                    const std::string line = out.substr(pos, nl - pos);
                    dl->AddText(font, fs * 0.85f, {o.x, y},
                                IM_COL32(200, 210, 220, 220), line.c_str());
                    y += fs;
                    pos = nl + 1;
                }
            }),
        // (Restart ProtoHUD + Shutdown moved to System > Power; the apt updater
        //  moved to System > Software. GPIO visualizer/buttons + cooling fans are
        //  appended to this list further below.)
    };

    // ── Updates submenu (in-HUD updater — Phase 1, user-initiated only) ───────
    // Pings the repo over plain git (no GitHub API): lists remote branches,
    // shows their change-notes/log in the context panel, and applies a chosen
    // branch via scripts/update.sh <branch> --restart. A pre-update rollback
    // point is recorded by update.sh; "Rollback" returns to it via
    // scripts/rollback.sh (also runnable standalone over SSH if the HUD won't
    // boot). POLICY: there is deliberately NO automatic update path here —
    // nothing fetches, builds or restarts unless the user picks it.
    struct UpdaterState {
        std::mutex  mtx;
        std::string root;          // repo root (resolved from /proc/self/exe)
        std::string cur_short;     // current HEAD short hash
        std::string cur_subject;   // current HEAD commit subject
        std::string cur_branch;    // current branch name
        std::vector<std::string> all_remote; // every remote branch (authoritative)
        std::vector<std::string> branches;  // filtered view shown in the menu
        bool        show_all = false;       // curated (main + claude/*) vs all
        int         behind   = -1;          // commits behind origin/<branch>; -1 unknown
        std::string last_check;             // human time of last fetch; "" = never
        std::string status   = "Not checked yet.";
        bool        busy     = false;       // an async git/update op is running
        int         sel      = -1;          // highlighted branch index (log panel)
        std::string sel_name;               // highlighted branch name
        std::string sel_log;                // cached git log for the highlighted branch
        bool        rollback_avail = false; // state/update/last_good.env exists
        std::string rollback_target;        // "hash on branch" for the label
    };
    auto upd = std::make_shared<UpdaterState>();
    {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            std::string p(exe);                                       // .../build/protohud
            auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
            s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
            upd->root = p;
        }
    }

    // Run `git -C <root> <args>` and capture stdout (local queries are fast;
    // network ones — fetch — are only ever called from a worker thread).
    auto git_q = [sh_read, upd](const std::string& args) -> std::string {
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        if (root.empty()) return {};
        const std::string cmd = "git -C '" + root + "' " + args;
        return sh_read(cmd.c_str());
    };

    // Refresh the current-version fields (local, instant).
    auto upd_refresh_cur = [git_q, upd]{
        std::string sh = git_q("rev-parse --short HEAD");
        std::string su = git_q("log -1 --pretty=%s");
        std::string br = git_q("rev-parse --abbrev-ref HEAD");
        std::lock_guard<std::mutex> lk(upd->mtx);
        upd->cur_short = sh; upd->cur_subject = su; upd->cur_branch = br;
    };

    // Recompute the visible list from the cached full list + curated filter.
    // Instant (no git) — safe to call on the UI thread when toggling the view.
    auto upd_apply_filter = [upd]{
        std::lock_guard<std::mutex> lk(upd->mtx);
        std::vector<std::string> out;
        for (const auto& ref : upd->all_remote) {
            if (!upd->show_all) {
                // Curated view: main/master plus the working claude/* branches.
                const bool keep = (ref == "main" || ref == "master"
                                   || ref.rfind("claude/", 0) == 0);
                if (!keep) continue;
            }
            out.push_back(ref);
        }
        upd->branches = std::move(out);
    };

    // Populate the full remote-branch list, then re-filter. local=true reads
    // local tracking refs (instant, but only branches already fetched);
    // local=false runs `git ls-remote --heads origin` (network — authoritative,
    // lists EVERY branch on the repo even if it was never fetched locally).
    auto upd_load_branches = [git_q, upd, upd_apply_filter](bool local){
        const std::string raw = local
            ? git_q("for-each-ref --format=%(refname:short) --sort=-committerdate refs/remotes/origin")
            : git_q("ls-remote --heads origin");
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t nl = raw.find('\n', pos);
            if (nl == std::string::npos) nl = raw.size();
            std::string line = raw.substr(pos, nl - pos);
            pos = nl + 1;
            std::string ref;
            if (local) {
                ref = line;
                const std::string pfx = "origin/";
                if (ref.rfind(pfx, 0) == 0) ref = ref.substr(pfx.size());
            } else {
                // ls-remote line: "<sha>\trefs/heads/<branch>"
                const auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                ref = line.substr(tab + 1);
                const std::string pfx = "refs/heads/";
                if (ref.rfind(pfx, 0) != 0) continue;
                ref = ref.substr(pfx.size());
            }
            if (ref.empty() || ref == "HEAD" || ref.find("HEAD ->") != std::string::npos) continue;
            out.push_back(ref);
        }
        // Don't clobber a good cached list with an empty network result
        // (offline / fetch failed) — but always accept the local read.
        if (!out.empty() || local) {
            std::lock_guard<std::mutex> lk(upd->mtx);
            upd->all_remote = std::move(out);
        }
        upd_apply_filter();
    };

    // Refresh rollback availability from the marker update.sh writes.
    auto upd_refresh_rollback = [sh_read, upd]{
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        const std::string marker = root + "/state/update/last_good.env";
        const std::string out = sh_read(
            ("[ -f '" + marker + "' ] && . '" + marker +
             "' && printf '%s|%s' \"${LAST_GOOD_COMMIT:0:9}\" \"${LAST_GOOD_BRANCH}\"").c_str());
        std::lock_guard<std::mutex> lk(upd->mtx);
        if (out.empty()) { upd->rollback_avail = false; upd->rollback_target.clear(); return; }
        upd->rollback_avail = true;
        auto bar = out.find('|');
        upd->rollback_target = (bar == std::string::npos)
            ? out : (out.substr(0, bar) + " on " + out.substr(bar + 1));
    };

    // Initial population (local only — never auto-fetches the network).
    upd_refresh_cur();
    upd_load_branches(/*local=*/true);
    upd_refresh_rollback();

    // "Check for Updates": async fetch + behind-count. The ONLY network call,
    // and only when the user picks it.
    auto upd_check = [git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
        { std::lock_guard<std::mutex> lk(upd->mtx);
          if (upd->busy) return; upd->busy = true; upd->status = "Checking\xe2\x80\xa6"; }
        std::thread([git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
            git_q("fetch --prune origin");                 // network (retry handled by git config / best-effort)
            upd_refresh_cur();
            upd_load_branches(/*local=*/false);            // authoritative remote list
            std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
            const std::string beh = git_q("rev-list --count HEAD..origin/" + br);
            int behind = beh.empty() ? -1 : std::atoi(beh.c_str());
            const std::string now = sh_read("date '+%a %H:%M'");
            std::string msg = behind > 0
                ? (std::to_string(behind) + " new commit(s) on " + br)
                : (behind == 0 ? "Up to date." : "Checked (branch not on remote).");
            { std::lock_guard<std::mutex> lk(upd->mtx);
              upd->behind = behind; upd->last_check = now; upd->status = msg; upd->busy = false; }
            push_notif(NotifType::App, "Updates",
                       behind > 0 ? (std::to_string(behind) + " update(s) available")
                                  : "Already up to date", 5.f);
        }).detach();
    };

    // Apply a branch via update.sh <branch> --restart, detached so the build +
    // restart survive ProtoHUD being killed during the restart step.
    auto upd_apply = [upd, push_notif](const std::string& branch){
        std::string root; bool busy;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; busy = upd->busy; }
        if (busy || root.empty() || branch.empty()) return;
        const std::string sp = root + "/scripts/update.sh";
        // setsid + bash -c '"$0" "$1" --restart' keeps the script in its own
        // session; $0/$1 carry the (quoted) script path + branch so neither
        // needs escaping in the -c string.
        const std::string cmd =
            "setsid bash -c '\"$0\" \"$1\" --restart' '" + sp + "' '" + branch +
            "' </dev/null >/tmp/protohud-update.log 2>&1 &";
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Update started",
                   "Pulling " + branch + " and rebuilding. The HUD restarts when "
                   "the build succeeds, and stays up if it fails. Log: "
                   "/tmp/protohud-update.log", 0.f);
    };

    // Roll back to the recorded known-good build via the standalone script.
    auto upd_rollback = [upd, push_notif]{
        std::string root; bool busy, avail;
        { std::lock_guard<std::mutex> lk(upd->mtx);
          root = upd->root; busy = upd->busy; avail = upd->rollback_avail; }
        if (busy || root.empty() || !avail) return;
        const std::string sp = root + "/scripts/rollback.sh";
        const std::string cmd =
            "setsid bash -c '\"$0\" --restart' '" + sp +
            "' </dev/null >/tmp/protohud-rollback.log 2>&1 &";
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Rollback started",
                   "Restoring the last known-good build + config, then restarting. "
                   "Log: /tmp/protohud-rollback.log", 0.f);
    };

    std::vector<MenuItem> updates_menu;
    {
        // Current version + status panel.
        MenuItem cur = leaf("Current Version", []{});
        cur.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            std::string s = "On " + (upd->cur_branch.empty() ? "?" : upd->cur_branch)
                          + " @ " + (upd->cur_short.empty() ? "?" : upd->cur_short);
            if (upd->behind > 0) s += "  (" + std::to_string(upd->behind) + " behind)";
            return s;
        };
        updates_menu.push_back(with_panel(std::move(cur), "Update Status",
            [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::lock_guard<std::mutex> lk(upd->mtx);
                float y = o.y;
                auto line = [&](const std::string& t, ImU32 col, float scale){
                    dl->AddText(font, fs * scale, {o.x, y}, col, t.c_str());
                    y += fs * (scale + 0.25f);
                };
                line("Current build", IM_COL32(230, 235, 240, 255), 1.0f);
                line(upd->cur_branch.empty() ? "branch ?" : upd->cur_branch,
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line((upd->cur_short.empty() ? "?" : upd->cur_short) + "  " + upd->cur_subject,
                     IM_COL32(180, 190, 200, 210), 0.85f);
                y += fs * 0.4f;
                line(upd->last_check.empty() ? "Last checked: never"
                                             : ("Last checked: " + upd->last_check),
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line(upd->status, upd->behind > 0 ? IM_COL32(120, 220, 140, 235)
                                                  : IM_COL32(200, 210, 220, 220), 0.85f);
            }));

        // Check for Updates (the only network action).
        MenuItem chk = with_desc(leaf("Check for Updates", [upd_check]{ upd_check(); }),
            "Fetch the latest commit list from the repo (git fetch) and report how "
            "far behind the current branch is. Does NOT change any code \xe2\x80\x94 nothing "
            "updates until you pick a branch below. This is the only step that uses "
            "the network.");
        chk.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->busy ? std::string("Checking\xe2\x80\xa6")
                             : std::string("Check for Updates");
        };
        updates_menu.push_back(std::move(chk));

        // Update current branch in place.
        updates_menu.push_back(with_desc(
            leaf("Update This Branch & Restart",
                 [upd, upd_apply]{
                     std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
                     upd_apply(br);
                 }),
            "Pull the latest commits for the CURRENT branch, rebuild, and restart. "
            "Your settings (config.json) and custom faces/effects are preserved \xe2\x80\x94 "
            "they're outside version control. A rollback point is saved first."));

        // Select Branch submenu — dynamic slots fed by upd->branches, with a
        // level context panel showing the highlighted branch's change log.
        std::vector<MenuItem> branch_items;
        branch_items.push_back(with_desc(toggle("Show All Branches",
            [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->show_all; },
            [upd, upd_apply_filter, upd_load_branches](bool v){
                { std::lock_guard<std::mutex> lk(upd->mtx); upd->show_all = v; }
                upd_apply_filter();   // instant re-filter of the cached list
                // …then refresh the authoritative list from the repo so branches
                // that were never fetched locally also appear (network).
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "OFF: show only main + the claude/* working branches (recommended). "
            "ON: list every branch on the repo (refreshed over the network)."));
        branch_items.push_back(with_desc(
            leaf("Refresh List", [upd_load_branches]{
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "Re-query the repo for its full branch list (git ls-remote). Shows "
            "every branch, including ones not yet downloaded to this Pi."));

        static constexpr int kBranchSlots = 48;
        for (int i = 0; i < kBranchSlots; ++i) {
            MenuItem b = leaf("branch", []{});
            b.visible_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                return i < static_cast<int>(upd->branches.size());
            };
            b.label_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (i >= static_cast<int>(upd->branches.size())) return std::string();
                std::string nm = upd->branches[i];
                if (nm == upd->cur_branch) nm += "  (current)";
                return nm;
            };
            // Load the change-log into the panel cache when highlighted.
            b.on_highlight = [upd, git_q, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  if (upd->sel == i) return;          // already cached
                  nm = upd->branches[i]; upd->sel = i; upd->sel_name = nm;
                  upd->sel_log = "Loading\xe2\x80\xa6"; }
                // Richer changelog: short hash + author-date + subject.
                std::string log = git_q(
                    "log --pretty=format:'%h %ad  %s' --date=short -n 20 origin/" + nm);
                std::string ahead  = git_q("rev-list --count HEAD..origin/" + nm);
                std::string behind = git_q("rev-list --count origin/" + nm + "..HEAD");
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (upd->sel != i) return;            // moved on while loading
                std::string hdr;
                if (!ahead.empty() && ahead != "0")  hdr += "+" + ahead + " new commit(s) to apply";
                if (!behind.empty() && behind != "0") hdr += (hdr.empty() ? "" : "  ") +
                                                             ("-" + behind + " not on this branch");
                if (hdr.empty() && !ahead.empty())   hdr = "Even with your current build";
                upd->sel_log = (hdr.empty() ? "" : (hdr + "\n\n"))
                             + (log.empty() ? "(no log \xe2\x80\x94 run Check for Updates to fetch it)" : log);
            };
            b.action = [upd, upd_apply, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  nm = upd->branches[i]; }
                upd_apply(nm);
            };
            b.description = "Select to update to this branch, rebuild, and restart. "
                            "The panel shows its recent commits. Settings + custom "
                            "content are preserved; a rollback point is saved first.";
            branch_items.push_back(std::move(b));
        }

        MenuItem branch_sub = submenu("Select Branch", std::move(branch_items));
        updates_menu.push_back(with_desc(
            with_panel(std::move(branch_sub), "Branch Changes",
                [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                    (void)sz;
                    ImFont* font = ImGui::GetFont();
                    const float fs = ImGui::GetFontSize();
                    std::lock_guard<std::mutex> lk(upd->mtx);
                    dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                                upd->sel_name.empty() ? "Highlight a branch"
                                                      : upd->sel_name.c_str());
                    float y = o.y + fs * 1.5f;
                    const std::string& t = upd->sel_log;
                    size_t pos = 0;
                    while (pos < t.size()) {
                        size_t nl = t.find('\n', pos);
                        if (nl == std::string::npos) nl = t.size();
                        const std::string ln = t.substr(pos, nl - pos);
                        dl->AddText(font, fs * 0.8f, {o.x, y},
                                    IM_COL32(200, 210, 220, 220), ln.c_str());
                        y += fs * 0.95f;
                        pos = nl + 1;
                    }
                }),
            "Choose a branch to update to. Highlight one to read its recent commits "
            "in the panel, then select to apply (rebuild + restart)."));

        // Rollback — visible only when a recorded known-good point exists.
        MenuItem rb = with_desc(leaf("Rollback Last Update",
            [upd_rollback]{ upd_rollback(); }),
            "Restore the build + config saved just before your last update, then "
            "rebuild and restart. If the HUD won't boot, run scripts/rollback.sh "
            "over SSH to do the same from outside ProtoHUD.");
        rb.visible_fn = [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->rollback_avail; };
        rb.label_fn   = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->rollback_target.empty()
                ? std::string("Rollback Last Update")
                : ("Rollback to " + upd->rollback_target);
        };
        updates_menu.push_back(std::move(rb));

        // Update History — reads state/update/history.log (written by update.sh),
        // newest first. Each line: ISO-date \t branch \t before..after \t subject.
        MenuItem hist = leaf("Update History", []{});
        hist.type = MenuItemType::SUBMENU;   // submenu so its context panel shows
        updates_menu.push_back(with_panel(std::move(hist), "Update History",
            [upd, sh_read](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::string root;
                { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
                const std::string log = root.empty() ? std::string() : sh_read(
                    ("f='" + root + "/state/update/history.log'; "
                     "[ -f \"$f\" ] && tac \"$f\" | head -n 18").c_str());
                dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                            "Recent updates (newest first)");
                float y = o.y + fs * 1.6f;
                if (log.empty()) {
                    dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(180, 190, 200, 210),
                                "No updates recorded yet.");
                    return;
                }
                size_t pos = 0;
                while (pos < log.size()) {
                    size_t nl = log.find('\n', pos);
                    if (nl == std::string::npos) nl = log.size();
                    std::string ln = log.substr(pos, nl - pos);
                    pos = nl + 1;
                    // Split the tab-separated record into a compact two-line entry.
                    std::vector<std::string> f; size_t p = 0;
                    while (true) { size_t t = ln.find('\t', p);
                        f.push_back(ln.substr(p, t == std::string::npos ? std::string::npos : t - p));
                        if (t == std::string::npos) break; p = t + 1; }
                    std::string when = f.size() > 0 ? f[0] : ln;
                    if (when.size() >= 16) when = when.substr(0, 16).replace(10, 1, " "); // YYYY-MM-DD HH:MM
                    const std::string br  = f.size() > 1 ? f[1] : "";
                    const std::string rng = f.size() > 2 ? f[2] : "";
                    const std::string sub = f.size() > 3 ? f[3] : "";
                    dl->AddText(font, fs * 0.82f, {o.x, y}, IM_COL32(210, 220, 230, 230),
                                (when + "   " + br + "  " + rng).c_str());
                    y += fs * 0.9f;
                    if (!sub.empty()) {
                        dl->AddText(font, fs * 0.78f, {o.x + fs * 0.8f, y},
                                    IM_COL32(170, 180, 190, 200), sub.c_str());
                        y += fs * 0.95f;
                    }
                    y += fs * 0.15f;
                }
            }));

        // Settings & Data Safety — a static explainer of what survives an update.
        MenuItem safe = leaf("Settings & Data Safety", []{});
        safe.type = MenuItemType::SUBMENU;
        updates_menu.push_back(with_panel(std::move(safe), "What's Preserved",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                static const char* kLines[] = {
                    "Updates never touch your data:",
                    "",
                    "\xe2\x80\xa2 config.json (all menu settings) is outside",
                    "  version control \xe2\x80\x94 git can't overwrite it.",
                    "\xe2\x80\xa2 New defaults from a release are merged IN;",
                    "  your existing values always win.",
                    "\xe2\x80\xa2 Custom faces/effects + IMU calibration",
                    "  live in config.json / Protoface and persist.",
                    "\xe2\x80\xa2 Protoface (imported faces) updates without",
                    "  deleting anything you imported.",
                    "",
                    "Before each update a rollback point is saved",
                    "(commit + a config backup under state/update/).",
                    "Recover anytime via Rollback, or over SSH with",
                    "scripts/rollback.sh --restart.",
                };
                float y = o.y;
                for (const char* ln : kLines) {
                    const bool head = (ln[0] != '\0' && ln[0] != ' ' && ln[0] != '\xe2');
                    dl->AddText(font, fs * (head ? 0.92f : 0.82f), {o.x, y},
                                head ? IM_COL32(230, 235, 240, 255)
                                     : IM_COL32(195, 205, 215, 220), ln);
                    y += fs * 0.95f;
                }
            }));
    }

    // ── Power submenu (existing reboot/quit + new shutdown moved here) ──────
    // Confirm wrapper: a destructive action becomes a submenu the user has to
    // descend into and pick "Confirm …" before it fires — guards against an
    // accidental select on Reboot Pi / Shutdown.
    auto confirm_action = [&](const char* label, const char* warn,
                              std::function<void()> act) -> MenuItem {
        std::vector<MenuItem> kids;
        kids.push_back(with_desc(leaf(std::string("Confirm: ") + label, std::move(act)), warn));
        MenuItem m = submenu(label, std::move(kids));
        m.description = warn;
        return m;
    };
    // Relaunch ProtoHUD via scripts/restart.sh (resolved from /proc/self/exe),
    // detached in its own session so it survives this process being killed.
    auto restart_protohud = [] {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        std::string p(exe);
        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
        const std::string script = p + "/scripts/restart.sh";
        std::system(("setsid \"" + script + "\" >/tmp/protohud.log 2>&1 </dev/null &").c_str());
    };
    std::vector<MenuItem> power_menu = {
        with_desc(leaf("Reboot ProtoHUD", restart_protohud),
            "Relaunch ProtoHUD (stop + start via scripts/restart.sh). The display "
            "blanks briefly while it comes back."),
        with_desc(leaf("Reboot ProtoFace", [teensy]{ if (teensy) teensy->restart(); }),
            "Restart the face backend (Protoface daemon / native renderer) without "
            "touching the rest of the HUD."),
        confirm_action("Reboot Pi",
            "Reboots the whole Raspberry Pi after a short delay. Unsaved work is lost.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n reboot 2>/dev/null || reboot 2>/dev/null &");
                }).detach();
            }),
        confirm_action("Shutdown",
            "Powers off the Raspberry Pi. You'll need to cycle power to start it again.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n poweroff 2>/dev/null || poweroff 2>/dev/null &");
                }).detach();
            }),
        with_desc(leaf("Close Program", [&state]{ state.quit = true; }),
            "Exit ProtoHUD without rebooting the system."),
        with_desc(toggle("Skip Startup Screen",
            [&state]{ return state.skip_landing; },
            [&state](bool v){ state.skip_landing = v; }),
            "Bypass the profile/continue landing screen at boot and run the current "
            "config directly. Takes effect on the next launch."),
    };

    // ── Display & HUD submenu (cosmetic / per-window controls) ───────────────
    std::vector<MenuItem> display_hud_menu = {
        with_desc(submenu("HUD / Menu Presets", std::move(hud_presets_menu)),
                  "Visual presets: built-in themes plus your own saved HUD color + "
                  "menu style combinations. Applied live."),
        with_desc(toggle("Radial Quick Menu",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp
                              && (*menu_sys_pp)->quick_style() == QuickStyle::Radial; },
            [menu_sys_pp](bool v){ if (menu_sys_pp && *menu_sys_pp)
                (*menu_sys_pp)->set_quick_style(v ? QuickStyle::Radial : QuickStyle::List); }),
            "ON: the quick menu is a radial wheel encircling the minimap. OFF: the "
            "legacy compact corner list. The minimap's position (HUD > Map) sets where "
            "the wheel sits."),
        with_desc(slider("Menu Tilt", 0.f, 0.8f, 0.05f, "",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp ? (*menu_sys_pp)->radial_tilt() : 0.f; },
            [menu_sys_pp](float v){ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->set_radial_tilt(v); }),
            "Helmet-style inward perspective tilt for the radial menu. 0 = flat on "
            "the glass; higher = the wheel curves away at the top like a visor."),
        with_desc(slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
            "Scale factor applied to all HUD text. Lower = more on-screen at once."),
        // Window mode (Fullscreen/Frameless) + Resolution moved to System > Display.
        // Legacy HUD toggle moved to HUD > Legacy HUD (grouped with its settings).
    };

    // ── Display submenu (window mode + windowed resolution) ───────────────────
    // Desktop/dev only — ignored while the glasses drive the window.
    std::vector<MenuItem> resolution_menu;
    {
        static const struct { const char* l; int w, h; } kRes[] = {
            {"1280 x 720",  1280,  720}, {"1600 x 900",  1600,  900},
            {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
            {"3840 x 1080 (SBS)", 3840, 1080}, {"3840 x 2160", 3840, 2160},
        };
        for (const auto& r : kRes) {
            const int w = r.w, h = r.h;
            resolution_menu.push_back(leaf(r.l, [&state, w, h]{
                state.win_resize_w.store(w); state.win_resize_h.store(h);
                state.win_resize_dirty.store(true);
            }));
        }
    }
    std::vector<MenuItem> display_menu = {
        with_desc(toggle("Fullscreen",
            [&state]{ return state.win_fullscreen.load(); },
            [&state](bool v){ state.win_fullscreen.store(v); state.win_mode_dirty.store(true); }),
            "Borderless fullscreen covering the whole screen. Desktop/dev only \xe2\x80\x94 "
            "ignored while the glasses are connected. Applied live."),
        with_desc(toggle("Frameless Window",
            [&state]{ return state.win_frameless.load(); },
            [&state](bool v){ state.win_frameless.store(v); state.win_mode_dirty.store(true); }),
            "Remove the OS window title bar and borders (windowed mode). Desktop/dev "
            "only. Applied live."),
        with_desc(submenu("Resolution", std::move(resolution_menu)),
            "Resize the windowed (non-fullscreen) output. Desktop/dev only; ignored "
            "while the glasses drive the display."),
    };

    // ── Connectivity submenu ─────────────────────────────────────────────────
    // ── Bluetooth controls (bluetoothctl shell-out) ──────────────────────────
    // Scan/power + per-device connect/pair/trust/remove. Devices come from
    // BtMonitor (state.bt_devices), which now also lists discovered-but-unpaired
    // devices after a scan so they can be paired here.
    auto bt_cmd = [&state, bt_mon](int i, const char* verb) {
        std::string mac;
        { std::lock_guard<std::mutex> lk(state.mtx);
          if (i >= 0 && i < (int)state.bt_devices.size()) mac = state.bt_devices[i].mac; }
        if (mac.empty()) return;
        std::system((std::string("bluetoothctl ") + verb + " " + mac +
                     " >/dev/null 2>&1 &").c_str());
        if (bt_mon) bt_mon->refresh();
    };
    std::vector<MenuItem> bluetooth_menu;
    bluetooth_menu.push_back(with_desc(leaf("Scan for Devices", [bt_mon]{
        std::system("bluetoothctl --timeout 20 scan on >/dev/null 2>&1 &");
        if (bt_mon) bt_mon->refresh();
    }), "Discover nearby devices for ~20s; new ones appear in the list below to pair."));
    bluetooth_menu.push_back(with_desc(leaf("Power On", [bt_mon]{
        std::system("bluetoothctl power on >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter on."));
    bluetooth_menu.push_back(with_desc(leaf("Power Off", [bt_mon]{
        std::system("bluetoothctl power off >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter off."));
    bluetooth_menu.push_back(with_desc(leaf("Refresh", [bt_mon]{ if (bt_mon) bt_mon->refresh(); }),
        "Re-scan known devices and update the list now."));
    for (int i = 0; i < 12; ++i) {
        MenuItem dev; dev.type = MenuItemType::SUBMENU; dev.label = "btdev";
        dev.label_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)state.bt_devices.size()) return std::string();
            const auto& d = state.bt_devices[i];
            return d.name + (d.connected ? "  \xE2\x9C\x93" : d.paired ? "  (paired)" : "  (new)");
        };
        dev.visible_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx); return i < (int)state.bt_devices.size(); };
        dev.children = {
            with_desc(leaf("Connect",    [bt_cmd, i]{ bt_cmd(i, "connect"); }),
                "Connect to this device."),
            with_desc(leaf("Disconnect", [bt_cmd, i]{ bt_cmd(i, "disconnect"); }),
                "Disconnect this device."),
            with_desc(leaf("Pair",       [bt_cmd, i]{ bt_cmd(i, "pair"); }),
                "Pair with this device (confirm any prompt on both ends)."),
            with_desc(leaf("Trust",      [bt_cmd, i]{ bt_cmd(i, "trust"); }),
                "Mark trusted so it can auto-reconnect."),
            with_desc(leaf("Remove",     [bt_cmd, i]{ bt_cmd(i, "remove"); }),
                "Unpair and forget this device."),
        };
        bluetooth_menu.push_back(std::move(dev));
    }

    std::vector<MenuItem> connectivity_menu = {
        with_desc(toggle("SSH Access",
            [state_ptr]{ return state_ptr && state_ptr->ssh.active; },
            [state_ptr](bool v){
                system(v ? "systemctl start ssh 2>&1 | logger -t protohud &"
                         : "systemctl stop ssh 2>&1 | logger -t protohud &");
                if (state_ptr) {
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->ssh.active = v;
                }
            }),
            "Toggle the sshd service. ON exposes the Pi to network logins on TCP 22."),
        with_desc(submenu("Bluetooth", std::move(bluetooth_menu)),
                  "Scan, pair, connect and manage Bluetooth devices."),
    };

    // ── Diagnostics submenu (probes + overlays + per-tab tools) ──────────────
    std::vector<MenuItem> diagnostics_group_menu = {
        with_desc(toggle("System Panel",
            [sys_panel_active]{ return sys_panel_active && *sys_panel_active; },
            [sys_panel_active](bool v){ if (sys_panel_active) *sys_panel_active = v; }),
            "Live system overlay (CPU, RAM, GPU temp, network ping)."),
        with_desc(toggle("FPS Overlay",
            [fps_overlay_active]{ return fps_overlay_active && *fps_overlay_active; },
            [fps_overlay_active](bool v){ if (fps_overlay_active) *fps_overlay_active = v; }),
            "Show the current frame rate as an overlay."),
        with_desc(submenu("FPS Average",   std::move(fps_interval_menu)),
                  "Averaging window for the FPS readout."),
        with_desc(submenu("Diagnostics",   std::move(diagnostics_menu)),
                  "Camera / network / Bluetooth / GPIO probes."),
        with_desc(leaf("Request Status", [teensy]{ teensy->request_status(); }),
                  "Poll the face controller for a fresh status frame."),
    };
    // Moved out of Diagnostics per the menu reorg: the GPIO visualizer + buttons
    // and the cooling-fan controls live under Pi Settings (hardware); Demo Mode
    // lives under Software (appended at the tail of this block).
    pi_settings_items.push_back(std::move(gpio_viz_item));
    pi_settings_items.push_back(std::move(gpio_buttons_item));
    // Cooling fans (Pi-driven PWM). Hidden when no FanController is wired.
    pi_settings_items.push_back([&]() -> MenuItem {
            if (!fans || fans->zone_count() == 0) {
                MenuItem m = leaf("Cooling Fans", []{}); m.visible_fn = []{ return false; }; return m;
            }
            // Per-zone speed/mode/curve submenu.
            auto build_zone = [&](int z) -> MenuItem {
                std::vector<MenuItem> mode_items = {
                    leaf_sel("Manual", [fans, z]{ fans->set_zone_auto(z, false); },
                                       [fans, z]{ return !fans->zone_auto(z); }),
                    leaf_sel("Auto (by CPU temp)", [fans, z]{ fans->set_zone_auto(z, true); },
                                       [fans, z]{ return fans->zone_auto(z); }),
                };
                MenuItem speed = slider("Speed", 0.f, 100.f, 5.f, "%",
                    [fans, z]{ return static_cast<float>(fans->zone_speed(z) * 100.0); },
                    [fans, z](float v){ fans->set_zone_speed(z, v / 100.0); });
                speed.visible_fn = [fans, z]{ return !fans->zone_auto(z); };
                MenuItem amin = slider("Auto Min Temp", 30.f, 80.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_min(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, v, fans->zone_auto_max(z)); });
                amin.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem amax = slider("Auto Max Temp", 40.f, 90.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_max(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, fans->zone_auto_min(z), v); });
                amax.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem status = leaf("Output", []{});
                status.label_fn = [fans, z]{
                    char b[40];
                    std::snprintf(b, sizeof(b), "Output: %d%%",
                                  (int)std::lround(fans->zone_duty(z) * 100.0));
                    return std::string(b);
                };
                std::vector<MenuItem> zi = {
                    submenu("Mode", std::move(mode_items)),
                    std::move(speed), std::move(amin), std::move(amax), std::move(status),
                };
                MenuItem m = submenu(fans->zone_name(z), std::move(zi));
                m.label_fn = [fans, z]{
                    char b[48];
                    std::snprintf(b, sizeof(b), "%s  [%d%%%s]", fans->zone_name(z).c_str(),
                                  (int)std::lround(fans->zone_duty(z) * 100.0),
                                  fans->zone_auto(z) ? " auto" : "");
                    return std::string(b);
                };
                return m;
            };
            std::vector<MenuItem> fan_items;
            fan_items.push_back(with_desc(toggle("Enabled",
                [fans]{ return fans->running(); },
                [fans](bool v){ if (v) fans->start(); else fans->stop(); }),
                "Drive the cooling fans on their configured GPIO. Off releases the lines."));
            for (int z = 0; z < fans->zone_count(); ++z)
                fan_items.push_back(build_zone(z));
            MenuItem temp = leaf("CPU Temp", []{});
            temp.label_fn = [fans]{
                char b[32]; std::snprintf(b, sizeof(b), "CPU Temp: %.0f\xc2\xb0""C",
                                          fans->current_temp_c());
                return std::string(b);
            };
            fan_items.push_back(std::move(temp));
            return with_desc(submenu("Cooling Fans", std::move(fan_items)),
                "Pi-driven PWM cooling fans \xe2\x80\x94 up to 4 fans in 2 zones, each "
                "with its own speed/mode. Pins in config[\"fans\"][\"zones\"]; use "
                "GPIO clear of HUB75 (see carrier PINMAP).");
        }());
    software_menu.push_back(with_desc(submenu("Demo Mode", std::move(demo_menu)),
        "Cycle prefab scenes for screenshots / video."));

    // ── Communications: LoRa + Phone (KDE Connect) + Notification Log ─────────
    MenuItem phone_item = [&]() -> MenuItem {
            // ── Phone (KDE Connect) ───────────────────────────────────────────
            // Ring the phone + edit the ignore list (mute servers/chats) and the
            // message-apps list (which apps get the big chat toast), from the HUD.
            auto ring_toast = [kdc_p, &state]{
                const bool ok = kdc_p && kdc_p->ring_phone();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n;
                n.type = NotifType::App; n.icon = "message";
                n.title = ok ? "Ringing phone\xE2\x80\xA6" : "Phone not connected";
                n.body  = ok ? "KDE Connect \xC2\xB7 findmyphone"
                             : "Pair a device in the KDE Connect app first";
                n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };
            // Generic CSV-list editor: an "Add… (keyboard)" row plus up to 16
            // removable entry rows. apply() pushes the joined CSV to the bridge.
            // menu_sys_pp is captured by value (it points at main's menu_ptr).
            auto list_menu = [menu_sys_pp](std::vector<std::string>* vec,
                                           std::function<void()> apply,
                                           std::string add_label, std::string osk_title) {
                std::vector<MenuItem> items;
                MenuItem add; add.type = MenuItemType::LEAF; add.label = std::move(add_label);
                add.action = [menu_sys_pp, vec, apply, osk_title]{
                    if (!menu_sys_pp || !*menu_sys_pp || !vec) return;
                    (*menu_sys_pp)->open_keyboard(osk_title, "",
                        [vec, apply](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            vec->push_back(s.substr(b, e - b + 1));
                            apply();
                        });
                };
                items.push_back(std::move(add));
                for (int i = 0; i < 16; ++i) {
                    MenuItem m; m.type = MenuItemType::LEAF; m.label = "entry";
                    m.label_fn   = [vec, i]{
                        return (vec && i < static_cast<int>(vec->size()))
                                   ? ("\xE2\x9C\x95  " + (*vec)[i]) : std::string(); };
                    m.visible_fn = [vec, i]{ return vec && i < static_cast<int>(vec->size()); };
                    m.action     = [vec, i, apply]{
                        if (vec && i < static_cast<int>(vec->size())) {
                            vec->erase(vec->begin() + i); apply(); } };
                    items.push_back(std::move(m));
                }
                return items;
            };
            auto join = [](std::vector<std::string>* v){
                std::string csv;
                if (v) for (size_t i = 0; i < v->size(); ++i) { if (i) csv += ','; csv += (*v)[i]; }
                return csv;
            };
            auto apply_ignore = [kdc_p, kdc_ignore_p, join]{
                if (kdc_p) kdc_p->set_ignore_list(join(kdc_ignore_p)); };
            auto apply_msg = [kdc_p, kdc_msg_p, join]{
                if (kdc_p) kdc_p->set_message_apps(join(kdc_msg_p)); };

            // Small toast helper for action feedback.
            auto toast = [&state](std::string t, std::string b){
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App; n.icon = "message";
                n.title = std::move(t); n.body = std::move(b); n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };

            // ── Notifications (reply / dismiss) ────────────────────────────────
            // Each row mirrors a recent phone notification; Reply (messaging apps)
            // opens the OSK, Dismiss clears it on the phone.
            std::vector<MenuItem> notif_menu;
            notif_menu.push_back(with_desc(leaf("Dismiss All", [kdc_p, toast]{
                if (kdc_p && kdc_p->dismiss_notification("")) toast("Dismissed", "Cleared phone notifications");
            }), "Dismiss every active notification on the phone."));
            for (int i = 0; i < 12; ++i) {
                MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "notif";
                row.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return std::string();
                    const auto& n = v[i];
                    std::string who = !n.title.empty() ? n.title
                                    : (!n.app.empty() ? n.app : std::string("Phone"));
                    return who + (n.repliable ? "  \xE2\x9C\x89" : "");
                };
                row.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->phone_notifications().size(); };
                MenuItem reply = leaf("Reply\xE2\x80\xA6", [kdc_p, i, menu_sys_pp, toast]{
                    if (!kdc_p || !menu_sys_pp || !*menu_sys_pp) return;
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return;
                    const std::string id = v[i].id, who = v[i].title;
                    (*menu_sys_pp)->open_keyboard("Reply to " + who, "",
                        [kdc_p, id, toast](const std::string& s){
                            if (s.empty()) return;
                            if (kdc_p->reply_notification(id, s)) toast("Reply sent", s);
                        });
                });
                reply.visible_fn = [kdc_p, i]{
                    auto v = kdc_p ? kdc_p->phone_notifications() : std::vector<integrations::KdeConnectBridge::PhoneNotif>{};
                    return i < (int)v.size() && v[i].repliable; };
                MenuItem dismiss = leaf("Dismiss", [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->phone_notifications();
                    if (i < (int)v.size() && kdc_p->dismiss_notification(v[i].id))
                        toast("Dismissed", v[i].title);
                });
                // Context panel: show the full body text of the focused notification.
                row.children = { std::move(reply), std::move(dismiss) };
                notif_menu.push_back(std::move(row));
            }

            // ── Media control (mprisremote) ────────────────────────────────────
            std::vector<MenuItem> media_menu;
            {
                MenuItem now; now.type = MenuItemType::LEAF; now.label = "now playing";
                now.label_fn = [kdc_p]{
                    if (!kdc_p) return std::string("No media");
                    auto m = kdc_p->media_status();
                    if (!m.has_player) return std::string("No media");
                    std::string s = m.playing ? "\xE2\x96\xB6 " : "\xE2\x9D\x9A ";
                    s += !m.title.empty() ? m.title : std::string("(unknown)");
                    if (!m.artist.empty()) s += " \xE2\x80\x94 " + m.artist;
                    return s;
                };
                media_menu.push_back(std::move(now));
                auto act = [kdc_p](const char* a){ if (kdc_p) kdc_p->media_action(a); };
                media_menu.push_back(leaf("Play / Pause", [act]{ act("PlayPause"); }));
                media_menu.push_back(leaf("Next",         [act]{ act("Next"); }));
                media_menu.push_back(leaf("Previous",     [act]{ act("Previous"); }));
                media_menu.push_back(leaf("Stop",         [act]{ act("Stop"); }));
                media_menu.push_back(slider("Volume", 0.f, 100.f, 5.f, "%",
                    [kdc_p]{ auto m = kdc_p ? kdc_p->media_status() : integrations::KdeConnectBridge::MediaStatus{};
                             return (float)(m.volume < 0 ? 0 : m.volume); },
                    [kdc_p](float v){ if (kdc_p) kdc_p->media_set_volume((int)v); }));
                // Player picker (up to 6).
                std::vector<MenuItem> players;
                for (int i = 0; i < 6; ++i) {
                    MenuItem p; p.type = MenuItemType::LEAF; p.label = "player";
                    p.label_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        if (i >= (int)m.players.size()) return std::string();
                        return m.players[i] + (m.players[i] == m.player ? "  \xE2\x97\x8F" : ""); };
                    p.visible_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        return i < (int)m.players.size(); };
                    p.action = [kdc_p, i]{ if (!kdc_p) return; auto m = kdc_p->media_status();
                        if (i < (int)m.players.size()) kdc_p->media_set_player(m.players[i]); };
                    players.push_back(std::move(p));
                }
                media_menu.push_back(submenu("Player", std::move(players)));
            }

            // ── Run commands (remotecommands) ──────────────────────────────────
            std::vector<MenuItem> cmd_menu;
            for (int i = 0; i < 16; ++i) {
                MenuItem c; c.type = MenuItemType::LEAF; c.label = "command";
                c.label_fn = [kdc_p, i]{ auto v = kdc_p ? kdc_p->run_commands()
                        : std::vector<integrations::KdeConnectBridge::RunCommand>{};
                    return i < (int)v.size() ? v[i].name : std::string(); };
                c.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->run_commands().size(); };
                c.action = [kdc_p, i, toast]{ if (!kdc_p) return; auto v = kdc_p->run_commands();
                    if (i < (int)v.size() && kdc_p->run_command(v[i].key)) toast("Sent", v[i].name); };
                cmd_menu.push_back(std::move(c));
            }

            // ── Grouped ignore picker (app → senders; red = muted) ─────────────
            auto roster = [kdc_p]{ return kdc_p ? kdc_p->notif_roster()
                : std::vector<std::pair<std::string, std::vector<std::string>>>{}; };
            auto ig_has = [kdc_ignore_p](const std::string& s){
                return kdc_ignore_p && std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s)
                                       != kdc_ignore_p->end(); };
            auto ig_toggle = [kdc_ignore_p, apply_ignore](const std::string& s){
                if (!kdc_ignore_p || s.empty()) return;
                auto it = std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s);
                if (it != kdc_ignore_p->end()) kdc_ignore_p->erase(it);
                else                           kdc_ignore_p->push_back(s);
                apply_ignore();
            };
            auto build_app_senders = [roster, ig_has, ig_toggle](int ai){
                std::vector<MenuItem> kids;
                MenuItem all; all.type = MenuItemType::TOGGLE; all.label = "Mute whole app";
                all.get_toggle = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                all.set_toggle = [roster, ig_toggle, ai](bool){ auto r = roster();
                    if (ai < (int)r.size()) ig_toggle(r[ai].first); };
                all.warn_fn = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                kids.push_back(std::move(all));
                for (int si = 0; si < 24; ++si) {
                    MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
                    m.label_fn   = [roster, ai, si]{ auto r = roster();
                        return (ai < (int)r.size() && si < (int)r[ai].second.size())
                                   ? r[ai].second[si] : std::string(); };
                    m.visible_fn = [roster, ai, si]{ auto r = roster();
                        return ai < (int)r.size() && si < (int)r[ai].second.size(); };
                    m.get_toggle = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    m.set_toggle = [roster, ig_toggle, ai, si](bool){ auto r = roster();
                        if (ai < (int)r.size() && si < (int)r[ai].second.size())
                            ig_toggle(r[ai].second[si]); };
                    m.warn_fn    = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    kids.push_back(std::move(m));
                }
                return kids;
            };
            std::vector<MenuItem> ignore_menu;
            ignore_menu.push_back(with_desc(leaf("Add Word\xE2\x80\xA6 (keyboard)",
                [menu_sys_pp, kdc_ignore_p, apply_ignore]{
                    if (!menu_sys_pp || !*menu_sys_pp || !kdc_ignore_p) return;
                    (*menu_sys_pp)->open_keyboard("Ignore (matches title/text)", "",
                        [kdc_ignore_p, apply_ignore](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            kdc_ignore_p->push_back(s.substr(b, e - b + 1)); apply_ignore();
                        });
                }), "Add a free-text mute (matches a notification's title/text)."));
            ignore_menu.push_back(with_desc(leaf("Unmute All",
                [kdc_ignore_p, apply_ignore]{ if (kdc_ignore_p) { kdc_ignore_p->clear(); apply_ignore(); } }),
                "Clear every mute."));
            for (int ai = 0; ai < 24; ++ai) {
                MenuItem appsub = submenu("app", build_app_senders(ai));
                appsub.label_fn   = [roster, ai]{ auto r = roster();
                    return ai < (int)r.size() ? r[ai].first : std::string(); };
                appsub.visible_fn = [roster, ai]{ return ai < (int)roster().size(); };
                appsub.warn_fn    = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                ignore_menu.push_back(std::move(appsub));
            }

            // ── SMS (send) ─────────────────────────────────────────────────────
            auto send_sms_flow = [menu_sys_pp, kdc_p, toast]{
                if (!menu_sys_pp || !*menu_sys_pp || !kdc_p) return;
                (*menu_sys_pp)->open_keyboard("SMS to (number)", "",
                    [menu_sys_pp, kdc_p, toast](const std::string& num){
                        if (num.empty() || !menu_sys_pp || !*menu_sys_pp) return;
                        (*menu_sys_pp)->open_keyboard("Message", "",
                            [kdc_p, num, toast](const std::string& msg){
                                if (msg.empty()) return;
                                if (kdc_p->send_sms(num, msg)) toast("SMS sent", num + ": " + msg);
                            });
                    });
            };

            // ── Connectivity readout ───────────────────────────────────────────
            MenuItem conn_item; conn_item.type = MenuItemType::LEAF; conn_item.label = "Signal";
            conn_item.label_fn = [kdc_p]{
                if (!kdc_p) return std::string("Signal: n/a");
                auto c = kdc_p->connectivity();
                if (!c.ok) return std::string("Signal: n/a");
                std::string s = "Signal: " + (c.network_type.empty() ? std::string("?") : c.network_type);
                if (c.strength >= 0) { s += "  "; for (int i = 0; i < 5; ++i) s += (i < c.strength) ? "\xE2\x96\xB0" : "\xE2\x96\xB1"; }
                return s;
            };

            // ── Pair Device ────────────────────────────────────────────────────
            // Lists every reachable KDE Connect device (paired or not). Selecting
            // an unpaired one requests pairing (accept the prompt on the phone);
            // selecting a paired one unpairs it.
            std::vector<MenuItem> pair_menu;
            for (int i = 0; i < 8; ++i) {
                MenuItem d; d.type = MenuItemType::LEAF; d.label = "device";
                d.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return std::string();
                    const auto& dev = v[i];
                    std::string suffix = dev.paired ? "  \xE2\x9C\x93 paired"
                                       : dev.reachable ? "  \xE2\x80\x94 tap to pair"
                                                       : "  (offline)";
                    return dev.name + suffix;
                };
                d.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->devices().size(); };
                d.action = [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return;
                    const auto& dev = v[i];
                    if (dev.paired) {
                        if (kdc_p->unpair(dev.id)) toast("Unpaired", dev.name);
                    } else if (kdc_p->request_pairing(dev.id)) {
                        toast("Pairing requested", "Accept the prompt on " + dev.name);
                    }
                };
                pair_menu.push_back(std::move(d));
            }

            // ── Export to Phone ─────────────────────────────────────────────────
            // Bundle a whole category (faces / gifs / qr codes) into one .tar.gz
            // and share it to the phone via KDE Connect. Source dirs are derived
            // from the active face path (faces + its sibling gifs) and state.qr_dir,
            // so no extra params have to be threaded through build_menu.
            auto export_to_phone = [kdc_p, teensy, &state, toast](const char* kind) {
                namespace fsx = std::filesystem;
                std::string src;
                if (std::string(kind) == "qr") {
                    std::lock_guard<std::mutex> lk(state.mtx); src = state.qr_dir;
                } else {
                    const std::string fp = teensy ? teensy->face_image_path("neutral")
                                                  : std::string();
                    if (!fp.empty()) {
                        fsx::path faces = fsx::path(fp).parent_path().parent_path();  // <assets>/faces
                        src = (std::string(kind) == "faces")
                                ? faces.string()
                                : (faces.parent_path() / "gifs").string();           // <assets>/gifs
                    }
                }
                const std::string k = kind;
                // Archive + share off-thread so a big folder doesn't stall the menu.
                std::thread([kdc_p, toast, src, k]{
                    namespace fsx = std::filesystem;
                    if (src.empty() || !fsx::exists(src)) {
                        toast("Export failed", "No " + k + " folder found"); return;
                    }
                    const std::string out    = "/tmp/protohud_" + k + ".tar.gz";
                    const std::string parent = fsx::path(src).parent_path().string();
                    const std::string base   = fsx::path(src).filename().string();
                    const std::string cmd = "tar -czf '" + out + "' -C '" + parent +
                                            "' '" + base + "' 2>/dev/null";
                    if (std::system(cmd.c_str()) != 0 || !fsx::exists(out)) {
                        toast("Export failed", "Could not archive " + k); return;
                    }
                    if (kdc_p && kdc_p->share_file(out)) toast("Sent to phone", k + ".tar.gz");
                    else                                 toast("Export ready", "Saved " + out);
                }).detach();
            };
            std::vector<MenuItem> export_menu = {
                with_desc(leaf("Export Faces",    [export_to_phone]{ export_to_phone("faces"); }),
                    "Bundle the faces folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export GIFs",     [export_to_phone]{ export_to_phone("gifs"); }),
                    "Bundle the gifs folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export QR Codes", [export_to_phone]{ export_to_phone("qr"); }),
                    "Bundle the captured QR-code folder into one .tar.gz and send it."),
                with_desc(leaf("Export All", [export_to_phone]{
                    export_to_phone("faces"); export_to_phone("gifs"); export_to_phone("qr"); }),
                    "Send all three bundles to the phone, one file each."),
                // Send the most recent photo/recording (newest file in the photo
                // dir, derived as the parent of map_dir).
                with_desc(leaf("Send Last Capture", [kdc_p, map_dir, toast]{
                    namespace fsx = std::filesystem;
                    const std::string dir = fsx::path(map_dir).parent_path().string();
                    std::error_code ec; fsx::path newest; fsx::file_time_type best{};
                    fsx::directory_iterator it(dir, ec), end;
                    for (; !ec && it != end; it.increment(ec)) {
                        if (!it->is_regular_file(ec)) continue;
                        const std::string ext = it->path().extension().string();
                        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".mp4")
                            continue;
                        const auto t = it->last_write_time(ec); if (ec) continue;
                        if (newest.empty() || t > best) { best = t; newest = it->path(); }
                    }
                    if (newest.empty()) { toast("No captures", "Nothing in the photo folder yet"); return; }
                    if (kdc_p && kdc_p->share_file(newest.string()))
                        toast("Sent to phone", newest.filename().string());
                    else toast("Export ready", newest.filename().string() + " (no phone)");
                }), "Share the newest photo/recording with the phone over KDE Connect."),
            };

            std::vector<MenuItem> phone_menu;
            phone_menu.push_back(with_desc(submenu("Pair Device", std::move(pair_menu)),
                "Pair or unpair a KDE Connect device. Reachable devices are listed; "
                "select an unpaired one and accept the prompt on the phone."));
            phone_menu.push_back(with_desc(submenu("Export to Phone", std::move(export_menu)),
                "Send your faces, GIFs or captured QR codes to the phone \xE2\x80\x94 each "
                "category bundled into a single .tar.gz over KDE Connect."));
            phone_menu.push_back(with_desc(leaf("Ring My Phone", ring_toast),
                "Ring the paired phone (KDE Connect findmyphone) so it plays its "
                "ringtone \xE2\x80\x94 handy for locating it."));
            phone_menu.push_back(with_desc(submenu("Notifications", std::move(notif_menu)),
                "Recent phone notifications. Reply to messaging apps or dismiss them "
                "(clears on the phone too)."));
            phone_menu.push_back(with_desc(submenu("Media", std::move(media_menu)),
                "Control the phone's media playback (play/pause, skip, volume, player)."));
            phone_menu.push_back(with_desc(submenu("Run Command", std::move(cmd_menu)),
                "Trigger one of the phone's saved KDE Connect commands."));
            phone_menu.push_back(with_desc(leaf("Send SMS\xE2\x80\xA6", send_sms_flow),
                "Send a text message through the phone (enter number, then message)."));
            phone_menu.push_back(with_desc(leaf("Mute Ringer", [kdc_p, toast]{
                if (kdc_p && kdc_p->mute_ringer()) toast("Ringer muted", "Silenced incoming call"); }),
                "Silence an incoming call's ringer (best-effort, depends on the phone)."));
            phone_menu.push_back(with_desc(conn_item,
                "Cellular network type + signal strength reported by the phone."));
            phone_menu.push_back(with_desc(submenu("Ignore List", std::move(ignore_menu)),
                "Mute notifications by app or sender. Apps/senders the phone has sent "
                "are grouped here \xE2\x80\x94 select one to mute it (muted entries turn red). "
                "\"Add Word\" still takes free text."));
            phone_menu.push_back(with_desc(submenu("Message Apps",
                list_menu(kdc_msg_p, apply_msg, "Add App\xE2\x80\xA6", "App name (e.g. Discord)")),
                "Apps whose notifications get the larger chat toast (sender + wrapped "
                "message, held longer). Select an entry to remove it."));
            return with_desc(submenu("Phone (KDE Connect)", std::move(phone_menu)),
                "Ring/mute, reply to & dismiss notifications, control media, run "
                "commands, send SMS, and mute apps/senders.");
    }();
    MenuItem notiflog_item = [&]() -> MenuItem {
        // ── Notification Log: grouped by sender, filterable, full text in panel ──
        // Type filter (radio).
        auto type_opt = [&state](const char* lbl, int t){
            return leaf_sel(lbl, [&state, t]{ state.notif_type_filter = t; },
                                 [&state, t]{ return state.notif_type_filter == t; });
        };
        std::vector<MenuItem> typ_menu = {
            type_opt("All Types",    -1),
            type_opt("Alarms",       static_cast<int>(NotifType::Alarm)),
            type_opt("Timers",       static_cast<int>(NotifType::Timer)),
            type_opt("LoRa",         static_cast<int>(NotifType::LoRa)),
            type_opt("Phone / Apps", static_cast<int>(NotifType::App)),
        };
        MenuItem typ_item = submenu("Filter: Type", std::move(typ_menu));
        typ_item.label_fn = [&state]{
            const char* t = state.notif_type_filter < 0 ? "All"
                : state.notif_type_filter == (int)NotifType::Alarm ? "Alarms"
                : state.notif_type_filter == (int)NotifType::Timer ? "Timers"
                : state.notif_type_filter == (int)NotifType::LoRa  ? "LoRa" : "Phone/Apps";
            return std::string("Filter: Type  [") + t + "]";
        };

        // Distinct sender names currently in the log (sorted).
        auto distinct_senders = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            std::set<std::string> s;
            for (const auto& n : state.notifs.items) if (!n.title.empty()) s.insert(n.title);
            return std::vector<std::string>(s.begin(), s.end());
        };
        // Sender checklist (multi-select).
        std::vector<MenuItem> snd_menu;
        snd_menu.push_back(with_desc(leaf("Show All Senders", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx); state.notif_sender_sel.clear();
        }), "Clear the selection so every sender shows."));
        for (int i = 0; i < 24; ++i) {
            MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
            m.label_fn   = [distinct_senders, i]{
                auto d = distinct_senders(); return i < (int)d.size() ? d[i] : std::string(); };
            m.visible_fn = [distinct_senders, i]{ return i < (int)distinct_senders().size(); };
            m.get_toggle = [distinct_senders, i, &state]{
                auto d = distinct_senders(); if (i >= (int)d.size()) return false;
                std::lock_guard<std::mutex> lk(state.mtx); return state.notif_sender_sel.count(d[i]) > 0; };
            m.set_toggle = [distinct_senders, i, &state](bool v){
                auto d = distinct_senders(); if (i >= (int)d.size()) return;
                std::lock_guard<std::mutex> lk(state.mtx);
                if (v) state.notif_sender_sel.insert(d[i]); else state.notif_sender_sel.erase(d[i]); };
            snd_menu.push_back(std::move(m));
        }
        MenuItem snd_item = submenu("Filter: Senders", std::move(snd_menu));
        snd_item.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            return state.notif_sender_sel.empty()
                ? std::string("Filter: Senders  [all]")
                : std::string("Filter: Senders  [") + std::to_string(state.notif_sender_sel.size()) + "]";
        };

        // Display order: queue indices passing the filters, grouped by sender
        // (sender A-Z, newest first within each). Rebuilt only when something changes.
        auto order     = std::make_shared<std::vector<int>>();
        auto order_key = std::make_shared<size_t>(SIZE_MAX);
        auto ensure_order = [&state, order, order_key]{
            std::lock_guard<std::mutex> lk(state.mtx);
            size_t key = static_cast<size_t>(state.notifs.next_id) * 1315423911u
                       + static_cast<size_t>(state.notif_type_filter + 2) * 2654435761u
                       + state.notifs.items.size();
            for (const auto& s : state.notif_sender_sel)
                key ^= std::hash<std::string>{}(s) + 0x9e3779b9u + (key << 6) + (key >> 2);
            if (key == *order_key) return;
            *order_key = key;
            order->clear();
            for (int i = 0; i < (int)state.notifs.items.size(); ++i) {
                const auto& n = state.notifs.items[i];
                if (state.notif_type_filter >= 0 &&
                    static_cast<int>(n.type) != state.notif_type_filter) continue;
                if (!state.notif_sender_sel.empty() && !state.notif_sender_sel.count(n.title)) continue;
                order->push_back(i);
            }
            std::stable_sort(order->begin(), order->end(), [&state](int a, int b){
                const auto& na = state.notifs.items[a]; const auto& nb = state.notifs.items[b];
                if (na.title != nb.title) return na.title < nb.title;
                return a < b;   // newest-first within a sender (queue is newest-first)
            });
        };

        std::vector<MenuItem> nlog_menu;
        // Fixed items (must stay 4 — the context panel maps cursor → row via -4).
        nlog_menu.push_back(with_desc(toggle("Persist Log",
            [&state]{ return state.notif_persist; },
            [&state](bool v){ state.notif_persist = v; }),
            "Save the log to disk so a sudden reboot doesn't lose it."));
        // Bulk clear — a small confirmation layer with the safe option first.
        // "Clear Unsaved" keeps anything the user pinned; "Clear All" wipes
        // everything including saved messages (red).
        std::vector<MenuItem> clr_menu;
        clr_menu.push_back(with_desc(leaf("Cancel", [menu_sys_pp]{
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); }),
            "Back out without clearing anything."));
        clr_menu.push_back(with_desc(leaf("Clear Unsaved", [&state, menu_sys_pp]{
            { std::lock_guard<std::mutex> lk(state.mtx);
              state.notifs.clear_if([](const Notification&){ return true; }, /*include_saved=*/false); }
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
        }), "Remove everything except messages you've saved."));
        {
            MenuItem all = leaf("Clear All", [&state, menu_sys_pp]{
                { std::lock_guard<std::mutex> lk(state.mtx); state.notifs.items.clear(); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            all.warn_fn = []{ return true; };   // red — also drops saved messages
            clr_menu.push_back(with_desc(std::move(all),
                "Remove every notification, including saved ones."));
        }
        MenuItem clr_item = submenu("Clear Log\xE2\x80\xA6", std::move(clr_menu));
        clr_item.context_panel_title = "Clear";
        clr_item.context_panel_draw  = [&state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            (void)sz; ImFont* f = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            int total, saved; { std::lock_guard<std::mutex> lk(state.mtx);
                total = (int)state.notifs.items.size(); saved = 0;
                for (const auto& n : state.notifs.items) saved += n.saved ? 1 : 0; }
            char ln[64]; float y = o.y;
            snprintf(ln, sizeof(ln), "%d total", total);
            dl->AddText(f, fs, {o.x,y}, IM_COL32(235,240,245,255), ln); y += fs*1.4f;
            snprintf(ln, sizeof(ln), "%d saved \xC2\xB7 %d unsaved", saved, total - saved);
            dl->AddText(f, fs*0.85f, {o.x,y}, IM_COL32(190,196,204,230), ln);
        };
        nlog_menu.push_back(with_desc(std::move(clr_item),
            "Bulk-clear the log: keep saved messages or wipe everything."));
        nlog_menu.push_back(with_desc(std::move(typ_item), "Show only the chosen type."));
        nlog_menu.push_back(with_desc(std::move(snd_item),
            "Tick which senders to show. None ticked = all senders."));
        // Message rows (grouped by sender, scrollable). Full text shows in the panel.
        // Draw a single notification (full text) into a panel — used by both the
        // log preview and the per-row delete confirmation.
        auto draw_one = [&state, order, ensure_order](ImDrawList* dl, ImVec2 o, ImVec2 sz,
                                                      int slot, bool confirm){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            if (confirm) {
                dl->AddText(font, fs * 0.95f, {o.x, y}, IM_COL32(255,110,100,255),
                            "Delete this message?");
                y += fs * 1.4f;
            }
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);
        };
        // Selecting a row opens a per-message popup with the full text in the
        // panel and three actions: Save (pin), Confirm Delete (red), Cancel.
        // Helper: queue index of the message currently at display slot `i`, or -1.
        auto qindex = [&state, order, ensure_order](int i) -> int {
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)order->size()) return -1;
            const int qi = (*order)[i];
            return qi < (int)state.notifs.items.size() ? qi : -1;
        };
        for (int i = 0; i < NotificationQueue::kMax; ++i) {
            MenuItem m; m.type = MenuItemType::SUBMENU; m.label = "msg";
            m.label_fn = [&state, qindex, i]{
                const int qi = qindex(i);
                std::lock_guard<std::mutex> lk(state.mtx);
                if (qi < 0) return std::string();
                const auto& n = state.notifs.items[qi];
                std::string s = n.saved ? std::string("\xE2\x98\x85 ") : std::string();  // ★ if saved
                s += n.title;
                if (!n.body.empty()) s += "  \xC2\xB7  " + n.body;
                if (s.size() > 60) s = s.substr(0, 59) + "\xE2\x80\xA6";
                return s;
            };
            m.visible_fn = [order, ensure_order, i]{ ensure_order(); return i < (int)order->size(); };
            // Save / pin toggle. qindex() locks internally, so resolve it first,
            // then re-validate the index after re-acquiring the lock.
            MenuItem save; save.type = MenuItemType::TOGGLE; save.label = "Save Message";
            save.get_toggle = [&state, qindex, i]{
                const int qi = qindex(i); std::lock_guard<std::mutex> lk(state.mtx);
                return qi >= 0 && qi < (int)state.notifs.items.size() && state.notifs.items[qi].saved; };
            save.set_toggle = [&state, qindex, i](bool v){
                const int qi = qindex(i);
                { std::lock_guard<std::mutex> lk(state.mtx);
                  if (qi >= 0 && qi < (int)state.notifs.items.size()) state.notifs.items[qi].saved = v; }
                state.notif_dirty.store(true);   // metadata-only edit → force a flush
            };
            MenuItem confirm = leaf("Confirm Delete",
                [&state, qindex, i, menu_sys_pp]{
                    const int qi = qindex(i);
                    { std::lock_guard<std::mutex> lk(state.mtx);
                      if (qi >= 0 && qi < (int)state.notifs.items.size())
                          state.notifs.items.erase(state.notifs.items.begin() + qi); }
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();   // back to the log
                });
            confirm.warn_fn = []{ return true; };   // red — destructive
            MenuItem cancel = leaf("Cancel",
                [menu_sys_pp]{ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); });
            m.children = { with_desc(std::move(save), "Pin this message so Clear and the rolling buffer keep it."),
                           std::move(confirm), std::move(cancel) };
            m.context_panel_title = "Message";
            m.context_panel_draw  = [draw_one, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_one(dl, o, sz, i, false); };
            nlog_menu.push_back(std::move(m));
        }
        // Context panel: full text of the focused row.
        auto panel = [&state, order, ensure_order, menu_sys_pp]
                     (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const int idx  = (menu_sys_pp && *menu_sys_pp) ? (*menu_sys_pp)->current_index() : -1;
            const int slot = idx - 4;   // 4 fixed items precede the rows
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message to read it here.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);   // wrapped
        };
        return with_panel(with_desc(submenu("Notification Log", std::move(nlog_menu)),
            "Browse the log grouped by sender; scroll to a message to read its full "
            "text on the right. Filter by type/sender, save messages to pin them, or "
            "bulk-clear the rest."),
            "Message", panel);
    }();
    std::vector<MenuItem> communications_menu;
    communications_menu.push_back(with_desc(submenu("LoRa", std::move(lora_menu)),
        "Long-range radio: team nodes, messages and status."));
    // KDE Connect now lives under System > Connectivity (alongside SSH/Bluetooth);
    // Communications keeps the notification log + QR codes.
    connectivity_menu.push_back(std::move(phone_item));
    communications_menu.push_back(std::move(notiflog_item));

    // ── Scanned QR Codes ──────────────────────────────────────────────────
    // Each captured code lives in its own folder (link + raw image). This
    // submenu browses the de-duplicated running list; the panel shows the
    // captured image + link, and each row can Open (URLs) or Delete.
    MenuItem qr_codes_item = [&]() -> MenuItem {
        struct QrThumb { GLuint tex = 0; std::string loaded; cv::Mat img; };
        auto thumb = std::make_shared<QrThumb>();
        auto focus = std::make_shared<int>(-1);

        auto cap_at = [&state](int i) -> QrCapture {
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i < 0 || i >= static_cast<int>(state.qr_captures.items.size())) return {};
            return state.qr_captures.items[i];
        };
        auto is_url = [](const std::string& t){
            return t.size() > 7 && (t.compare(0, 7, "http://") == 0 ||
                                    t.compare(0, 8, "https://") == 0);
        };
        // Shared panel painter: thumbnail (top) + link (wrapped) + type/time.
        auto draw_cap = [thumb](ImDrawList* dl, ImVec2 o, ImVec2 sz, const QrCapture& c){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            if (c.text.empty()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a code.");
                return;
            }
            float y = o.y;
            // Prefer the colour camera frame; fall back to the grayscale decode.
            std::string ipath;
            const std::string& fn = !c.image.empty() ? c.image : c.decode;
            if (!c.folder.empty() && !fn.empty())
                ipath = (std::filesystem::path(c.folder) / fn).string();
            if (!ipath.empty()) {
                if (ipath != thumb->loaded) {
                    thumb->img = face::load_png_rgba(ipath, 240, 180);
                    thumb->loaded = ipath;
                }
                const float pw = std::min(sz.x * 0.85f, 200.f), ph = pw * 0.75f;
                const float px = o.x, py = y;
                dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10,16,22,190));
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
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumb->img.cols, thumb->img.rows,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb->img.data);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    dl->AddImage(reinterpret_cast<ImTextureID>(
                        static_cast<uintptr_t>(thumb->tex)), {px, py}, {px + pw, py + ph});
                }
                y = py + ph + 8.f;
            }
            dl->AddText(font, fs * 0.9f, {o.x, y}, IM_COL32(235,240,245,255),
                        c.text.c_str(), nullptr, sz.x - 6.f);
            ImVec2 tsz = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, sz.x - 6.f, c.text.c_str());
            y += tsz.y + 6.f;
            char meta[80]; std::string when;
            if (c.timestamp > 0) {
                char ts[24]; time_t t = static_cast<time_t>(c.timestamp);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&t));
                when = ts;
            }
            std::snprintf(meta, sizeof(meta), "%s%s%s", c.type.c_str(),
                          when.empty() ? "" : "  \xC2\xB7  ", when.c_str());
            dl->AddText(font, fs * 0.72f, {o.x, y}, IM_COL32(150,158,166,210), meta);
        };

        // List removable mounts (USB sticks etc.) under the auto-mount roots as
        // {path, display}. Cached ~1 s so the menu's per-slot label/visible fns
        // don't readdir every frame. Cheap listing only (no write probe).
        auto usb_cache   = std::make_shared<std::vector<std::pair<std::string,std::string>>>();
        auto usb_cache_t = std::make_shared<double>(-100.0);
        auto list_usb_mounts =
            [usb_cache, usb_cache_t]() -> const std::vector<std::pair<std::string,std::string>>& {
            const double now = ImGui::GetTime();
            if (now - *usb_cache_t > 1.0) {
                *usb_cache_t = now;
                usb_cache->clear();
                namespace fs = std::filesystem;
                std::vector<std::string> roots;
                std::error_code ec;
                if (const char* u = std::getenv("USER")) {
                    const std::string mu = std::string("/media/") + u;
                    if (fs::is_directory(mu, ec)) roots.push_back(mu);
                }
                if (roots.empty()) roots.push_back("/media");
                roots.push_back("/mnt");
                for (const auto& root : roots) {
                    if (!fs::is_directory(root, ec)) continue;
                    for (const auto& e : fs::directory_iterator(root, ec))
                        if (e.is_directory(ec))
                            usb_cache->emplace_back(e.path().string(),
                                                    e.path().filename().string());
                }
            }
            return *usb_cache;
        };
        // Derive a filename-safe, human-ish name from a code's text so the image
        // matches its link in the manifest. Host/path for URLs, else first chars,
        // plus a short hash of the full text to keep it unique.
        auto qr_parsed_name = [](const std::string& text) -> std::string {
            std::string base = text;
            const auto scheme = base.find("://");
            if (scheme != std::string::npos) base = base.substr(scheme + 3);
            std::string out;
            for (char ch : base) {
                if (std::isalnum(static_cast<unsigned char>(ch))) out += ch;
                else if (ch == '.' || ch == '-' || ch == '_') out += ch;
                else out += '_';
                if (out.size() >= 40) break;
            }
            if (out.empty()) out = "qr";
            char hx[8];
            std::snprintf(hx, sizeof(hx), "%04x",
                          static_cast<unsigned>(std::hash<std::string>{}(text) & 0xffff));
            return out + "_" + hx;
        };
        // Stage a temp bundle: each code's image copied to <parsed_name>.png plus
        // a qr_links.txt manifest mapping names → links. Returns the file list.
        auto build_qr_bundle = [qr_parsed_name](const std::vector<QrCapture>& caps)
            -> std::vector<std::string> {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path(ec) / "protohud_qr_share";
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            std::vector<std::string> files;
            std::ofstream man((dir / "qr_links.txt").string());
            if (man) man << "ProtoHUD scanned codes\n======================\n\n";
            for (const auto& c : caps) {
                const std::string nm = qr_parsed_name(c.text);
                std::string src;
                if (!c.folder.empty()) {
                    for (const char* pref : {"camera.png", "decode.png"}) {
                        const fs::path p = fs::path(c.folder) / pref;
                        if (fs::exists(p, ec)) { src = p.string(); break; }
                    }
                    if (src.empty())
                        for (const auto& e : fs::directory_iterator(c.folder, ec))
                            if (e.is_regular_file() && e.path().extension() == ".png") {
                                src = e.path().string(); break; }
                }
                if (!src.empty()) {
                    const fs::path dst = dir / (nm + ".png");
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (!ec) files.push_back(dst.string());
                }
                if (man) {
                    char when[24] = "";
                    if (c.timestamp > 0) { time_t t = static_cast<time_t>(c.timestamp);
                        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t)); }
                    man << nm << ".png\n    " << c.text << "\n    [" << c.type
                        << (when[0] ? std::string("  ") + when : std::string()) << "]\n\n";
                }
            }
            if (man) { man.close(); files.push_back((dir / "qr_links.txt").string()); }
            return files;
        };
        // Send a set of captures (images + manifest) to the paired phone.
        auto send_to_phone = [kdc_p, build_qr_bundle, &state](const std::vector<QrCapture>& caps) {
            const bool dev = kdc_p && kdc_p->device_ready();
            int n = 0;
            if (dev) for (const auto& f : build_qr_bundle(caps))
                if (kdc_p->share_file(f)) ++n;
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = dev ? "Sent to phone" : "No phone connected";
            nt.body  = dev ? (std::to_string(n) + " file(s) shared (images + qr_links.txt)")
                           : "Pair + connect a device in KDE Connect first.";
            nt.auto_dismiss_s = 5.f; state.notifs.push(std::move(nt));
        };
        // Copy the bundle (renamed images + manifest) to a chosen mount.
        auto export_to_usb = [build_qr_bundle, &state](const std::vector<QrCapture>& caps,
                                                       const std::string& dest) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::path(dest) / "protohud_qr";
            fs::create_directories(target, ec);
            int n = 0;
            for (const auto& f : build_qr_bundle(caps)) {
                fs::copy_file(f, target / fs::path(f).filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = n > 0 ? "Exported to USB" : "Export failed";
            nt.body  = n > 0 ? ("Copied " + std::to_string(n) + " file(s) to " + target.string())
                             : ("Could not write to " + dest);
            nt.auto_dismiss_s = 6.f; state.notifs.push(std::move(nt));
        };
        // Build the "Export / Send" target list (phone + each connected drive).
        // get_caps() returns the captures to export (one row, or all).
        auto build_export_targets =
            [kdc_p, list_usb_mounts, send_to_phone, export_to_usb](
                std::function<std::vector<QrCapture>()> get_caps) {
            std::vector<MenuItem> t;
            MenuItem p; p.type = MenuItemType::LEAF; p.label = "phone";
            p.label_fn   = [kdc_p]{ return std::string("Phone: ") +
                                    (kdc_p ? kdc_p->active_device_name() : std::string()); };
            p.visible_fn = [kdc_p]{ return kdc_p && kdc_p->device_ready(); };
            p.action     = [send_to_phone, get_caps]{ send_to_phone(get_caps()); };
            t.push_back(std::move(p));
            for (int j = 0; j < 6; ++j) {
                MenuItem m; m.type = MenuItemType::LEAF; m.label = "drive";
                m.label_fn   = [list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    return j < (int)v.size() ? ("USB: " + v[j].second) : std::string(); };
                m.visible_fn = [list_usb_mounts, j]{ return j < (int)list_usb_mounts().size(); };
                m.action     = [export_to_usb, get_caps, list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    if (j < (int)v.size()) export_to_usb(get_caps(), v[j].first); };
                t.push_back(std::move(m));
            }
            MenuItem none; none.type = MenuItemType::LEAF; none.label = "(no devices connected)";
            none.visible_fn = [kdc_p, list_usb_mounts]{
                return !(kdc_p && kdc_p->device_ready()) && list_usb_mounts().empty(); };
            t.push_back(std::move(none));
            return t;
        };

        std::vector<MenuItem> qmenu;
        // Clear All (red) — drops every entry + its folder.
        {
            MenuItem clr = leaf("Clear All", [&state]{
                std::vector<std::string> folders;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    for (auto& c : state.qr_captures.items)
                        if (!c.folder.empty()) folders.push_back(c.folder);
                    state.qr_captures.items.clear();
                    state.qr_captures.seen.clear();
                    qr_write_index(state.qr_dir, state.qr_captures);
                }
                std::error_code ec;
                for (auto& f : folders) std::filesystem::remove_all(f, ec);
            });
            clr.warn_fn    = []{ return true; };
            clr.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(clr),
                "Delete every saved QR code and its folder."));
        }
        // Export / Send All — to a chosen connected device (phone or a drive).
        // One bundle with every code's image + a combined qr_links.txt manifest.
        {
            MenuItem all = submenu("Export / Send All", build_export_targets(
                [&state]() -> std::vector<QrCapture> {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return std::vector<QrCapture>(state.qr_captures.items.begin(),
                                                  state.qr_captures.items.end());
                }));
            all.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(all),
                "Send all saved codes to a connected device as named images plus a "
                "single qr_links.txt listing every link."));
        }
        // Capture rows (newest first).
        for (int i = 0; i < 40; ++i) {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "qr";
            row.label_fn = [&state, i]{
                std::lock_guard<std::mutex> lk(state.mtx);
                if (i >= static_cast<int>(state.qr_captures.items.size())) return std::string();
                std::string s = state.qr_captures.items[i].text;
                if (s.size() > 48) s = s.substr(0, 47) + "\xE2\x80\xA6";
                return s;
            };
            row.visible_fn   = [&state, i]{ std::lock_guard<std::mutex> lk(state.mtx);
                                 return i < static_cast<int>(state.qr_captures.items.size()); };
            row.on_highlight = [focus, i]{ *focus = i; };
            MenuItem open = leaf("Open Link", [cap_at, is_url, i]{
                QrCapture c = cap_at(i);
                if (!is_url(c.text)) return;
                std::string safe = c.text; for (auto& ch : safe) if (ch == '\'') ch = ' ';
                std::string cmd = "xdg-open '" + safe + "' >/dev/null 2>&1 &";
                system(cmd.c_str());
            });
            open.visible_fn = [cap_at, is_url, i]{ return is_url(cap_at(i).text); };
            MenuItem del = leaf("Delete", [&state, i, menu_sys_pp]{
                std::string folder;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (i < static_cast<int>(state.qr_captures.items.size())) {
                        folder = state.qr_captures.items[i].folder;
                        state.qr_captures.seen.erase(state.qr_captures.items[i].text);
                        state.qr_captures.items.erase(state.qr_captures.items.begin() + i);
                        qr_write_index(state.qr_dir, state.qr_captures);
                    }
                }
                if (!folder.empty()) { std::error_code ec;
                    std::filesystem::remove_all(folder, ec); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            del.warn_fn = []{ return true; };
            MenuItem exp = submenu("Export / Send", build_export_targets(
                [cap_at, i]() -> std::vector<QrCapture> { return { cap_at(i) }; }));
            exp.visible_fn = [cap_at, i]{ return !cap_at(i).folder.empty(); };
            exp.description = "Send this code (image + a qr_links.txt) to a connected "
                              "device — the paired phone (KDE Connect) or a USB drive.";
            row.children = { std::move(open), std::move(exp), std::move(del) };
            row.context_panel_title = "QR Code";
            row.context_panel_draw  = [draw_cap, cap_at, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_cap(dl, o, sz, cap_at(i)); };
            qmenu.push_back(std::move(row));
        }
        MenuItem sub = submenu("QR Codes", std::move(qmenu));
        sub.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            const size_t n = state.qr_captures.items.size();
            return n ? ("QR Codes  [" + std::to_string(n) + "]") : std::string("QR Codes");
        };
        sub.context_panel_title = "QR Code";
        sub.context_panel_draw  = [draw_cap, focus, &state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            QrCapture c;
            { std::lock_guard<std::mutex> lk(state.mtx);
              if (*focus >= 0 && *focus < static_cast<int>(state.qr_captures.items.size()))
                  c = state.qr_captures.items[*focus]; }
            draw_cap(dl, o, sz, c);
        };
        return with_desc(std::move(sub),
            "Scanned QR/barcodes — each saved in its own folder with the link "
            "and the captured image. Already-seen codes aren't captured twice.");
    }();
    communications_menu.push_back(std::move(qr_codes_item));

    // Audio submenu wrapped with its existing live-status context panel.
    MenuItem audio_item = with_panel(
        submenu("Audio", std::move(audio_menu)),
        "Audio Status",
        [&state, menu_sys_pp]
        (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            (void)sz;
            const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                ? (*menu_sys_pp)->accent_color()
                : IM_COL32(255, 255, 255, 255);
            const float lh = 18.f;
            ImVec2 p = o;
            auto row = [&](const char* k, const char* v, ImU32 vc) {
                dl->AddText({ p.x, p.y }, IM_COL32(180, 180, 180, 200), k);
                dl->AddText({ p.x + 130.f, p.y }, vc, v);
                p.y += lh;
            };
            bool enabled;  int  out_idx;  float cpu;  int  xruns;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                enabled = state.audio.enabled;
                out_idx = state.audio.output;
                cpu     = state.audio.cpu_load;
                xruns   = state.audio.xrun_count;
            }
            const char* out_name =
                out_idx == 0 ? "VITURE" :
                out_idx == 1 ? "Headphones" :
                out_idx == 2 ? "HDMI" : "?";
            row("State",  enabled ? "Enabled" : "Muted",
                enabled ? ((accent & 0x00FFFFFFu) | (220u << 24))
                        : IM_COL32(200, 90, 90, 220));
            row("Output", out_name, IM_COL32(230, 230, 230, 230));
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f %%", cpu * 100.f);
            row("CPU load", buf, IM_COL32(230, 230, 230, 230));
            snprintf(buf, sizeof(buf), "%d", xruns);
            row("XRuns", buf,
                xruns == 0 ? IM_COL32(160, 220, 160, 220)
                           : IM_COL32(220, 160, 90, 220));
        });

    // ── System root: grouped, one screen tall ────────────────────────────────
    std::vector<MenuItem> system_menu = {
        with_desc(submenu("Display", std::move(display_menu)),
                  "Fullscreen / frameless window mode and windowed resolution."),
        with_desc(submenu("HUD & Menu", std::move(display_hud_menu)),
                  "Text scale, HUD/menu theme + presets, radial-menu options."),
        std::move(audio_item),
        with_desc(submenu("Connectivity",     std::move(connectivity_menu)),
                  "SSH, Bluetooth and other network/peripheral toggles."),
        with_desc(submenu("Pi Settings",      std::move(pi_settings_items)),
                  "Hostname, time, storage, GPIO visualizer/buttons and cooling fans."),
        with_desc(submenu("XR Headset (Viture Beast)", std::move(headset_menu)),
                  "Electrochromic transparency, HUD/backlight brightness, recenter, "
                  "gaze lock and 3D side-by-side \xe2\x80\x94 specific to the glasses."),
        with_desc(submenu("Timers and Alarm",   std::move(timers_alarm_menu)),
                  "Stopwatches, countdowns and one-shot alarms."),
        with_desc(submenu("Diagnostics",        std::move(diagnostics_group_menu)),
                  "Probes, overlays and per-tab tools."),
        with_desc(submenu("Power",              std::move(power_menu)),
                  "Restart ProtoHUD / ProtoFace, reboot or shut down the Pi."),
    };

    // ── Profiles ────────────────────────────────────────────────────────────────
    // Save the current setup as a named snapshot, load one (relaunches ProtoHUD),
    // or delete one. The load/delete lists are dynamic: label_fn/visible_fn read the
    // ProfileManager live, so newly-saved profiles appear without rebuilding the menu.
    // (kProfileSlots declared above, near the HUD/Menu Presets section.)
    std::vector<MenuItem> profile_delete_menu;
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type       = MenuItemType::LEAF;
        m.label      = "profile";
        m.label_fn   = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.visible_fn = [profiles, i]{ return profiles && i < profiles->count(); };
        m.description = "Delete this profile permanently.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_delete_name = nm;
        };
        profile_delete_menu.push_back(std::move(m));
    }

    std::vector<MenuItem> profiles_menu;
    profiles_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Profile Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->profile_save_name = name;
                });
        }),
        "Save every current setting (HUD layout, menu style, camera/vision, "
        "Protoface look) as a named profile you can switch to later."));
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "profile";
        m.label_fn    = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.visible_fn  = [profiles, i]{ return profiles && i < profiles->count(); };
        m.description = "Load this profile. ProtoHUD restarts to apply it.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_load_name = nm;
        };
        profiles_menu.push_back(std::move(m));
    }
    profiles_menu.push_back(with_desc(submenu("Delete Profile", std::move(profile_delete_menu)),
        "Remove a saved profile permanently."));

    // Assemble Software last, now that profiles_menu exists: the in-HUD Updater
    // first, then Demo Mode (appended earlier), then Profiles & Backup. Added to
    // the System root as a single "Software" tab.
    software_menu.insert(software_menu.begin(),
        with_desc(submenu("Updates", std::move(updates_menu)),
            "Check the repo for new commits, switch/update branches, and roll back "
            "\xe2\x80\x94 all user-initiated. ProtoHUD never auto-updates."));
    // Pi system (apt) update — confirm-gated, since it changes the OS. Runs in
    // the background and toasts on completion.
    software_menu.push_back(confirm_action("Pi System Update",
        "Runs apt-get update && apt-get -y upgrade in the background (30 s to "
        "several minutes). Don't power off while it runs.",
        [run_sh]{ run_sh("System Update",
            "sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get -y upgrade"); }));
    // Submit Issue — compose a short report; saved under state/issues/ and (when a
    // phone is connected) shared to it so it can be filed on GitHub. No network /
    // token is used on the Pi, so it can't post directly.
    software_menu.push_back(with_desc(leaf("Submit Issue\xE2\x80\xA6",
        [menu_sys_pp, &state, kdc_p]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Describe the issue", "",
                [&state, kdc_p](const std::string& text){
                    if (text.empty()) return;
                    char exe[4096]; ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                    std::string root;
                    if (n > 0) { exe[n] = '\0'; std::string p(exe);
                        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);
                        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);
                        root = p; }
                    const std::string dir = (root.empty() ? std::string("/tmp")
                                                          : root + "/state/issues");
                    std::error_code ec; std::filesystem::create_directories(dir, ec);
                    std::time_t t = std::time(nullptr); char ts[32];
                    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
                    const std::string path = dir + "/issue_" + ts + ".txt";
                    { std::ofstream f(path);
                      if (f) f << "ProtoHUD issue (" << ts << ")\n\n" << text << "\n\n"
                               << "File at: https://github.com/brooksthemaker/protohud/issues/new\n"; }
                    if (kdc_p) kdc_p->share_file(path);   // hand off to the phone if paired
                    Notification nt; nt.type = NotifType::App; nt.icon = "message";
                    nt.title = "Issue saved"; nt.body = path; nt.auto_dismiss_s = 6.f;
                    std::lock_guard<std::mutex> lk(state.mtx); state.notifs.push(std::move(nt));
                });
        }),
        "Write a short bug report. Saved under state/issues/ and shared to the "
        "phone (if connected) so you can file it on GitHub."));
    software_menu.push_back(with_desc(submenu("Profiles & Backup", std::move(profiles_menu)),
        "Save and load full-setup snapshots. Loading a profile restarts ProtoHUD "
        "with that config."));
    system_menu.push_back(with_desc(submenu("Software", std::move(software_menu)),
        "In-HUD updater, Pi system update, demo scenes, issue reports, and "
        "full-setup profiles & backups."));

    // ── Quick (corner / radial) menu ─────────────────────────────────────────────
    // A short, curated set of mid-use actions — separate from the full settings tree
    // above. Some items are always present; an optional "catalog" of extras can be
    // pinned by the user (favorites, persisted in config) and is gated by visible_fn.
    // Submenus here become outer rings in the radial renderer.
    if (quick_out) {
        std::vector<MenuItem> q;

        // Night vision (mirror of the Low-Light "Night Vision" toggle).
        q.push_back(toggle("Night Vision",
            [&state]{ return state.night_vision.nv_enabled; },
            [&state](bool v){
                state.night_vision.nv_enabled  = v;
                state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
                state.night_vision.shutter_us  = v ? 40000 : 16667;
            }));

        // Digital zoom (both eyes together).
        q.push_back(slider("Zoom", 1.0f, 4.0f, 0.25f, "x",
            [&state]{ return state.zoom_left.zoom; },
            [&state](float v){ state.zoom_left.zoom = v; state.zoom_right.zoom = v; }));

        // Manual focus — only when OWLsight cameras are present.
        {
            std::vector<MenuItem> focus = {
                leaf("Autofocus", [cameras]{
                    if (cameras && cameras->owl_left())  cameras->owl_left()->start_autofocus();
                    if (cameras && cameras->owl_right()) cameras->owl_right()->start_autofocus();
                }),
                slider("Position", 0.f, 1000.f, 25.f, "",
                    [cameras]{ return (cameras && cameras->owl_left())
                                        ? (float)cameras->owl_left()->get_focus_position() : 0.f; },
                    [cameras](float v){
                        if (cameras && cameras->owl_left())  cameras->owl_left()->set_focus_position((int)v);
                        if (cameras && cameras->owl_right()) cameras->owl_right()->set_focus_position((int)v);
                    }),
            };
            MenuItem fsub = submenu("Manual Focus", std::move(focus));
            fsub.visible_fn = [cameras]{ return cameras && cameras->owl_left(); };
            q.push_back(std::move(fsub));
        }

        // Quick photo → opens a sub-ring of capture modes; entering it locks focus
        // (stops AF + manual) so the whole burst shares one focus. All shoot both eyes.
        {
            auto shoot = [state_ptr](int extra) {
                if (!state_ptr) return;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                state_ptr->capture_request = CaptureRequest::Stereo;
                state_ptr->capture_burst   = extra;
            };
            std::vector<MenuItem> photo = {
                leaf("Single",   [shoot]{ shoot(0); }),
                leaf("Burst x3", [shoot]{ shoot(2); }),
                leaf("Burst",    [shoot]{ shoot(7); }),
            };
            MenuItem pm = submenu("Quick Photo", std::move(photo));
            pm.action = [cameras, state_ptr] {            // on-enter: lock focus
                if (cameras) {
                    if (cameras->owl_left())  cameras->owl_left()->stop_autofocus();
                    if (cameras->owl_right()) cameras->owl_right()->stop_autofocus();
                }
                if (state_ptr) {
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->focus_left.mode  = CameraFocusState::Mode::MANUAL;
                    state_ptr->focus_right.mode = CameraFocusState::Mode::MANUAL;
                }
            };
            q.push_back(std::move(pm));
        }
        q.push_back(toggle("Record",
            [&state]{ return state.video_recording; },
            [&state](bool v){ std::lock_guard<std::mutex> lk(state.mtx);
                              state.video_request = v ? VideoRequest::Start : VideoRequest::Stop; }));

        // Timers.
        {
            auto set_timer = [&state](int secs){
                state.timer_alarm.timer_active = true;
                state.timer_alarm.timer_end    = time(nullptr) + secs;
            };
            std::vector<MenuItem> timers = {
                leaf("5 min",  [set_timer]{ set_timer(300);  }),
                leaf("10 min", [set_timer]{ set_timer(600);  }),
                leaf("30 min", [set_timer]{ set_timer(1800); }),
                leaf("60 min", [set_timer]{ set_timer(3600); }),
                leaf("Cancel", [&state]{ state.timer_alarm.timer_active = false; }),
            };
            q.push_back(submenu("Timers", std::move(timers)));
        }

        // Expand Map — open the Helldivers-style pan/zoom view (closes the wheel).
        q.push_back(leaf("Expand Map", [state_ptr, menu_sys_pp]{
            if (state_ptr) {
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                state_ptr->map_overlay.expanded   = true;
                state_ptr->map_overlay.view_zoom  = 1.f;
                state_ptr->map_overlay.view_pan_x = 0.f;
                state_ptr->map_overlay.view_pan_y = 0.f;
            }
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->close();
        }));

        // ── Favorites catalog (optional, user-pinned) ────────────────────────────
        struct QuickFav { std::string key; MenuItem item; };
        std::vector<QuickFav> catalog;
        catalog.push_back({ "edge", toggle("Edge Highlight",
            [&state]{ return state.pp_cfg.edge_enabled; },
            [&state](bool v){ state.pp_cfg.edge_enabled = v; }) });
        catalog.push_back({ "desat", toggle("Desaturate BG",
            [&state]{ return state.pp_cfg.desat_enabled; },
            [&state](bool v){ state.pp_cfg.desat_enabled = v; }) });
        catalog.push_back({ "motion", toggle("Motion Highlight",
            [&state]{ return state.pp_cfg.motion_enabled; },
            [&state](bool v){ state.pp_cfg.motion_enabled = v; }) });
        catalog.push_back({ "map", toggle("Map Overlay",
            [&state]{ return state.map_overlay.enabled; },
            [&state](bool v){ state.map_overlay.enabled = v; }) });
        catalog.push_back({ "theater", toggle("Theater Mode",
            [&state]{ return state.theater_mode; },
            [&state](bool v){ state.theater_mode = v; }) });
        catalog.push_back({ "swap", toggle("Swap Cameras",
            [&state]{ return state.cameras_swapped; },
            [&state](bool v){ state.cameras_swapped = v; }) });
        catalog.push_back({ "fps", toggle("FPS Overlay",
            [fps_overlay_active]{ return fps_overlay_active && *fps_overlay_active; },
            [fps_overlay_active](bool v){ if (fps_overlay_active) *fps_overlay_active = v; }) });
        catalog.push_back({ "syspanel", toggle("System Panel",
            [sys_panel_active]{ return sys_panel_active && *sys_panel_active; },
            [sys_panel_active](bool v){ if (sys_panel_active) *sys_panel_active = v; }) });
        // Action item (not a toggle): rings the paired phone via KDE Connect.
        catalog.push_back({ "ring_phone", leaf("Ring Phone", [kdc_p, &state]{
            const bool ok = kdc_p && kdc_p->ring_phone();
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification n; n.type = NotifType::App; n.icon = "message";
            n.title = ok ? "Ringing phone\xE2\x80\xA6" : "Phone not connected";
            n.body  = ok ? "KDE Connect \xC2\xB7 findmyphone"
                         : "Pair a device in the KDE Connect app first";
            n.auto_dismiss_s = 4.f;
            state.notifs.push(std::move(n));
        }) });
        // Diagnostics — quick recovery actions (CSI reinit + face-renderer restart).
        {
            std::vector<MenuItem> diag;
            diag.push_back(with_desc(leaf("Reinit CSI Cameras", [cameras, &state]{
                const bool ok = cameras->reinit_owls();
                const bool lok = cameras->owl_left_ok(), rok = cameras->owl_right_ok();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "CSI cameras reinitialized" : "CSI reinit: no camera";
                char b[64]; snprintf(b, sizeof(b), "Left %s  \xC2\xB7  Right %s",
                                     lok ? "OK" : "\xE2\x80\x94", rok ? "OK" : "\xE2\x80\x94");
                n.body = b; n.auto_dismiss_s = 5.f; state.notifs.push(std::move(n));
            }), "Re-enumerate + restart the CSI cameras to recover a dark eye."));
            if (pf_restart_renderer) {
                MenuItem rf = with_desc(leaf("Restart Face Renderer", [pf_restart_renderer, state_ptr]{
                    pf_restart_renderer();
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    Notification n; n.type = NotifType::App; n.title = "Face renderer restarted";
                    n.body = "Relaunched the HUB75 panel driver"; n.auto_dismiss_s = 4.f;
                    state_ptr->notifs.push(std::move(n));
                }), "Kill + relaunch the HUB75 panel pusher to recover the face feed.");
                rf.visible_fn = [pf_backend_p]{ return !pf_backend_p || *pf_backend_p == "hub75"; };
                diag.push_back(std::move(rf));
            }
            catalog.push_back({ "diagnostics", submenu("Diagnostics", std::move(diag)) });
        }

        // Pinned catalog items appear in the quick menu, gated on the favorites set.
        for (auto& f : catalog) {
            MenuItem it  = f.item;
            std::string key = f.key;
            it.visible_fn = [state_ptr, key]{
                if (!state_ptr) return false;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                return state_ptr->quick_favorites.count(key) > 0;
            };
            q.push_back(std::move(it));
        }

        // "Customize" — toggles to pin/unpin each catalog item.
        std::vector<MenuItem> customize;
        for (auto& f : catalog) {
            std::string key   = f.key;
            std::string label = f.item.label;
            customize.push_back(toggle(label,
                [state_ptr, key]{
                    if (!state_ptr) return false;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    return state_ptr->quick_favorites.count(key) > 0;
                },
                [state_ptr, key](bool v){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    if (v) state_ptr->quick_favorites.insert(key);
                    else   state_ptr->quick_favorites.erase(key);
                }));
        }
        q.push_back(submenu("Customize", std::move(customize)));

        // Jump to the full-screen settings.
        q.push_back(leaf("Full Settings", [menu_sys_pp]{
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->open_deep();
        }));

        *quick_out = std::move(q);
    }

    return {
        with_desc(submenu("Vision",       std::move(cameras_menu)),
                  "Camera feeds and vision tools: resolution, digital zoom/crop, focus, "
                  "night vision and the vision-assist overlays."),
        with_desc(submenu("HUD",          std::move(hud_menu)),
                  "Heads-up display: colors/theme, compass, clock, picture-in-picture "
                  "camera windows, and on-screen effects."),
        with_desc(submenu("Face Display", std::move(face_display_menu)),
                  "The LED Protoface: expression, color, material, particle effects, "
                  "animations and brightness."),
        with_desc(submenu("Files",        std::vector<MenuItem>{
                      with_panel(submenu("GIFs",        std::move(gif_files_menu)),
                                 "GIF Preview",        draw_gif_preview),
                      with_panel(submenu("Backgrounds", std::move(bg_files_menu)),
                                 "Background Preview", draw_bg_preview),
                  }),
                  "Import media into Protoface from disk: GIFs and "
                  "landing-page backgrounds. Face / mouth / boop PNGs live "
                  "under Face Display > Protoface > Faces — they're tied to "
                  "the active face backend (MAX7219 / RGB matrix)."),
        with_desc(submenu("Communications", std::move(communications_menu)),
                  "Radio + phone: LoRa team mesh, KDE Connect phone, and the "
                  "notification log."),
        with_desc(submenu("System",       std::move(system_menu)),
                  "Display, audio, connectivity, Pi settings, timers, "
                  "diagnostics, profiles & power."),
    };
}
