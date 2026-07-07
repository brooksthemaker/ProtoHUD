// ── build_hud.cpp ─────────────────────────────────────────────────────────────
// The HUD tab, moved verbatim out of build_menu(): text/indicator options,
// compass + IMU source, colors/themes, map overlay, mini-map + info-panel
// modules, location dock, clock, menu position and the legacy HUD toggles.

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
#include "sensor/bno08x.h"
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

std::vector<MenuItem> build_hud_menu(MenuBuildContext& ctx)
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

    // Built-in theme leaves come from the shared context helper (also used by
    // System > HUD/Menu Presets); apply_theme lives inside it, in build_menu().
    auto make_builtin_theme_leaves = ctx.make_builtin_theme_leaves;

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
        { "Auto (BNO086 > BNO055 > MPU9250 > Viture)", AppState::ImuSource::Auto },
        { "BNO086 (SH-2, mag-referenced, no drift)",
                                              AppState::ImuSource::Bno08x  },
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

    // ── IMU hardware group ──────────────────────────────────────────────────
    // Source picker, head-tracking recenter, axis mapping, calibration and
    // sensor restart — the head-mounted IMU devices themselves. Routed to the
    // GPIO tab's On-Board GPIO section via ctx.imu_out when build_menu wires
    // it (the chips hang off the 40-pin header's I²C pins); otherwise nests
    // here under Compass as before.
    std::vector<MenuItem> imu_menu;
    imu_menu.push_back(
        with_desc(submenu("IMU Source", std::move(imu_source_menu)),
                  "Which sensor drives the HUD compass. Auto walks "
                  "BNO086 > BNO055 > MPU9250 > Viture and picks the "
                  "highest-priority fresh source each frame; explicit choices "
                  "force their source even if stale."));

    // ── Live readout ─────────────────────────────────────────────────────────
    // Every output the IMUs publish, as live rows: per-source heading, euler
    // angles, raw accel/gyro/mag, calibration and sample rates. Reads
    // state.imu_data + the heading slots — the same feeds the debug overlay
    // uses. Values hold their last reading when a sensor goes quiet; the
    // status rows flag it.
    {
        auto ro = [](const char* name, std::function<std::string()> fn) {
            MenuItem m = leaf(name, []{});
            m.label_fn = std::move(fn);
            return m;
        };
        auto now_us = []{
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        // Matches pick_imu_heading's freshness window in main.cpp.
        auto fresh = [](const AppState::ImuSlot& s, int64_t now) {
            return s.last_us > 0 && (now - s.last_us) < 2'000'000;
        };
        Bno08x* b86 = ctx.bno08x;
        std::vector<MenuItem> imu_readout;

        imu_readout.push_back(ro("Driving", [state_ptr, now_us, fresh]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const AppState& s = *state_ptr;
            const int64_t now = now_us();
            // Mirror pick_imu_heading (main.cpp): resolve which slot drives
            // the compass this frame and show its heading. The smoothed
            // frozen value for None lives on the render thread only.
            const char* n = nullptr;
            float h = 0.f;
            switch (s.imu_source) {
            case AppState::ImuSource::Bno08x:  n = "BNO086";   h = s.imu_bno08x.heading_deg; break;
            case AppState::ImuSource::Bno055:  n = "BNO055";   h = s.imu_bno.heading_deg;    break;
            case AppState::ImuSource::Mpu9250: n = "MPU-9250"; h = s.imu_mpu.heading_deg;    break;
            case AppState::ImuSource::Viture:  n = "VITURE";   h = s.imu_viture.heading_deg; break;
            case AppState::ImuSource::None:    break;
            case AppState::ImuSource::Auto:
                if      (fresh(s.imu_bno08x, now)) { n = "auto: BNO086";   h = s.imu_bno08x.heading_deg; }
                else if (fresh(s.imu_bno,    now)) { n = "auto: BNO055";   h = s.imu_bno.heading_deg;    }
                else if (fresh(s.imu_mpu,    now)) { n = "auto: MPU-9250"; h = s.imu_mpu.heading_deg;    }
                else if (fresh(s.imu_viture, now)) { n = "auto: VITURE";   h = s.imu_viture.heading_deg; }
                break;
            }
            char b[64];
            if (!n)
                snprintf(b, sizeof b, "Driving: %s",
                         s.imu_source == AppState::ImuSource::None
                             ? "none (heading held)" : "auto: none fresh");
            else
                snprintf(b, sizeof b, "Driving: %s  %.1f\xc2\xb0", n,
                         static_cast<double>(h));
            return std::string(b);
        }));

        // The SH-2 open can't tell a silent address from a live chip (binding
        // an I2C address always succeeds), so "driver up + no samples" is the
        // signature of a wrong i2c_addr or missing INT — say so, with the
        // address the driver is actually talking to.
        const int b86_cfg_addr =
            (ctx.cfg_root && ctx.cfg_root->contains("bno086") &&
             (*ctx.cfg_root)["bno086"].is_object())
                ? (*ctx.cfg_root)["bno086"].value("i2c_addr", 0x4A) : 0x4A;
        imu_readout.push_back(ro("BNO086", [state_ptr, b86, b86_cfg_addr, now_us, fresh]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[80];
            if (!d.b86_ok)
                snprintf(b, sizeof b,
                         b86 && b86->connected()
                             ? "BNO086:  no samples at 0x%02X - addr/INT?"
                             : "BNO086:  driver off (cfg 0x%02X)",
                         b86_cfg_addr);
            else
                snprintf(b, sizeof b, "BNO086:  %.1f\xc2\xb0  \xc2\xb1%.1f\xc2\xb0  %.0f Hz%s",
                         static_cast<double>(state_ptr->imu_bno08x.heading_deg),
                         static_cast<double>(d.b86_accuracy_deg),
                         static_cast<double>(d.b86_rate_hz),
                         fresh(state_ptr->imu_bno08x, now_us()) ? "" : "  STALE");
            return std::string(b);
        }));
        imu_readout.push_back(ro("B86 RPY", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  RPY %7.1f %7.1f %7.1f",
                     static_cast<double>(d.b86_euler[0]),
                     static_cast<double>(d.b86_euler[1]),
                     static_cast<double>(d.b86_euler[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B86 Acc", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Acc %6.2f %6.2f %6.2f g",
                     static_cast<double>(d.b86_accel_g[0]),
                     static_cast<double>(d.b86_accel_g[1]),
                     static_cast<double>(d.b86_accel_g[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B86 Gyr", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Gyr %6.0f %6.0f %6.0f d/s",
                     static_cast<double>(d.b86_gyro_dps[0]),
                     static_cast<double>(d.b86_gyro_dps[1]),
                     static_cast<double>(d.b86_gyro_dps[2]));
            return std::string(b);
        }));
        // Mag row carries the SH-2 magnetometer calibration quality (s0..s3):
        // the fused heading only earns trust from s2 up, and a vector that
        // jumps near the panels/fans is magnetic interference the quaternion
        // hides. Amber below s2.
        {
            MenuItem m = ro("B86 Mag", [state_ptr]{
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                const auto& d = state_ptr->imu_data;
                char b[64];
                snprintf(b, sizeof b, "  Mag %6.0f %6.0f %6.0f uT  s%d",
                         static_cast<double>(d.b86_mag_ut[0]),
                         static_cast<double>(d.b86_mag_ut[1]),
                         static_cast<double>(d.b86_mag_ut[2]),
                         d.b86_mag_acc);
                return std::string(b);
            });
            m.warn_fn = [state_ptr]{
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                const auto& d = state_ptr->imu_data;
                return d.b86_aux_ok && d.b86_mag_acc < 2;
            };
            imu_readout.push_back(std::move(m));
        }

        imu_readout.push_back(ro("BNO055", [state_ptr, now_us, fresh]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[80];
            if (!d.bno_ok)
                snprintf(b, sizeof b, "BNO055:  no data");
            else
                snprintf(b, sizeof b, "BNO055:  %.1f\xc2\xb0  cal %d/3%s",
                         static_cast<double>(state_ptr->imu_bno.heading_deg),
                         d.bno_calib_sys,
                         fresh(state_ptr->imu_bno, now_us()) ? "" : "  STALE");
            return std::string(b);
        }));
        imu_readout.push_back(ro("B55 HRP", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  HRP %7.1f %7.1f %7.1f",
                     static_cast<double>(d.bno_euler[0]),
                     static_cast<double>(d.bno_euler[1]),
                     static_cast<double>(d.bno_euler[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B55 Acc", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Acc %6.2f %6.2f %6.2f g",
                     static_cast<double>(d.bno_accel_g[0]),
                     static_cast<double>(d.bno_accel_g[1]),
                     static_cast<double>(d.bno_accel_g[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B55 Gyr", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Gyr %6.0f %6.0f %6.0f d/s",
                     static_cast<double>(d.bno_gyro_dps[0]),
                     static_cast<double>(d.bno_gyro_dps[1]),
                     static_cast<double>(d.bno_gyro_dps[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B55 Mag", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Mag %6.0f %6.0f %6.0f uT",
                     static_cast<double>(d.bno_mag_ut[0]),
                     static_cast<double>(d.bno_mag_ut[1]),
                     static_cast<double>(d.bno_mag_ut[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("B55 Cal", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Cal sys%d gyr%d acc%d mag%d",
                     d.bno_calib_sys, d.bno_calib_gyro,
                     d.bno_calib_accel, d.bno_calib_mag);
            return std::string(b);
        }));

        imu_readout.push_back(ro("MPU-9250", [state_ptr, now_us, fresh]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[80];
            if (!d.mpu_ok)
                snprintf(b, sizeof b, "MPU-9250:  no data");
            else
                snprintf(b, sizeof b, "MPU-9250:  %.1f\xc2\xb0  %.0f Hz  %.1f C%s",
                         static_cast<double>(state_ptr->imu_mpu.heading_deg),
                         static_cast<double>(d.mpu_rate_hz),
                         static_cast<double>(d.temp_c),
                         fresh(state_ptr->imu_mpu, now_us()) ? "" : "  STALE");
            return std::string(b);
        }));
        imu_readout.push_back(ro("MPU Acc", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Acc %6.2f %6.2f %6.2f g",
                     static_cast<double>(d.accel_g[0]),
                     static_cast<double>(d.accel_g[1]),
                     static_cast<double>(d.accel_g[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("MPU Gyr", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Gyr %6.0f %6.0f %6.0f d/s",
                     static_cast<double>(d.gyro_dps[0]),
                     static_cast<double>(d.gyro_dps[1]),
                     static_cast<double>(d.gyro_dps[2]));
            return std::string(b);
        }));
        imu_readout.push_back(ro("MPU Mag", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  Mag %6.0f %6.0f %6.0f uT",
                     static_cast<double>(d.mag_ut[0]),
                     static_cast<double>(d.mag_ut[1]),
                     static_cast<double>(d.mag_ut[2]));
            return std::string(b);
        }));

        imu_readout.push_back(ro("VITURE", [state_ptr, now_us, fresh]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[80];
            if (!d.xr_active)
                snprintf(b, sizeof b, "VITURE:  no data");
            else
                snprintf(b, sizeof b, "VITURE:  %.1f\xc2\xb0  %.0f Hz%s",
                         static_cast<double>(state_ptr->imu_viture.heading_deg),
                         static_cast<double>(d.xr_rate_hz),
                         fresh(state_ptr->imu_viture, now_us()) ? "" : "  STALE");
            return std::string(b);
        }));
        imu_readout.push_back(ro("XR RPY", [state_ptr]{
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            const auto& d = state_ptr->imu_data;
            char b[64];
            snprintf(b, sizeof b, "  RPY %7.1f %7.1f %7.1f",
                     static_cast<double>(d.xr_roll),
                     static_cast<double>(d.xr_pitch),
                     static_cast<double>(d.xr_yaw));
            return std::string(b);
        }));

        imu_menu.push_back(with_desc(submenu("Live Readout", std::move(imu_readout)),
            "Every value the IMUs publish, live: the heading each source "
            "reports, euler angles, raw accel/gyro/mag, calibration state, "
            "die temperature and sample rates. STALE = no update for 2 s "
            "(the freshness window the Auto source picker uses)."));
    }

    // ── I2C scan ─────────────────────────────────────────────────────────────
    // Probe the buses for the IMU chips at their known addresses, off the
    // render thread (same probe the System > Diagnostics full-bus scan uses).
    // Result rows appear under the scan action once it has run.
    {
        struct ImuScan {
            std::mutex m;
            bool busy = false, ran = false;
            int  found[3]    = {-1, -1, -1};   // detected addr per chip, -1 = none
            int  cfg_addr[3] = {-1, -1, -1};   // addr the config points the driver at
        };
        struct ImuChip { const char* name; const char* cfg_key; int addr[2]; int def_addr; };
        static constexpr ImuChip kChips[3] = {
            { "BNO086",   "bno086",  {0x4A, 0x4B}, 0x4A },
            { "BNO055",   "bno055",  {0x28, 0x29}, 0x28 },
            { "MPU-9250", "mpu9250", {0x68, 0x69}, 0x68 },
        };
        auto scan = std::make_shared<ImuScan>();
        json* cfg_root = ctx.cfg_root;
        MenuItem sc = leaf("Scan I2C for IMUs", [scan, cfg_root]{
            {
                std::lock_guard<std::mutex> lk(scan->m);
                if (scan->busy) return;
                scan->busy = true;
            }
            // Each chip is probed on its configured bus (default /dev/i2c-1);
            // also note the address the config points the driver at, so the
            // result row can flag a wired-vs-configured mismatch.
            std::array<std::string, 3> buses;
            std::array<int, 3> cfg_addr;
            for (int i = 0; i < 3; ++i) {
                buses[i]    = "/dev/i2c-1";
                cfg_addr[i] = kChips[i].def_addr;
                if (cfg_root && cfg_root->contains(kChips[i].cfg_key) &&
                    (*cfg_root)[kChips[i].cfg_key].is_object()) {
                    const auto& jc = (*cfg_root)[kChips[i].cfg_key];
                    buses[i]    = jc.value("i2c_bus", std::string("/dev/i2c-1"));
                    cfg_addr[i] = jc.value(i == 2 ? "mpu_addr" : "i2c_addr",
                                           kChips[i].def_addr);
                }
            }
            std::thread([scan, buses, cfg_addr]{
                int found[3] = {-1, -1, -1};
                for (int i = 0; i < 3; ++i) {
                    int fd = open(buses[i].c_str(), O_RDWR);
                    if (fd < 0) continue;
                    for (int a : kChips[i].addr)
                        if (menu_shared::i2c_probe_addr(fd, a)) { found[i] = a; break; }
                    close(fd);
                }
                std::lock_guard<std::mutex> lk(scan->m);
                for (int i = 0; i < 3; ++i) {
                    scan->found[i]    = found[i];
                    scan->cfg_addr[i] = cfg_addr[i];
                }
                scan->busy = false;
                scan->ran  = true;
            }).detach();
        });
        sc.label_fn = [scan]{
            std::lock_guard<std::mutex> lk(scan->m);
            return std::string(scan->busy ? "Scanning..."
                                          : "Scan I\xc2\xb2""C for IMUs");
        };
        imu_menu.push_back(with_desc(std::move(sc),
            "Probe the I\xc2\xb2""C bus for the IMU chips at their known "
            "addresses (BNO086 0x4A/0x4B, BNO055 0x28/0x29, MPU-9250 "
            "0x68/0x69), each on its configured bus. Answers \"is it wired "
            "and powered?\" separately from whether its driver is running. "
            "A BNO055 on the UART transport won't show here."));
        for (int i = 0; i < 3; ++i) {
            MenuItem r = leaf(kChips[i].name, []{});
            r.label_fn = [scan, i]{
                std::lock_guard<std::mutex> lk(scan->m);
                char b[64];
                if (scan->busy)
                    snprintf(b, sizeof b, "  %s:  scanning...", kChips[i].name);
                else if (scan->found[i] < 0)
                    snprintf(b, sizeof b, "  %s:  not found", kChips[i].name);
                else if (scan->found[i] != scan->cfg_addr[i])
                    // Wired at one address, driver configured for another —
                    // the "connected but silent" trap. Point at the fix.
                    snprintf(b, sizeof b, "  %s:  0x%02X but config says 0x%02X!",
                             kChips[i].name, scan->found[i], scan->cfg_addr[i]);
                else
                    snprintf(b, sizeof b, "  %s:  found at 0x%02X",
                             kChips[i].name, scan->found[i]);
                return std::string(b);
            };
            r.warn_fn = [scan, i]{
                std::lock_guard<std::mutex> lk(scan->m);
                return scan->ran && scan->found[i] >= 0 &&
                       scan->found[i] != scan->cfg_addr[i];
            };
            r.visible_fn = [scan]{
                std::lock_guard<std::mutex> lk(scan->m);
                return scan->ran || scan->busy;
            };
            imu_menu.push_back(std::move(r));
        }
    }
    imu_menu.push_back([&]() -> MenuItem {
        // BNO086 head-tracking recenter: tare the sensor so the direction
        // you're facing becomes "forward" for pin-in-space / the compass.
        Bno08x* b = ctx.bno08x;
        MenuItem m = with_desc(
            leaf("Recenter Head Tracking", [b]{ if (b) b->recenter(false); }),
            "Tare the BNO086 so your current facing becomes forward "
            "(zeroes heading/yaw). Use after mounting or if the view has "
            "drifted off-centre.");
        m.visible_fn = [b]{ return b && b->connected(); };
        return m;
    }());
    imu_menu.push_back([&]() -> MenuItem {
        // Mounting calibration: X|Y tare, persisted on the chip. Kills the
        // standing roll a rotated mount feeds every absolute-roll consumer
        // (Motion Reactive lean, Face Inertia rest offset, readout RPY).
        Bno08x* b = ctx.bno08x;
        MenuItem m = with_desc(
            leaf("Set Level (Tare Roll/Pitch)", [b]{ if (b) b->level(); }),
            "Hold the helmet level and look straight ahead, then select. "
            "Tares the BNO086's roll/pitch so its mounting orientation reads "
            "as zero - fixes face effects leaning sideways and Face Inertia "
            "resting off-centre. Heading/north is untouched. Persisted on "
            "the chip across power cycles.");
        m.visible_fn = [b]{ return b && b->connected(); };
        return m;
    }());
    // Manual trim: fine roll/pitch correction on top of Set Level, live and
    // persisted to cfg["bno086"]. Small additive offsets — big mounting
    // angles belong to Set Level's quaternion reorientation.
    {
        Bno08x* b   = ctx.bno08x;
        json* cfgr  = ctx.cfg_root;
        auto  trimv = std::make_shared<std::array<float, 2>>();
        (*trimv) = {0.f, 0.f};
        if (cfgr && cfgr->contains("bno086") && (*cfgr)["bno086"].is_object()) {
            (*trimv)[0] = (*cfgr)["bno086"].value("roll_trim",  0.0f);
            (*trimv)[1] = (*cfgr)["bno086"].value("pitch_trim", 0.0f);
        }
        auto apply = [b, cfgr, trimv]{
            if (b) b->set_trim((*trimv)[0], (*trimv)[1]);
            if (cfgr) {
                (*cfgr)["bno086"]["roll_trim"]  = (*trimv)[0];
                (*cfgr)["bno086"]["pitch_trim"] = (*trimv)[1];
            }
        };
        MenuItem rt = with_desc(slider("Roll Trim", -15.f, 15.f, 0.5f, "\xc2\xb0",
            [trimv]{ return (*trimv)[0]; },
            [trimv, apply](float v){ (*trimv)[0] = v; apply(); }),
            "Manual fine correction added to the roll output. Nudge until the "
            "Live Readout RPY 'R' reads 0 with your head level (or effects "
            "fall straight). Applies live; saved to cfg[\"bno086\"].");
        rt.visible_fn = [b]{ return b && b->connected(); };
        imu_menu.push_back(std::move(rt));
        MenuItem pt = with_desc(slider("Pitch Trim", -15.f, 15.f, 0.5f, "\xc2\xb0",
            [trimv]{ return (*trimv)[1]; },
            [trimv, apply](float v){ (*trimv)[1] = v; apply(); }),
            "Manual fine correction added to the pitch output. Nudge until "
            "the Live Readout RPY 'P' reads 0 with your head level. Applies "
            "live; saved to cfg[\"bno086\"].");
        pt.visible_fn = [b]{ return b && b->connected(); };
        imu_menu.push_back(std::move(pt));
    }
    // Guided range calibration: look up/down/left/right, back to centre.
    if (ctx.imu_cal_start) {
        MenuItem m = leaf("Calibrate Motion Range",
                          [fn = ctx.imu_cal_start]{ fn(); });
        m.label_fn = [st = ctx.imu_cal_status]{
            const std::string s = st ? st() : std::string();
            return s.empty() ? std::string("Calibrate Motion Range")
                             : "Calibrating: " + s;
        };
        imu_menu.push_back(with_desc(std::move(m),
            "Guided setup: hold your head level, then look up, down, left, "
            "right, and back to centre - each step advances when you hold "
            "still. Measures your comfortable head range and normalises the "
            "face-motion response to it (slight tilts stop reading "
            "exaggerated), and re-levels the mount at the final step. Select "
            "again mid-run to cancel."));
    }
    imu_menu.push_back(submenu("IMU Axis", std::move(imu_axis_menu)));
    imu_menu.push_back([&]() -> MenuItem {
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
    }());
    imu_menu.push_back([&]() -> MenuItem {
        // Re-init the BNO055 without restarting ProtoHUD — for a sensor
        // that wasn't powered/ready at boot (the chip needs ≥1 s after
        // power-on before it talks). restart() stops + joins the poll
        // thread and sleeps through the settle window, so it must NOT run
        // on the render thread — hand it to a detached worker.
        MenuItem m = leaf("Restart IMU Sensor", [bno055, state_ptr]{
            if (!bno055) return;
            std::thread([bno055, state_ptr]{
                const bool ok = bno055->restart();
                if (!state_ptr) return;
                Notification n; n.type = NotifType::App;
                n.title = ok ? "IMU sensor connected" : "IMU sensor not found";
                n.body  = ok ? "BNO055 re-initialised and streaming."
                             : "No response — check wiring/power, then retry.";
                n.auto_dismiss_s = 5.f;
                std::lock_guard<std::mutex> lk(state_ptr->mtx);
                state_ptr->notifs.push(std::move(n));
            }).detach();
        });
        m.label_fn = [bno055]{
            return std::string("Restart IMU Sensor  [") +
                   (bno055 && bno055->connected() ? "connected" : "offline") + "]";
        };
        return with_desc(std::move(m),
            "Tear down and re-initialise the BNO055. Use when the sensor "
            "wasn't ready at boot — the chip needs about a second after "
            "power-on before it responds, so a sensor powered late comes "
            "up offline until you trigger this. No ProtoHUD restart needed.");
    }());

    std::vector<MenuItem> compass_menu = {
        toggle("Compass Tape",
            [&state]{ return state.compass_tape; },
            [&state](bool v){ state.compass_tape = v; }),
        submenu("Onboard Compass",     std::move(onboard_compass_menu)),
        slider("Tick Length", 8.f, 48.f, 2.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_tick_length); },
            [hud_cfg](float v){ hud_cfg->compass_tick_length = static_cast<int>(v); }),
        submenu("Tagged Radio Colors", std::move(tagged_radio_colors_menu)),
        slider("Tape Height", 50.f, 120.f, 5.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_height); },
            [hud_cfg](float v){ hud_cfg->compass_height = static_cast<int>(v); }),
    };
    if (ctx.imu_out) {
        ctx.imu_out->push_back(with_desc(submenu("IMU", std::move(imu_menu)),
            "Head-tracking IMU hardware on the 40-pin header's I\xc2\xb2""C bus: "
            "pick which sensor drives the compass (BNO086 / BNO055 / MPU-9250 / "
            "Viture), recenter head tracking, remap axes, save calibration or "
            "restart the sensor."));
    } else {
        // No GPIO tab wired — keep the IMU items under Compass, after the
        // Compass Tape toggle, as before.
        compass_menu.insert(compass_menu.begin() + 1,
                            std::make_move_iterator(imu_menu.begin()),
                            std::make_move_iterator(imu_menu.end()));
    }

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
        // close_menu = nullptr: unlike the quick wheel's Expand Map, the deep
        // menu stays open behind the expanded view.
        menu_shared::expand_map_leaf(state_ptr, "Expand Map (Pan/Zoom)", nullptr),
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

    return hud_menu;
}
