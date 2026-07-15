// ── build_vision.cpp ──────────────────────────────────────────────────────────
// The Vision tab, moved verbatim out of build_menu(): CSI camera controls,
// resolution/zoom/crop, single-camera + multi-cam layouts, USB camera PiPs,
// Android mirror placement, and the Vision Assist post-processing cues.

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
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

std::vector<MenuItem> build_vision_menu(MenuBuildContext& ctx)
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
                    // "Both eyes" shortcut — keep both per-eye states in sync.
                    state.camera_resolution       = { w, h, fps };
                    state.camera_resolution_right = { w, h, fps };
                }
            },
            [&state, w = p.w, h = p.h]{ return state.camera_resolution.width == w && state.camera_resolution.height == h; }
        ));
    }

    std::vector<MenuItem> nv_menu = {
        with_desc(menu_shared::night_vision_toggle(state, "Enable Low-Light"),
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
        std::function<DmaCamera*()>            cam_ptr,
        CameraFocusState&                      focus_st,
        bool&                                  awb_flag,
        CameraResolutionState&                 res_st,
        std::function<bool(int,int,int)>       apply_res,  // re-init at new w/h/fps
        EyeSource*                             eye_src,    // this eye's background source
        ZoomCropState*                         zoom_st,    // this eye's digital zoom
        int                                    eye_num)    // 1 = left, 2 = right
    {
        // Helpers for the extended controls: a radio-list submenu bound to a
        // DmaCamera int setter/getter, and a slider bound to a float one. Menu
        // values match libcamera's enum numbers so they pass straight through.
        auto enum_sub = [cam_ptr](std::vector<std::pair<std::string,int>> opts,
                                  std::function<void(DmaCamera*,int)> set,
                                  std::function<int(DmaCamera*)>      get) {
            std::vector<MenuItem> v;
            for (auto& o : opts) {
                int val = o.second;
                v.push_back(leaf_sel(o.first,
                    [cam_ptr, set, val]{ if (auto* c = cam_ptr()) set(c, val); },
                    [cam_ptr, get, val]{ auto* c = cam_ptr(); return c && get(c) == val; }));
            }
            return v;
        };
        auto fslider = [cam_ptr](std::string lbl, float mn, float mx, float st,
                                 std::string unit,
                                 std::function<void(DmaCamera*,float)> set,
                                 std::function<float(DmaCamera*)>      get) {
            return slider(std::move(lbl), mn, mx, st, std::move(unit),
                [cam_ptr, get]{ auto* c = cam_ptr(); return c ? get(c) : 0.f; },
                [cam_ptr, set](float v){ if (auto* c = cam_ptr()) set(c, v); });
        };

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

        // AF range/speed live alongside the focus mode (PDAF sensors only).
        fm.push_back(submenu("AF Range", enum_sub(
            {{"Normal", 0}, {"Macro", 1}, {"Full", 2}},
            [](DmaCamera* c, int v){ c->set_af_range(v); },
            [](DmaCamera* c){ return c->af_range(); })));
        fm.push_back(submenu("AF Speed", enum_sub(
            {{"Normal", 0}, {"Fast", 1}},
            [](DmaCamera* c, int v){ c->set_af_speed(v); },
            [](DmaCamera* c){ return c->af_speed(); })));

        std::vector<MenuItem> wbm;
        wbm.push_back(toggle("Auto WB",
            [&awb_flag]{ return awb_flag; },
            [cam_ptr, &awb_flag](bool v){
                awb_flag = v;
                if (auto* c = cam_ptr()) c->set_awb_enable(v);
            }));
        // Preset illuminant modes (ISP picks the gains) — alt to the manual
        // Kelvin presets below.
        wbm.push_back(submenu("AWB Mode", enum_sub(
            {{"Auto", 0}, {"Daylight", 5}, {"Cloudy", 6}, {"Incandescent", 1},
             {"Tungsten", 2}, {"Fluorescent", 3}, {"Indoor", 4}},
            [](DmaCamera* c, int v){ c->set_awb_mode(v); },
            [](DmaCamera* c){ return c->awb_mode(); })));
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

        // Resolution — built LIVE from this sensor's own reported modes, so any
        // CSI camera shows valid options instead of a fixed preset list (the
        // cause of "pick 1440p, nothing changes" on sensors that lack it).
        // Selecting a mode requests the current target fps; libcamera clamps to
        // what the sensor can do at that size, and the Current row shows the
        // real measured fps. Per-camera, so each eye is set independently.
        MenuItem res_status;
        res_status.type     = MenuItemType::LEAF;
        res_status.label    = "Current";
        res_status.label_fn = [cam_ptr]{
            auto* c = cam_ptr();
            if (!c) return std::string("Current: (camera offline)");
            char b[64];
            snprintf(b, sizeof(b), "Current: %d x %d  @ %.0f fps",
                     c->width(), c->height(), c->measured_fps());
            return std::string(b);
        };
        res_status.action = []{};   // informational row

        auto res_rows = make_dynamic_rows(24,
            [cam_ptr]{ auto* c = cam_ptr();
                       return c ? static_cast<int>(c->supported_modes().size()) : 0; },
            [cam_ptr, apply_res, &res_st, &state](int i) {
                MenuItem m;
                m.type     = MenuItemType::LEAF;
                m.label    = "mode";
                m.label_fn = [cam_ptr, i]{
                    auto* c = cam_ptr(); if (!c) return std::string();
                    const auto& ms = c->supported_modes();
                    if (i >= static_cast<int>(ms.size())) return std::string();
                    // Mark where each mode came from: the sensor's own mode
                    // list, the manufacturer table (spec), or an ISP-scaled
                    // size. See DmaCamera::kMode*.
                    const uint8_t src  = ms[i].src;
                    const bool    sens = src & DmaCamera::kModeSensor;
                    const bool    spec = src & DmaCamera::kModeSpec;
                    const char*   tag  = sens && spec ? "sensor+spec"
                                       : sens         ? "sensor"
                                       : spec         ? "spec"
                                                      : "scaled";
                    char b[72];
                    if (ms[i].max_fps > 0)
                        snprintf(b, sizeof(b), "%d x %d  @ %d fps  \xc2\xb7 %s",
                                 ms[i].width, ms[i].height, ms[i].max_fps, tag);
                    else
                        snprintf(b, sizeof(b), "%d x %d  \xc2\xb7 %s",
                                 ms[i].width, ms[i].height, tag);
                    return std::string(b);
                };
                m.description =
                    "sensor = mode the camera reported at startup \xc2\xb7 "
                    "spec = manufacturer mode table (matched via config.txt / "
                    "camera id) \xc2\xb7 scaled = ISP-resized option. fps is "
                    "probed on this Pi where possible.";
                m.get_state = [cam_ptr, i]{
                    auto* c = cam_ptr(); if (!c) return false;
                    const auto& ms = c->supported_modes();
                    return i < static_cast<int>(ms.size())
                        && c->width()  == ms[i].width
                        && c->height() == ms[i].height;
                };
                m.action = [cam_ptr, apply_res, &res_st, &state, i]{
                    auto* c = cam_ptr(); if (!c || !apply_res) return;
                    const auto& ms = c->supported_modes();
                    if (i >= static_cast<int>(ms.size())) return;
                    // Read the target BEFORE applying — apply_res re-inits the
                    // camera (clean configure; avoids the libpisp TDN crash an
                    // in-place reconfigure causes), which DESTROYS this DmaCamera.
                    // Request the mode's probed max fps so it runs at its best
                    // rate (fall back to the current target if unknown).
                    const int w = ms[i].width, h = ms[i].height;
                    const int fps = ms[i].max_fps > 0 ? ms[i].max_fps : c->fps();
                    if (apply_res(w, h, fps)) {
                        auto* nc = cam_ptr();            // re-fetch the new camera
                        std::lock_guard<std::mutex> lk(state.mtx);
                        res_st.width  = nc ? nc->width()  : w;
                        res_st.height = nc ? nc->height() : h;
                        res_st.fps    = nc ? nc->fps()    : fps;
                    }
                };
                return m;
            });

        std::vector<MenuItem> resm;
        resm.push_back(std::move(res_status));
        for (auto& r : res_rows) resm.push_back(std::move(r));

        // ── Exposure (AE tuning + manual gain) ────────────────────────────────
        std::vector<MenuItem> expm = {
            submenu("Metering", enum_sub(
                {{"Centre-Weighted", 0}, {"Spot", 1}, {"Matrix", 2}},
                [](DmaCamera* c, int v){ c->set_ae_metering(v); },
                [](DmaCamera* c){ return c->ae_metering(); })),
            submenu("Constraint", enum_sub(
                {{"Normal", 0}, {"Highlight", 1}, {"Shadows", 2}},
                [](DmaCamera* c, int v){ c->set_ae_constraint(v); },
                [](DmaCamera* c){ return c->ae_constraint(); })),
            submenu("Exposure Profile", enum_sub(
                {{"Normal", 0}, {"Short", 1}, {"Long", 2}},
                [](DmaCamera* c, int v){ c->set_ae_exposure_mode(v); },
                [](DmaCamera* c){ return c->ae_exposure_mode(); })),
            submenu("Anti-Flicker", enum_sub(
                {{"Off", 0}, {"Auto", 1}, {"50 Hz", 2}, {"60 Hz", 3}},
                [](DmaCamera* c, int v){ c->set_flicker_mode(v); },
                [](DmaCamera* c){ return c->flicker_mode(); })),
            with_desc(fslider("Manual Gain", 0.f, 16.f, 0.5f, "x",
                [](DmaCamera* c, float v){ c->set_analogue_gain(v); },
                [](DmaCamera* c){ return c->analogue_gain_target(); }),
                "Manual sensor (analogue) gain. 0 = leave on auto. Takes effect "
                "with auto-exposure off (set a manual Shutter Speed)."),
        };

        // ── Image (ISP tuning) ────────────────────────────────────────────────
        std::vector<MenuItem> imgm = {
            fslider("Brightness", -1.f, 1.f, 0.1f, "",
                [](DmaCamera* c, float v){ c->set_brightness(v); },
                [](DmaCamera* c){ return c->brightness(); }),
            fslider("Contrast", 0.f, 2.f, 0.1f, "",
                [](DmaCamera* c, float v){ c->set_contrast(v); },
                [](DmaCamera* c){ return c->contrast(); }),
            fslider("Saturation", 0.f, 2.f, 0.1f, "",
                [](DmaCamera* c, float v){ c->set_saturation(v); },
                [](DmaCamera* c){ return c->saturation(); }),
            fslider("Sharpness", 0.f, 2.f, 0.1f, "",
                [](DmaCamera* c, float v){ c->set_sharpness(v); },
                [](DmaCamera* c){ return c->sharpness(); }),
            submenu("Noise Reduction", enum_sub(
                {{"Off", 0}, {"Fast", 1}, {"High Quality", 2}, {"Minimal", 3}},
                [](DmaCamera* c, int v){ c->set_noise_reduction(v); },
                [](DmaCamera* c){ return c->noise_reduction(); })),
        };

        // ── HDR (on-sensor; IMX708 / Camera Module 3) ─────────────────────────
        auto hdrm = enum_sub(
            {{"Off", 0}, {"Single Exposure", 3}, {"Multi Exposure", 2}, {"Night", 4}},
            [](DmaCamera* c, int v){ c->set_hdr_mode(v); },
            [](DmaCamera* c){ return c->hdr_mode(); });

        // ── Image Adjustments (White Balance + Exposure + ISP image) ──────────
        std::vector<MenuItem> imageadj;
        imageadj.push_back(submenu("White Balance", std::move(wbm)));
        imageadj.push_back(submenu("Exposure",      std::move(expm)));
        for (auto& it : imgm) imageadj.push_back(std::move(it));

        // ── Zoom (this eye's digital zoom + crop centre) ──────────────────────
        struct ZLvl { const char* l; float z; };
        static const ZLvl kZoom[] = {
            {"1.0\xC3\x97 (Full)", 1.0f}, {"1.25\xC3\x97", 1.25f},
            {"1.5\xC3\x97", 1.5f}, {"2.0\xC3\x97", 2.0f}, {"3.0\xC3\x97", 3.0f} };
        std::vector<MenuItem> zlvl;
        for (const auto& zp : kZoom)
            zlvl.push_back(live(leaf_sel(zp.l,
                [zoom_st, z = zp.z]{ if (zoom_st) zoom_st->zoom = z; },
                [zoom_st, z = zp.z]{ return zoom_st && zoom_st->zoom == z; })));
        struct ZCtr { const char* l; float cx, cy; };
        static const ZCtr kCtr[] = {
            {"Center", 0.5f, 0.5f}, {"Top", 0.5f, 0.25f}, {"Bottom", 0.5f, 0.75f},
            {"Left", 0.25f, 0.5f}, {"Right", 0.75f, 0.5f} };
        std::vector<MenuItem> zctr;
        for (const auto& cp : kCtr)
            zctr.push_back(live(leaf_sel(cp.l,
                [zoom_st, cx = cp.cx, cy = cp.cy]{
                    if (zoom_st) { zoom_st->center_x = cx; zoom_st->center_y = cy; } },
                [zoom_st, cx = cp.cx, cy = cp.cy]{
                    return zoom_st && zoom_st->center_x == cx && zoom_st->center_y == cy; })));
        std::vector<MenuItem> zoomm = {
            submenu("Zoom Level",  std::move(zlvl)),
            submenu("Crop Center", std::move(zctr)),
            leaf("Reset Zoom", [zoom_st]{ if (zoom_st) *zoom_st = ZoomCropState{}; }),
        };

        // ── Source (this eye's background camera) ─────────────────────────────
        auto src_row = [eye_src](const char* lbl, EyeSource v) {
            return leaf_sel(lbl, [eye_src, v]{ if (eye_src) *eye_src = v; },
                                 [eye_src, v]{ return eye_src && *eye_src == v; });
        };
        std::vector<MenuItem> srcm = {
            src_row("CSI Camera", EyeSource::CSI),
            src_row("CSI Cam 0",  EyeSource::CSI_LEFT),
            src_row("CSI Cam 1",  EyeSource::CSI_RIGHT),
            src_row("USB Cam 1",  EyeSource::USB1),
            src_row("USB Cam 2",  EyeSource::USB2),
            src_row("USB Cam 3",  EyeSource::USB3),
        };

        return std::vector<MenuItem>{
            submenu("Focus",          std::move(fm)),
            with_desc(submenu("Resolution", std::move(resm)),
                "Set THIS camera's capture resolution from the modes the sensor "
                "actually reports. fps follows the camera's limit at that size; "
                "the Current row shows the real measured rate."),
            submenu("Rotation",          std::move(rm)),
            submenu("Image Adjustments", std::move(imageadj)),
            with_desc(submenu("HDR",  std::move(hdrm)),
                "On-sensor HDR (IMX708 / Camera Module 3). Needs a recent "
                "libcamera that exposes HdrMode at runtime; on sensors that "
                "don't, the options are a no-op."),
            submenu("Zoom",   std::move(zoomm)),
            submenu("Source", std::move(srcm)),
            with_desc(leaf("Capture Photo (Full Res)", [&state, eye_num]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.fullres_capture_req = eye_num;
                }),
                "Take a full-sensor-resolution JPEG from THIS camera. Briefly "
                "switches it to its max mode (both eyes blank a few seconds), "
                "grabs one frame, then restores the live resolution."),
        };
    };

    auto left_cam_menu  = make_cam_menu(
        [cameras]{ return cameras ? cameras->owl_left()  : nullptr; },
        state.focus_left,  state.night_vision.csi_awb_left,
        state.camera_resolution,
        [cameras](int w, int h, int fps){
            return cameras ? cameras->set_owl_left_resolution(w, h, fps) : false; },
        left_eye_src,  &state.zoom_left,  /*eye_num=*/1);
    auto right_cam_menu = make_cam_menu(
        [cameras]{ return cameras ? cameras->owl_right() : nullptr; },
        state.focus_right, state.night_vision.csi_awb_right,
        state.camera_resolution_right,
        [cameras](int w, int h, int fps){
            return cameras ? cameras->set_owl_right_resolution(w, h, fps) : false; },
        right_eye_src, &state.zoom_right, /*eye_num=*/2);

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
            [&state, zoom = z.zoom]{ menu_shared::set_both_eye_zoom(state, zoom); },
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

    // burst_extra = -1: the deep capture leaves have never touched
    // capture_burst, so a pending quick-photo burst counter is deliberately
    // left as-is here (divergence from the quick menu's burst modes).
    std::vector<MenuItem> capture_menu = {
        leaf("Left Eye",  menu_shared::capture_action(state_ptr, CaptureRequest::Left,   -1)),
        leaf("Right Eye", menu_shared::capture_action(state_ptr, CaptureRequest::Right,  -1)),
        leaf("Both Eyes", menu_shared::capture_action(state_ptr, CaptureRequest::Stereo, -1)),
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
        menu_shared::record_toggle(state),
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
            leaf_sel("CSI Cam 0",
                [src]{ *src = EyeSource::CSI_LEFT;  },
                [src]{ return *src == EyeSource::CSI_LEFT;  }),
            leaf_sel("CSI Cam 1",
                [src]{ *src = EyeSource::CSI_RIGHT; },
                [src]{ return *src == EyeSource::CSI_RIGHT; }),
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
        menu_shared::theater_toggle(state, "Enable"),
        submenu("Position", std::move(theater_pos_menu)),
    };

    // Build per-camera submenu labels from the configured model names.  When both
    // cameras share a model (e.g. default OWLsight pair) disambiguate with #1/#2
    // plus an eye hint; otherwise show "<Model> (Left/Right)".
    // The two CSI cameras are referred to as "Left Camera" / "Right Camera"
    // throughout the UI (regardless of sensor model).
    const std::string left_label  = "Left Camera";
    const std::string right_label = "Right Camera";

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

    // ── "Other Options" — everything that isn't a per-camera setting ──────────
    std::vector<MenuItem> other_options = {
        with_desc(submenu("Multi-Cam Layout", std::move(multicam_menu)),
            "Each eye stacks a top + bottom camera (full width); the two eyes use "
            "independent sources, so side-by-side shows four distinct feeds with no "
            "duplicates. Each of the four slots picks any CSI or USB camera. Set CSI "
            "rotation to 90\xc2\xb0 under Left/Right Camera > Rotation if needed."),
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
            with_desc(submenu("Single Camera View", std::move(single_cam_menu)),
                "Show ONE camera filling a region of the screen (full, or a half) instead "
                "of the stereo pair. Choose the camera and anchor; preview at right."),
            "Single Camera Preview", single_cam_preview),
        submenu("Low-Light Options",   std::move(nv_menu)),
        submenu("Multi-Cam Autofocus", std::move(af_both_menu)),
        with_panel(
            with_desc(submenu("Raw View", std::move(raw_view_menu)),
                "Pass the camera feed straight through. Toggle Enable to show it; "
                "Position places each eye. The preview at right shows it live."),
            "Raw View Preview", eye_pos_preview),
        menu_shared::swap_cameras_toggle(state),
        with_desc(menu_shared::reinit_csi_leaf(cameras, state,
                                               "Reinitialize CSI Cameras",
                                               /*live_status_label=*/true),
            "Re-enumerate and restart the CSI cameras \xE2\x80\x94 recovers an "
            "eye that came up dark/wedged at boot, without rebooting. Briefly blacks "
            "both feeds while it re-acquires."),
        submenu("Capture Photo",    std::move(capture_menu)),
        submenu("Record Video",     std::move(video_menu)),
        submenu("QR Scan",          std::move(qr_menu)),
    };

    std::vector<MenuItem> main_cameras_menu = {
        submenu(left_label,        std::move(left_cam_menu)),
        submenu(right_label,       std::move(right_cam_menu)),
        submenu("Other Options",   std::move(other_options)),
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
        toggle("Black Out Phone Screen",
            [android_mirror]{ return android_mirror->turn_screen_off(); },
            [android_mirror](bool v){ android_mirror->set_turn_screen_off(v); }),
        toggle("Maps Display (virtual)",
            [android_mirror]{ return android_mirror->new_display_enabled(); },
            [android_mirror](bool v){ android_mirror->set_new_display(v); }),
        submenu("Navigate To", [&]{
            std::vector<MenuItem> nav;
            for (const auto& d : android_mirror->destinations()) {
                const std::string q = d.query;
                nav.push_back(leaf(d.name, [android_mirror, q]{
                    std::thread([android_mirror, q]{ android_mirror->navigate_to(q); }).detach();
                }));
            }
            if (nav.empty())
                nav.push_back(leaf("(set android.destinations in config)", []{}));
            return nav;
        }()),
        toggle("Show Overlay",
            [android_overlay]{ return *android_overlay; },
            [android_overlay](bool v){ *android_overlay = v; }),
        submenu("Position", make_position_items(android_cfg)),
        make_size_slider("Size", android_cfg),
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
        menu_shared::edge_highlight_toggle(state),
        submenu("Strength",  std::move(edge_strength_menu)),
        submenu("Color",     std::move(edge_color_menu)),
        submenu("Detail",    std::move(edge_detail_menu)),
        submenu("Threshold", std::move(edge_threshold_menu)),
        submenu("Size Filter", std::move(size_filter_menu)),
    };

    std::vector<MenuItem> motion_menu = {
        menu_shared::motion_highlight_toggle(state),
        submenu("Mode",        std::move(motion_mode_menu)),
        submenu("Sensitivity", std::move(motion_sensitivity_menu)),
        submenu("Spread",      std::move(motion_spread_menu)),
        submenu("Trail",       std::move(motion_trail_menu)),
        submenu("Color",       std::move(motion_color_menu)),
    };

    std::vector<MenuItem> desat_menu = {
        menu_shared::desaturate_toggle(state, "Bg Desaturate"),
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
        menu_shared::edge_highlight_toggle(state),
        menu_shared::motion_highlight_toggle(state),
        menu_shared::desaturate_toggle(state, "Bg Desaturate"),
        submenu("Edge Highlight", std::move(edge_menu)),
        submenu("Motion Highlight", std::move(motion_menu)),
        submenu("Bg Desaturate",  std::move(desat_menu)),
    };

    cameras_menu.push_back(submenu("Android Mirror", std::move(android_menu)));
    cameras_menu.push_back(submenu("Vision Assist",  std::move(vision_menu)));

    return cameras_menu;
}
