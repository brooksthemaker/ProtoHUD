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

    // ── Shared helpers for the tab builders ──────────────────────────────────
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
    ctx.push_notif = push_notif;

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
    ctx.make_builtin_theme_leaves = make_builtin_theme_leaves;


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
    // Publish the GIF machinery for the Face Display + Files tab builders.
    // gif_leaf / gif_slot_row are build-phase only (capture locals by ref).
    ctx.draw_gif_preview = draw_gif_preview;
    ctx.gif_leaf         = gif_leaf;
    ctx.gif_slot_row     = gif_slot_row;


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

    // ── Vision tab ───────────────────────────────────────────────────────────
    // Built in src/menu/build_vision.cpp.
    std::vector<MenuItem> cameras_menu = build_vision_menu(ctx);

    // ── Face Display tab ─────────────────────────────────────────────────────
    // Built in src/menu/build_face_display.cpp (uses ctx.gif_leaf and
    // ctx.draw_gif_preview for its Animations / GIFs submenus).
    std::vector<MenuItem> face_display_menu = build_face_display_menu(ctx);

    // ── HUD tab ──────────────────────────────────────────────────────────────
    // Built in src/menu/build_hud.cpp (uses ctx.make_builtin_theme_leaves).
    std::vector<MenuItem> hud_menu = build_hud_menu(ctx);

    // (System tab sections moved to src/menu/build_system.cpp.)

    // ── Communications tab ───────────────────────────────────────────────────
    // Built in src/menu/build_communications.cpp (uses ctx.push_notif; hands
    // the Phone item to the System builder through ctx.phone_item).
    std::vector<MenuItem> communications_menu = build_communications_menu(ctx);

    // ── System tab ───────────────────────────────────────────────────────────
    // Built in src/menu/build_system.cpp (consumes ctx.phone_item,
    // ctx.push_notif and ctx.make_builtin_theme_leaves).
    std::vector<MenuItem> system_menu = build_system_menu(ctx);

    // ── Quick (corner / radial) menu ─────────────────────────────────────────────
    // Built in src/menu/build_quick.cpp; assigned through quick_out when set.
    if (quick_out) *quick_out = build_quick_menu(ctx);

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
