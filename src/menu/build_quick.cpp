// ── build_quick.cpp ───────────────────────────────────────────────────────────
// The curated corner/radial quick menu, moved verbatim out of build_menu().
// build_menu() assigns the result through ctx.quick_out.

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
#include "menu/shared_items.h"

    // ── Quick (corner / radial) menu ─────────────────────────────────────────────
    // A short, curated set of mid-use actions — separate from the full settings tree
    // above. Some items are always present; an optional "catalog" of extras can be
    // pinned by the user (favorites, persisted in config) and is gated by visible_fn.
    // Submenus here become outer rings in the radial renderer.
std::vector<MenuItem> build_quick_menu(MenuBuildContext& ctx)
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

    std::vector<MenuItem> q;

    // Night vision (same item as Vision > Low-Light Mode > "Enable Low-Light").
    q.push_back(menu_shared::night_vision_toggle(state, "Night Vision"));

    // Digital zoom (both eyes together) — continuous slider here vs the deep
    // tab's preset leaves; both go through the shared setter.
    q.push_back(slider("Zoom", 1.0f, 4.0f, 0.25f, "x",
        [&state]{ return state.zoom_left.zoom; },
        [&state](float v){ menu_shared::set_both_eye_zoom(state, v); }));

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
        // burst_extra >= 0: unlike the deep Capture Photo leaves, the quick
        // modes (re)set capture_burst so each shot count is explicit.
        std::vector<MenuItem> photo = {
            leaf("Single",   menu_shared::capture_action(state_ptr, CaptureRequest::Stereo, 0)),
            leaf("Burst x3", menu_shared::capture_action(state_ptr, CaptureRequest::Stereo, 2)),
            leaf("Burst",    menu_shared::capture_action(state_ptr, CaptureRequest::Stereo, 7)),
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
    q.push_back(menu_shared::record_toggle(state));

    // Timers — show_selected_state = false: the wheel keeps plain leaves (no
    // running-preset radio dots, unlike System > Timers and Alarm).
    q.push_back(submenu("Timers",
        menu_shared::timer_preset_items(state, /*show_selected_state=*/false,
                                        "Cancel")));

    // Expand Map — open the Helldivers-style pan/zoom view. Passing
    // menu_sys_pp also closes the wheel (the deep copy stays open).
    q.push_back(menu_shared::expand_map_leaf(state_ptr, "Expand Map", menu_sys_pp));

    // ── Favorites catalog (optional, user-pinned) ────────────────────────────
    struct QuickFav { std::string key; MenuItem item; };
    std::vector<QuickFav> catalog;
    catalog.push_back({ "edge",   menu_shared::edge_highlight_toggle(state) });
    catalog.push_back({ "desat",  menu_shared::desaturate_toggle(state, "Desaturate BG") });
    catalog.push_back({ "motion", menu_shared::motion_highlight_toggle(state) });
    // Quick-only: the deep Mini-Map module has no enable toggle (apply_hud_dock
    // forces it on); this favorite flips the raw flag directly.
    catalog.push_back({ "map", toggle("Map Overlay",
        [&state]{ return state.map_overlay.enabled; },
        [&state](bool v){ state.map_overlay.enabled = v; }) });
    catalog.push_back({ "theater", menu_shared::theater_toggle(state, "Theater Mode") });
    catalog.push_back({ "swap",    menu_shared::swap_cameras_toggle(state) });
    catalog.push_back({ "fps",     menu_shared::fps_overlay_toggle(fps_overlay_active) });
    catalog.push_back({ "syspanel", menu_shared::system_panel_toggle(sys_panel_active) });
    // Action item (not a toggle): rings the paired phone via KDE Connect.
    catalog.push_back({ "ring_phone",
        leaf("Ring Phone", menu_shared::ring_phone_action(kdc_p, state)) });
    // Diagnostics — quick recovery actions (CSI reinit + face-renderer restart).
    {
        std::vector<MenuItem> diag;
        // live_status_label = false: the wheel keeps its short static label
        // (the deep Vision copy shows the live "[L:ok R:—]" suffix).
        diag.push_back(with_desc(
            menu_shared::reinit_csi_leaf(cameras, state, "Reinit CSI Cameras",
                                         /*live_status_label=*/false),
            "Re-enumerate + restart the CSI cameras to recover a dark eye."));
        if (pf_restart_renderer) {
            diag.push_back(with_desc(
                menu_shared::restart_face_renderer_leaf(pf_restart_renderer,
                                                        state_ptr, pf_backend_p),
                "Kill + relaunch the HUB75 panel pusher to recover the face feed."));
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

    return q;
}
