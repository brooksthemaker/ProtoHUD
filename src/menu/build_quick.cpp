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

    return q;
}
