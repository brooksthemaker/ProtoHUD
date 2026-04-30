#include <iostream>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>

#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include "app_state.h"
#include "android/android_mirror.h"
#include "camera/camera_manager.h"
#include "camera/viture_camera.h"
#include "input/gpio_buttons.h"
#include "serial/teensy_controller.h"
#include "serial/lora_radio.h"
#include "serial/smartknob.h"
#include "hud/hud_renderer.h"
#include "menu/menu_system.h"
#include "vitrue/xr_display.h"
#include "vitrue/timewarp.h"
#include "audio/audio_engine.h"
#include "post_process.h"
#include "sensor/mpu9250.h"

using json = nlohmann::json;

// ── Config loading ────────────────────────────────────────────────────────────

static json load_config(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        std::cerr << "[cfg] cannot open " << path << " — using defaults\n";
        return json::object();
    }
    json j = json::parse(f, nullptr, false);
    fclose(f);
    if (j.is_discarded()) {
        std::cerr << "[cfg] parse error in " << path << " — using defaults\n";
        return json::object();
    }
    return j;
}

template<typename T>
T jval(const json& j, const std::string& key, T def) {
    try { return j.value(key, def); }
    catch (...) { return def; }
}

// ── Overlay anchor parsing ────────────────────────────────────────────────────

static OverlayConfig::Anchor parse_anchor(const std::string& s) {
    if (s == "top_left")      return OverlayConfig::Anchor::TOP_LEFT;
    if (s == "top_right")     return OverlayConfig::Anchor::TOP_RIGHT;
    if (s == "bottom_left")   return OverlayConfig::Anchor::BOTTOM_LEFT;
    if (s == "bottom_center") return OverlayConfig::Anchor::BOTTOM_CENTER;
    if (s == "bottom_right")  return OverlayConfig::Anchor::BOTTOM_RIGHT;
    return OverlayConfig::Anchor::TOP_CENTER;
}

// ── Color serialization helpers ───────────────────────────────────────────────
// ImU32 is ABGR: alpha in high byte, red in low byte.

static ImU32 jcolor(const json& j, const std::string& key, ImU32 def) {
    if (!j.contains(key)) return def;
    try {
        const auto& a = j[key];
        if (a.is_array() && a.size() >= 3) {
            int r = a[0].get<int>(), g = a[1].get<int>(), b = a[2].get<int>();
            int al = a.size() >= 4 ? a[3].get<int>() : 255;
            return IM_COL32(r, g, b, al);
        }
    } catch (...) {}
    return def;
}

static json color_to_json(ImU32 col) {
    return json::array({
        int((col >>  0) & 0xFF),   // R
        int((col >>  8) & 0xFF),   // G
        int((col >> 16) & 0xFF),   // B
        int((col >> 24) & 0xFF),   // A
    });
}

// ── GLFW key edge detection ───────────────────────────────────────────────────
// Returns true on the frame the key transitions from released to pressed.
// ImGui intercepts GLFW callbacks; after begin_frame() we use ImGui's key state.

static bool key_pressed(ImGuiKey key) {
    return ImGui::IsKeyPressed(key, /*repeat=*/false);
}

// ── Menu definition ───────────────────────────────────────────────────────────

static std::vector<MenuItem> build_menu(
        TeensyController* teensy, XRDisplay* xr, CameraManager* cameras,
        LoRaRadio* lora, SmartKnob* knob, AudioEngine* audio, AppState& state,
        AndroidMirror* android_mirror, bool* android_overlay,
        OverlayConfig* pip_cfg1, OverlayConfig* pip_cfg2,
        bool* pip_cam1_overlay, bool* pip_cam2_overlay,
        OverlayConfig* android_cfg,
        HudColors* hud_col, HudConfig* hud_cfg, MenuSystem** menu_sys_pp,
        Mpu9250* mpu9250)
{
    (void)lora; (void)knob;

    // ── Factory helpers ───────────────────────────────────────────────────────

    auto leaf = [](std::string lbl, std::function<void()> fn) -> MenuItem {
        MenuItem m;
        m.label  = std::move(lbl);
        m.type   = MenuItemType::LEAF;
        m.action = std::move(fn);
        return m;
    };

    auto submenu = [](std::string lbl, std::vector<MenuItem> ch) -> MenuItem {
        MenuItem m;
        m.label    = std::move(lbl);
        m.type     = MenuItemType::SUBMENU;
        m.children = std::move(ch);
        return m;
    };

    auto toggle = [](std::string lbl,
                     std::function<bool()>     get_fn,
                     std::function<void(bool)> set_fn) -> MenuItem {
        MenuItem m;
        m.label      = std::move(lbl);
        m.type       = MenuItemType::TOGGLE;
        m.get_toggle = std::move(get_fn);
        m.set_toggle = std::move(set_fn);
        return m;
    };

    auto slider = [](std::string lbl,
                     float mn, float mx, float step, std::string unit,
                     std::function<float()>     get_fn,
                     std::function<void(float)> set_fn) -> MenuItem {
        MenuItem m;
        m.label            = std::move(lbl);
        m.type             = MenuItemType::SLIDER;
        m.slider.min       = mn;
        m.slider.max       = mx;
        m.slider.step      = step;
        m.slider.unit      = std::move(unit);
        m.slider.get_value = std::move(get_fn);
        m.slider.set_value = std::move(set_fn);
        return m;
    };

    auto color_picker = [](std::string lbl,
                            std::function<void(uint8_t,uint8_t,uint8_t)> set_fn,
                            std::function<std::tuple<uint8_t,uint8_t,uint8_t>()> get_fn
                                = nullptr) -> MenuItem {
        MenuItem m;
        m.label           = std::move(lbl);
        m.type            = MenuItemType::COLOR_PICKER;
        m.color.set_color = std::move(set_fn);
        m.color.get_color = std::move(get_fn);
        return m;
    };

    // ── Face effects ──────────────────────────────────────────────────────────
    std::vector<MenuItem> effects;
    for (uint8_t id = 0; id < 10; id++) {
        const char* names[] = { "Idle","Blink","Angry","Happy","Sad",
                                 "Shocked","Rainbow","Pulse","Wave","Custom" };
        effects.push_back(leaf(names[id], [teensy, id]{ teensy->set_effect(id); }));
    }

    // ── Face colors (presets + custom picker) ─────────────────────────────────
    std::vector<MenuItem> colors;
    colors.push_back(leaf("Teal",   [teensy]{ teensy->set_color(0,220,180);   }));
    colors.push_back(leaf("Cyan",   [teensy]{ teensy->set_color(0,180,255);   }));
    colors.push_back(leaf("Red",    [teensy]{ teensy->set_color(220,30,30);   }));
    colors.push_back(leaf("Green",  [teensy]{ teensy->set_color(30,220,60);   }));
    colors.push_back(leaf("Purple", [teensy]{ teensy->set_color(180,30,220);  }));
    colors.push_back(leaf("White",  [teensy]{ teensy->set_color(255,255,255); }));
    colors.push_back(color_picker(
        "Custom Color",
        [teensy](uint8_t r, uint8_t g, uint8_t b){ teensy->set_color(r, g, b); },
        [&state]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
            return { state.face.r, state.face.g, state.face.b };
        }
    ));

    // ── GIFs ─────────────────────────────────────────────────────────────────
    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "GIF #%d", i);
        gifs.push_back(leaf(lbl, [teensy, i]{ teensy->play_gif(i); }));
    }

    // ── Camera controls ───────────────────────────────────────────────────────
    std::vector<MenuItem> focus_modes = {
        leaf("Manual", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::MANUAL;
            state.focus_right.mode = CameraFocusState::Mode::MANUAL;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->stop_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->stop_autofocus();
            }
        }),
        leaf("Auto", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::AUTO;
            state.focus_right.mode = CameraFocusState::Mode::AUTO;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->start_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->start_autofocus();
            }
        }),
        leaf("Slave", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::SLAVE;
            state.focus_right.mode = CameraFocusState::Mode::SLAVE;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->stop_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->stop_autofocus();
            }
        }),
    };

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
        leaf("1/4000", [&state]{ state.night_vision.shutter_us =   250; }),
        leaf("1/2000", [&state]{ state.night_vision.shutter_us =   500; }),
        leaf("1/1000", [&state]{ state.night_vision.shutter_us =  1000; }),
        leaf("1/500",  [&state]{ state.night_vision.shutter_us =  2000; }),
        leaf("1/250",  [&state]{ state.night_vision.shutter_us =  4000; }),
        leaf("1/125",  [&state]{ state.night_vision.shutter_us =  8000; }),
        leaf("1/60",   [&state]{ state.night_vision.shutter_us = 16667; }),
        leaf("1/30",   [&state]{ state.night_vision.shutter_us = 33333; }),
        leaf("1/25",   [&state]{ state.night_vision.shutter_us = 40000; }),
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
        resolution_presets.push_back(leaf(
            p.label,
            [cameras, &state, w = p.w, h = p.h, fps = p.fps](){
                if (!cameras) return;
                if (cameras->set_resolution(w, h, fps)) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.camera_resolution = { w, h, fps };
                }
            }
        ));
    }

    std::vector<MenuItem> nv_menu = {
        toggle("Night Vision",
            [&state]{ return state.night_vision.nv_enabled; },
            [&state](bool v){
                state.night_vision.nv_enabled  = v;
                state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
                state.night_vision.shutter_us  = v ? 40000 : 16667;
            }),
        slider("Exposure (EV)", -3.f, 3.f, 0.5f, " EV",
            [&state]{ return state.night_vision.exposure_ev; },
            [&state](float v){ state.night_vision.exposure_ev = v; }),
        submenu("Shutter Speed", std::move(shutter_speeds)),
    };

    std::vector<MenuItem> main_cameras_menu = {
        submenu("Resolution",  std::move(resolution_presets)),
        submenu("Focus Mode",  std::move(focus_modes)),
        slider("Focus Position", 0.f, 1000.f, 50.f, "",
            [&state]{ return static_cast<float>(state.focus_left.focus_position); },
            [cameras, &state](float v){
                int pos = static_cast<int>(v);
                if (cameras) {
                    if (cameras->owl_left())  cameras->owl_left()->set_focus_position(pos);
                    if (cameras->owl_right()) cameras->owl_right()->set_focus_position(pos);
                }
                state.focus_left.focus_position  = pos;
                state.focus_right.focus_position = pos;
            }),
        submenu("Autofocus",    std::move(af_triggers)),
        submenu("Night Vision", std::move(nv_menu)),
    };

    // ── Headset controls ──────────────────────────────────────────────────────
    std::vector<MenuItem> headset_menu = {
        slider("Dimming", 0.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_dimming); },
            [xr, &state](float v){
                state.xr_dimming = static_cast<int>(v);
                if (xr) xr->set_dimming(static_cast<int>(v));
            }),
        slider("HUD Bright", 1.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_hud_brightness); },
            [xr, &state](float v){
                state.xr_hud_brightness = static_cast<int>(v);
                if (xr) xr->set_hud_brightness(static_cast<int>(v));
            }),
        leaf("Recenter",  [xr]{ if (xr) xr->recenter_tracking(); }),
        leaf("Gaze Lock", [xr]{ if (xr) xr->toggle_gaze_lock(); }),
        leaf("3D SBS",    [xr]{ if (xr) xr->set_3d_mode(true); }),
    };

    // ── Audio controls ────────────────────────────────────────────────────────
    // Beamforming / noise suppression handled by RP2350; CM5 controls output + volume.
    std::vector<MenuItem> output_menu = {
        leaf("VITURE",     [audio]{ if (audio) audio->set_output(AudioOutput::VITURE);     }),
        leaf("Headphones", [audio]{ if (audio) audio->set_output(AudioOutput::HEADPHONES); }),
        leaf("HDMI",       [audio]{ if (audio) audio->set_output(AudioOutput::HDMI);       }),
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
    using A = OverlayConfig::Anchor;

    auto make_position_items = [&leaf](OverlayConfig* cfg) {
        return std::vector<MenuItem>{
            leaf("Top Left",      [cfg]{ cfg->anchor = A::TOP_LEFT;      }),
            leaf("Top Center",    [cfg]{ cfg->anchor = A::TOP_CENTER;    }),
            leaf("Top Right",     [cfg]{ cfg->anchor = A::TOP_RIGHT;     }),
            leaf("Bottom Left",   [cfg]{ cfg->anchor = A::BOTTOM_LEFT;   }),
            leaf("Bottom Center", [cfg]{ cfg->anchor = A::BOTTOM_CENTER; }),
            leaf("Bottom Right",  [cfg]{ cfg->anchor = A::BOTTOM_RIGHT;  }),
        };
    };

    auto make_size_slider = [&slider](std::string lbl, OverlayConfig* cfg) -> MenuItem {
        return slider(std::move(lbl), 15.f, 60.f, 5.f, " %",
            [cfg]{ return cfg->size * 100.f; },
            [cfg](float v){ cfg->size = v / 100.f; });
    };

    // ── USB camera menus ──────────────────────────────────────────────────────
    std::vector<MenuItem> cam1_overlay_menu = {
        toggle("Show Overlay",
            [pip_cam1_overlay]{ return *pip_cam1_overlay; },
            [pip_cam1_overlay](bool v){ *pip_cam1_overlay = v; }),
        submenu("Position", make_position_items(pip_cfg1)),
        make_size_slider("Size", pip_cfg1),
    };
    std::vector<MenuItem> usb_cam1_menu = {
        toggle("Open Stream",
            [cameras]{ return cameras && cameras->usb1_ok(); },
            [cameras, pip_cam1_overlay](bool v){
                if (!cameras) return;
                if (v) {
                    std::thread([cameras, pip_cam1_overlay]{
                        cameras->open_usb1();
                        *pip_cam1_overlay = true;
                    }).detach();
                } else {
                    cameras->close_usb1();
                    *pip_cam1_overlay = false;
                }
            }),
        submenu("Overlay", std::move(cam1_overlay_menu)),
        leaf("Scan for Camera", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb1();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb1 = ok;
            }
        }),
    };

    std::vector<MenuItem> cam2_overlay_menu = {
        toggle("Show Overlay",
            [pip_cam2_overlay]{ return *pip_cam2_overlay; },
            [pip_cam2_overlay](bool v){ *pip_cam2_overlay = v; }),
        submenu("Position", make_position_items(pip_cfg2)),
        make_size_slider("Size", pip_cfg2),
    };
    std::vector<MenuItem> usb_cam2_menu = {
        toggle("Open Stream",
            [cameras]{ return cameras && cameras->usb2_ok(); },
            [cameras, pip_cam2_overlay](bool v){
                if (!cameras) return;
                if (v) {
                    std::thread([cameras, pip_cam2_overlay]{
                        cameras->open_usb2();
                        *pip_cam2_overlay = true;
                    }).detach();
                } else {
                    cameras->close_usb2();
                    *pip_cam2_overlay = false;
                }
            }),
        submenu("Overlay", std::move(cam2_overlay_menu)),
        leaf("Scan for Camera", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb2();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb2 = ok;
            }
        }),
    };

    std::vector<MenuItem> usb_cameras_menu = {
        submenu("USB Cam 1",  std::move(usb_cam1_menu)),
        submenu("USB Cam 2",  std::move(usb_cam2_menu)),
        toggle("Auto-Reconnect",
            [cameras]{ return cameras &&
                              cameras->usb1_reconnect_enabled() &&
                              cameras->usb2_reconnect_enabled(); },
            [cameras](bool v){
                if (!cameras) return;
                cameras->set_usb1_reconnect(v);
                cameras->set_usb2_reconnect(v);
            }),
    };

    std::vector<MenuItem> cameras_menu = {
        submenu("Main Cameras", std::move(main_cameras_menu)),
        submenu("USB Cameras",  std::move(usb_cameras_menu)),
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

    // ── Prototracer (face controller) submenu ─────────────────────────────────
    std::vector<MenuItem> prototracer_menu = {
        submenu("Effects",  std::move(effects)),
        submenu("Color",    std::move(colors)),
        submenu("Play GIF", std::move(gifs)),
        slider("Brightness", 0.f, 255.f, 1.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
        slider("Lens Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
    };

    // ── HUD settings ──────────────────────────────────────────────────────────

    auto make_color_items = [&leaf](std::vector<std::pair<const char*, ImU32>> presets,
                                     std::function<void(ImU32)> apply) {
        std::vector<MenuItem> v;
        for (auto& [lbl, col] : presets)
            v.push_back(leaf(lbl, [apply, col]{ apply(col); }));
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
    }, [hud_col](ImU32 c){ hud_col->text_fill = c; });

    std::vector<MenuItem> glow_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Amber",  IM_COL32(255, 190,  50, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Yellow", IM_COL32(255, 240,  50, 255) },
        { "Red",    IM_COL32(255,  50,  50, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "Blue",   IM_COL32( 60, 100, 255, 255) },
        { "Pink",   IM_COL32(255,  80, 200, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->glow_base = c; });

    std::vector<MenuItem> text_options_menu = {
        submenu("Color",     std::move(text_color_menu)),
        slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
        submenu("Glow Color", std::move(glow_color_menu)),
        toggle("Glow Enable",
            [hud_cfg]{ return hud_cfg->glow_enabled; },
            [hud_cfg](bool v){ hud_cfg->glow_enabled = v; }),
    };

    // ── Indicator Options ─────────────────────────────────────────────────────
    std::vector<MenuItem> ind_good_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_good = c; });

    std::vector<MenuItem> ind_inactive_color_menu = make_color_items({
        { "Gray",   IM_COL32(120, 120, 120, 255) },
        { "Blue",   IM_COL32( 60,  80, 160, 255) },
        { "Dim",    IM_COL32( 80,  80,  80, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_inactive = c; });

    std::vector<MenuItem> ind_fail_color_menu = make_color_items({
        { "Red",    IM_COL32(255,  60,  60, 255) },
        { "Orange", IM_COL32(255, 120,   0, 255) },
        { "Yellow", IM_COL32(240, 220,   0, 255) },
    }, [hud_col](ImU32 c){ hud_col->ind_fail = c; });

    std::vector<MenuItem> indicator_options_menu = {
        submenu("Active Color",   std::move(ind_good_color_menu)),
        submenu("Inactive Color", std::move(ind_inactive_color_menu)),
        submenu("Fail Color",     std::move(ind_fail_color_menu)),
    };

    // ── Compass ───────────────────────────────────────────────────────────────
    // Onboard MPU-9250 backup compass (moved from root "Backup Compass")
    std::vector<MenuItem> onboard_compass_menu = {
        toggle("Active",
            [mpu9250]{ return mpu9250 && mpu9250->is_running(); },
            [mpu9250](bool v){
                if (!mpu9250) return;
                if (v) mpu9250->start(); else mpu9250->stop();
            }),
        toggle("Calibrate",
            [mpu9250]{ return mpu9250 && mpu9250->is_calibrating(); },
            [mpu9250](bool v){
                if (!mpu9250) return;
                if (v) mpu9250->begin_calibration();
                else   mpu9250->end_calibration();
            }),
    };

    std::vector<MenuItem> compass_bg_color_menu = make_color_items({
        { "Default", IM_COL32(  8,  12,  18, 255) },
        { "Teal",    IM_COL32(  5,  30,  25, 255) },
        { "Purple",  IM_COL32( 18,   8,  28, 255) },
        { "Blue",    IM_COL32( 10,  10,  40, 255) },
        { "Black",   IM_COL32(  0,   0,   0, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_bg_color = c; });

    std::vector<MenuItem> compass_bg_options_menu = {
        slider("Tape Height", 50.f, 120.f, 5.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_height); },
            [hud_cfg](float v){ hud_cfg->compass_height = static_cast<int>(v); }),
        submenu("Color", std::move(compass_bg_color_menu)),
    };

    std::vector<MenuItem> compass_tick_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_tick = c; });

    std::vector<MenuItem> compass_glow_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
        { "White",  IM_COL32(255, 255, 255, 255) },
    }, [hud_col](ImU32 c){ hud_col->compass_glow = c; });

    std::vector<MenuItem> compass_color_options_menu = {
        submenu("Tick Color", std::move(compass_tick_color_menu)),
        submenu("Glow Color", std::move(compass_glow_color_menu)),
    };

    std::vector<MenuItem> compass_menu = {
        submenu("Onboard Compass",   std::move(onboard_compass_menu)),
        submenu("Background Options",std::move(compass_bg_options_menu)),
        submenu("Color Options",     std::move(compass_color_options_menu)),
        slider("Tick Length", 8.f, 48.f, 2.f, "",
            [hud_cfg]{ return static_cast<float>(hud_cfg->compass_tick_length); },
            [hud_cfg](float v){ hud_cfg->compass_tick_length = static_cast<int>(v); }),
        toggle("Tick Glow",
            [hud_cfg]{ return hud_cfg->compass_tick_glow; },
            [hud_cfg](bool v){ hud_cfg->compass_tick_glow = v; }),
    };

    // ── Menu Options ──────────────────────────────────────────────────────────
    std::vector<MenuItem> menu_color_menu = make_color_items({
        { "Orange", IM_COL32(255, 160,  32, 255) },
        { "Teal",   IM_COL32(  0, 220, 180, 255) },
        { "Cyan",   IM_COL32(  0, 180, 255, 255) },
        { "Green",  IM_COL32( 30, 220,  60, 255) },
        { "Purple", IM_COL32(180,  30, 220, 255) },
    }, [menu_sys_pp](ImU32 c){ if (*menu_sys_pp) (*menu_sys_pp)->set_accent_color(c); });

    std::vector<MenuItem> menu_bg_color_menu = make_color_items({
        { "Dark",   IM_COL32( 10,  15,  20, 225) },
        { "Teal",   IM_COL32(  5,  25,  22, 225) },
        { "Purple", IM_COL32( 20,   8,  28, 225) },
        { "Navy",   IM_COL32(  8,   8,  35, 225) },
        { "Black",  IM_COL32(  0,   0,   0, 230) },
    }, [menu_sys_pp](ImU32 c){ if (*menu_sys_pp) (*menu_sys_pp)->set_bg_color(c); });

    std::vector<MenuItem> menu_options_menu = {
        submenu("Menu Color",  std::move(menu_color_menu)),
        submenu("BG Color",    std::move(menu_bg_color_menu)),
        toggle("Background",
            [menu_sys_pp]{ return *menu_sys_pp && (*menu_sys_pp)->bg_enabled(); },
            [menu_sys_pp](bool v){ if (*menu_sys_pp) (*menu_sys_pp)->set_bg_enabled(v); }),
    };

    std::vector<MenuItem> hud_menu = {
        toggle("Show Backgrounds",
            [hud_cfg, &state]{ return hud_cfg->indicator_bg_enabled && state.compass_bg_enabled; },
            [hud_cfg, &state](bool v){
                hud_cfg->indicator_bg_enabled = v;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.compass_bg_enabled = v;
            }),
        submenu("Text Options",      std::move(text_options_menu)),
        submenu("Indicator Options", std::move(indicator_options_menu)),
        submenu("Compass",           std::move(compass_menu)),
        submenu("Menu Options",      std::move(menu_options_menu)),
    };

    // ── Vision Assist (post-processing depth cues) ────────────────────────────

    std::vector<MenuItem> edge_strength_menu = {
        leaf("10%",  [&state]{ state.pp_cfg.edge_strength = 0.10f; }),
        leaf("30%",  [&state]{ state.pp_cfg.edge_strength = 0.30f; }),
        leaf("50%",  [&state]{ state.pp_cfg.edge_strength = 0.50f; }),
        leaf("70%",  [&state]{ state.pp_cfg.edge_strength = 0.70f; }),
        leaf("100%", [&state]{ state.pp_cfg.edge_strength = 1.00f; }),
    };

    std::vector<MenuItem> edge_color_menu = {
        leaf("Orange", [&state]{ state.pp_cfg.edge_color = IM_COL32(255, 160,  32, 255); }),
        leaf("Teal",   [&state]{ state.pp_cfg.edge_color = IM_COL32(  0, 220, 180, 255); }),
        leaf("Cyan",   [&state]{ state.pp_cfg.edge_color = IM_COL32(  0, 180, 255, 255); }),
        leaf("Green",  [&state]{ state.pp_cfg.edge_color = IM_COL32( 30, 220,  60, 255); }),
        leaf("White",  [&state]{ state.pp_cfg.edge_color = IM_COL32(255, 255, 255, 255); }),
    };

    std::vector<MenuItem> desat_strength_menu = {
        leaf("25%",  [&state]{ state.pp_cfg.desat_strength = 0.25f; }),
        leaf("50%",  [&state]{ state.pp_cfg.desat_strength = 0.50f; }),
        leaf("75%",  [&state]{ state.pp_cfg.desat_strength = 0.75f; }),
        leaf("100%", [&state]{ state.pp_cfg.desat_strength = 1.00f; }),
    };

    std::vector<MenuItem> bg_threshold_menu = {
        leaf("Subtle (0.25)",     [&state]{ state.pp_cfg.contrast_threshold = 0.25f; }),
        leaf("Medium (0.15)",     [&state]{ state.pp_cfg.contrast_threshold = 0.15f; }),
        leaf("Aggressive (0.07)", [&state]{ state.pp_cfg.contrast_threshold = 0.07f; }),
    };

    std::vector<MenuItem> vision_menu = {
        toggle("Edge Highlight",
            [&state]{ return state.pp_cfg.edge_enabled; },
            [&state](bool v){ state.pp_cfg.edge_enabled = v; }),
        submenu("Edge Strength",  std::move(edge_strength_menu)),
        submenu("Edge Color",     std::move(edge_color_menu)),
        toggle("Bg Desaturate",
            [&state]{ return state.pp_cfg.desat_enabled; },
            [&state](bool v){ state.pp_cfg.desat_enabled = v; }),
        submenu("Desat Strength", std::move(desat_strength_menu)),
        submenu("BG Threshold",   std::move(bg_threshold_menu)),
    };

    std::vector<MenuItem> settings_menu = {
        submenu("Headset",        std::move(headset_menu)),
        submenu("Audio",          std::move(audio_menu)),
        submenu("HUD",            std::move(hud_menu)),
        submenu("Android Mirror", std::move(android_menu)),
    };

    return {
        submenu("Cameras",       std::move(cameras_menu)),
        submenu("Prototracer",   std::move(prototracer_menu)),
        submenu("Settings",      std::move(settings_menu)),
        submenu("Vision Assist", std::move(vision_menu)),
        leaf("Request Status",   [teensy]{ teensy->request_status(); }),
        leaf("Close Program",    [&state]{ state.quit = true; }),
    };
}

// ── Render one eye into its FBO ───────────────────────────────────────────────

static void render_eye_fbo(gl::Fbo& fbo,
                            bool draw_camera,          // attempt camera draw
                            std::function<bool()> draw_cam_fn,
                            GLuint fallback_tex) {
    fbo.bind();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    bool drew = false;
    if (draw_camera) drew = draw_cam_fn();

    // Fallback: blit a static GL texture (e.g. Beast camera at lower res)
    if (!drew && fallback_tex != 0) {
        // Simple blit: bind texture, use gl:: blit helpers if available
        // For now, leave black — the fallback path is rare (Beast not yet ported fully)
        (void)fallback_tex;
    }

    fbo.unbind();
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Resolve resource paths relative to the binary's own directory so the app
    // works from any working directory (e.g. run via symlink from project root).
    namespace fs = std::filesystem;
    std::string bin_dir;
    try {
        bin_dir = fs::canonical(argv[0]).parent_path().string();
    } catch (...) {
        bin_dir = ".";  // fallback: use CWD
    }
    auto res = [&bin_dir](const std::string& rel) {
        return bin_dir + "/" + rel;
    };

    // Config search order (no explicit path argument):
    //   1. <bin_dir>/../config/config.json  — in-tree dev layout (build/protohud →
    //                                         ../config/config.json).  Edits here
    //                                         take effect immediately without rebuild.
    //   2. <bin_dir>/config.json            — installed/packaged layout fallback.
    std::string cfg_path;
    if (argc > 1) {
        cfg_path = argv[1];
    } else {
        std::string dev_cfg = bin_dir + "/../config/config.json";
        std::string def_cfg = res("config.json");
        try {
            cfg_path = fs::exists(dev_cfg) ? dev_cfg : def_cfg;
        } catch (...) {
            cfg_path = def_cfg;
        }
    }
    json cfg = load_config(cfg_path);

    // ── Config extraction ─────────────────────────────────────────────────────

    json empty = json::object();
    auto& jdisp = cfg.contains("display")  ? cfg["display"]  : empty;
    auto& jcam  = cfg.contains("cameras")  ? cfg["cameras"]  : empty;
    auto& jser  = cfg.contains("serial")   ? cfg["serial"]   : empty;
    auto& jvtr  = cfg.contains("vitrue")   ? cfg["vitrue"]   : empty;
    auto& jhud  = cfg.contains("hud")      ? cfg["hud"]      : empty;
    auto& jaud  = cfg.contains("audio")    ? cfg["audio"]    : empty;

    // Audio: RP2350 provides pre-processed stereo via USB Audio (UAC2).
    // CM5 just receives, applies gain, and routes to the chosen output.
    AudioConfig aud_cfg;
    aud_cfg.enabled           = jval(jaud, "enabled",     true);
    aud_cfg.capture_device    = jaud.value("capture_device",
                                    std::string("hw:CARD=HelmetAudio6Mic,DEV=0"));
    aud_cfg.output_viture     = jaud.value("output_viture",
                                    std::string("hw:CARD=VITUREXRGlasses,DEV=0"));
    aud_cfg.output_headphones = jaud.value("output_headphones",
                                    std::string("hw:CARD=Headphones,DEV=0"));
    aud_cfg.output_hdmi       = jaud.value("output_hdmi",
                                    std::string("hw:CARD=vc4hdmi0,DEV=0"));
    aud_cfg.sample_rate       = jval(jaud, "sample_rate",  48000);
    aud_cfg.period_size       = jval(jaud, "period_size",  256);
    aud_cfg.n_periods         = jval(jaud, "n_periods",    4);
    aud_cfg.master_gain       = jval(jaud, "master_gain",  1.0f);

    // Resolve active output from config string
    {
        std::string out_str = jaud.value("active_output", std::string("viture"));
        if      (out_str == "headphones") aud_cfg.active_output = AudioOutput::HEADPHONES;
        else if (out_str == "hdmi")       aud_cfg.active_output = AudioOutput::HDMI;
        else                              aud_cfg.active_output = AudioOutput::VITURE;
    }

    XRConfig xr_cfg;
    xr_cfg.product_id       = jval(jvtr,  "product_id",       0);
    xr_cfg.monitor_index    = jval(jvtr,  "monitor_index",    -1);
    xr_cfg.target_fps       = jval(jdisp, "target_fps",       90);
    xr_cfg.use_beast_camera = jval(jvtr,  "use_beast_camera", true);
    xr_cfg.enable_imu       = jval(jvtr,  "enable_imu",       true);
    xr_cfg.frameless        = jval(jdisp, "frameless",         false);

    CamConfig owl_left, owl_right;
    if (jcam.contains("owlsight_left")) {
        auto& jl          = jcam["owlsight_left"];
        owl_left.libcamera_id = jl.value("libcamera_id", 0);
        owl_left.width        = jl.value("width",  1280);
        owl_left.height       = jl.value("height",  800);
        owl_left.fps          = jl.value("fps",      60);
    }
    if (jcam.contains("owlsight_right")) {
        auto& jr           = jcam["owlsight_right"];
        owl_right.libcamera_id = jr.value("libcamera_id", 1);
        owl_right.width        = jr.value("width",  1280);
        owl_right.height       = jr.value("height",  800);
        owl_right.fps          = jr.value("fps",      60);
    }

    UsbCamConfig usb1_cfg, usb2_cfg;
    if (jcam.contains("usb_cam_1")) {
        usb1_cfg.device = jcam["usb_cam_1"].value("device", "/dev/video2");
        usb1_cfg.width  = jcam["usb_cam_1"].value("width",  1280);
        usb1_cfg.height = jcam["usb_cam_1"].value("height",  720);
        usb1_cfg.fps    = jcam["usb_cam_1"].value("fps",      30);
    }
    if (jcam.contains("usb_cam_2")) {
        usb2_cfg.device = jcam["usb_cam_2"].value("device", "/dev/video3");
        usb2_cfg.width  = jcam["usb_cam_2"].value("width",  1280);
        usb2_cfg.height = jcam["usb_cam_2"].value("height",  720);
        usb2_cfg.fps    = jcam["usb_cam_2"].value("fps",      30);
    }

    HudConfig hud_cfg;
    hud_cfg.compass_height        = jval(jhud, "compass_height_px",        60);
    hud_cfg.compass_bottom_margin = jval(jhud, "compass_bottom_margin_px",  20);
    hud_cfg.compass_bg_opacity    = jval(jhud, "compass_bg_opacity",        0.75f);
    hud_cfg.compass_bg_side_fade  = jval(jhud, "compass_bg_side_fade_px",   80);
    hud_cfg.panel_width          = jval(jhud, "panel_width_px",       200);
    hud_cfg.health_panel_opacity = jval(jhud, "health_panel_opacity", 0.71f);
    hud_cfg.pip_corner_clip_px    = jval(jhud, "pip_corner_clip_px",   16.f);
    hud_cfg.opacity               = jval(jdisp,"hud_opacity",          0.85f);
    hud_cfg.scale                 = jval(jdisp,"hud_scale",            1.0f);
    hud_cfg.indicator_bg_enabled  = jval(jhud, "indicator_bg_enabled", false);

    Mpu9250::Config mpu_cfg;
    if (cfg.contains("mpu9250")) {
        auto& jm = cfg["mpu9250"];
        mpu_cfg.enabled         = jval(jm, "enabled",        false);
        mpu_cfg.i2c_bus         = jm.value("i2c_bus",        std::string("/dev/i2c-1"));
        mpu_cfg.mpu_addr        = jval(jm, "mpu_addr",       0x68);
        mpu_cfg.declination_deg = jval(jm, "declination_deg", 0.0f);
        mpu_cfg.heading_offset  = jval(jm, "heading_offset",  0.0f);
        if (jm.contains("mag_bias") && jm["mag_bias"].is_array() &&
            jm["mag_bias"].size() >= 3) {
            mpu_cfg.mag_bias_x = jm["mag_bias"][0].get<float>();
            mpu_cfg.mag_bias_y = jm["mag_bias"][1].get<float>();
            mpu_cfg.mag_bias_z = jm["mag_bias"][2].get<float>();
        }
    }

    AndroidMirrorConfig and_cfg;
    OverlayConfig       pip_overlay_cfg1, pip_overlay_cfg2;
    OverlayConfig       android_overlay_cfg;

    if (cfg.contains("pip")) {
        auto& jpip = cfg["pip"];
        // Per-camera configs; fall back to top-level pip values
        float def_size = jval(jpip, "size", 0.25f);
        std::string def_anch = jpip.value("anchor", std::string("top_left"));

        auto& jc1 = jpip.contains("cam1") ? jpip["cam1"] : jpip;
        pip_overlay_cfg1.size   = jval(jc1, "size",   def_size);
        pip_overlay_cfg1.anchor = parse_anchor(jc1.value("anchor", def_anch));

        std::string def_anch2 = jpip.contains("cam1") ? std::string("top_right") : def_anch;
        auto& jc2 = jpip.contains("cam2") ? jpip["cam2"] : jpip;
        pip_overlay_cfg2.size   = jval(jc2, "size",   def_size);
        pip_overlay_cfg2.anchor = parse_anchor(jc2.value("anchor", def_anch2));
    }

    if (cfg.contains("android")) {
        auto& jand = cfg["android"];
        and_cfg.enabled    = jval(jand, "enabled",   false);
        and_cfg.v4l2_sink  = jand.value("v4l2_sink", std::string("/dev/video4"));
        and_cfg.adb_serial = jand.value("adb_serial",std::string(""));
        and_cfg.max_size   = jval(jand, "max_size",   1080);
        and_cfg.fps        = jval(jand, "fps",         30);
        android_overlay_cfg.size   = jval(jand, "overlay_size", 0.40f);
        android_overlay_cfg.anchor = parse_anchor(
            jand.value("anchor", std::string("bottom_left")));
    }

    // ── Shared state ──────────────────────────────────────────────────────────

    AppState state;
    state.max_messages        = jval(jhud, "lora_message_history", 50);
    state.compass_bg_enabled  = jhud.value("compass_bg", false);

    if (cfg.contains("night_vision")) {
        auto& jnv = cfg["night_vision"];
        state.night_vision.exposure_ev = jnv.value("exposure_ev",  0.0f);
        state.night_vision.shutter_us  = jnv.value("shutter_us",  33333);
    }

    if (cfg.contains("post_process")) {
        auto& jpp = cfg["post_process"];
        state.pp_cfg.edge_enabled       = jpp.value("edge_enabled",       false);
        state.pp_cfg.edge_strength      = jpp.value("edge_strength",      0.7f);
        state.pp_cfg.desat_enabled      = jpp.value("desat_enabled",      false);
        state.pp_cfg.desat_strength     = jpp.value("desat_strength",     0.8f);
        state.pp_cfg.contrast_threshold = jpp.value("contrast_threshold", 0.15f);
        if (jpp.contains("edge_color") && jpp["edge_color"].is_array() &&
            jpp["edge_color"].size() >= 3) {
            auto& jc = jpp["edge_color"];
            uint8_t r = jc[0], g = jc[1], b = jc[2];
            uint8_t a = jpp["edge_color"].size() >= 4 ? (uint8_t)jc[3] : 255;
            state.pp_cfg.edge_color = IM_COL32(r, g, b, a);
        }
    }

    // Seed resolution state from whatever the OWLsight config says
    state.camera_resolution.width  = owl_left.width;
    state.camera_resolution.height = owl_left.height;
    state.camera_resolution.fps    = owl_left.fps;

    // ── Audio engine ──────────────────────────────────────────────────────────

    AudioEngine audio(aud_cfg, state);

    // ── XR Display + GLFW window ──────────────────────────────────────────────
    // XRDisplay calls glfwInit() + glfwCreateWindow() and establishes the EGL
    // GLES2 context. Everything else must come after this.

    XRDisplay xr(xr_cfg);
    if (!xr.init()) {
        std::cerr << "[main] XR display init failed\n";
        return 1;
    }

    // IMU → compass heading + spatial audio head tracking
    // Timestamp lets the MPU-9250 backup detect when the XR glasses go offline.
    std::atomic<int64_t> last_xr_imu_us { 0 };

    xr.on_imu_pose([&state, &last_xr_imu_us](float roll, float pitch, float yaw) {
        last_xr_imu_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float bearing = fmod(360.0f - yaw, 360.0f);
        std::lock_guard<std::mutex> lk(state.mtx);
        state.compass_heading = bearing;
        state.imu_pose = { roll, pitch, yaw };
    });

    xr.on_state_changed([](int id, int val) {
        std::cout << "[xr] state change id=" << id << " val=" << val << "\n";
    });

    // ── MPU-9250 backup compass ───────────────────────────────────────────────
    // Takes over compass_heading when the XR glasses haven't sent an IMU frame
    // for more than 2 seconds (glasses off, disconnected, or XR disabled).

    Mpu9250 mpu9250(mpu_cfg);
    mpu9250.set_heading_callback([&state, &last_xr_imu_us](float heading) {
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        bool xr_fresh = (now_us - last_xr_imu_us.load()) < 2'000'000LL; // 2 s

        std::lock_guard<std::mutex> lk(state.mtx);
        state.health.mpu9250_ok = true;
        if (!xr_fresh)
            state.compass_heading = heading;
    });

    if (!mpu9250.start() && mpu_cfg.enabled)
        std::cerr << "[main] MPU-9250 backup compass unavailable\n";

    // ── Async Timewarp ────────────────────────────────────────────────────────

    CameraIntrinsics K { 1920.0f, 1920.0f, 960.0f, 540.0f };
    AsyncTimewarp timewarp(K,
        res("assets/shaders/timewarp.vs").c_str(),
        res("assets/shaders/timewarp.fs").c_str());
    bool use_timewarp = timewarp.init();
    if (!use_timewarp)
        std::cerr << "[main] timewarp shader unavailable — skipping\n";

    // ── Post-processing (Vision Assist) ───────────────────────────────────────

    PostProcessor post_proc;
    gl::Fbo pp_fbo_left, pp_fbo_right;
    const bool pp_ok = post_proc.init(
        xr.eye_width(), xr.eye_height(),
        res("assets/shaders/postprocess.vs").c_str(),
        res("assets/shaders/postprocess.fs").c_str());
    if (pp_ok) {
        pp_fbo_left  = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_fbo_right = gl::make_fbo(xr.eye_width(), xr.eye_height());
    } else {
        std::cerr << "[main] post-process shader unavailable — Vision Assist disabled\n";
    }

    // ── Spatial audio ─────────────────────────────────────────────────────────

    if (!audio.start())
        std::cerr << "[main] spatial audio unavailable — continuing without audio\n";

    // ── HUD renderer ──────────────────────────────────────────────────────────
    // Must be after xr.init() so the GLFW window + GL context exist.

    HudColors hud_col;
    // Load runtime palette (saved by a previous session via menu edits).
    if (cfg.contains("hud_colors")) {
        auto& jc = cfg["hud_colors"];
        hud_col.glow_base        = jcolor(jc, "glow_base",        hud_col.glow_base);
        hud_col.text_fill        = jcolor(jc, "text_fill",        hud_col.text_fill);
        hud_col.ind_good         = jcolor(jc, "ind_good",         hud_col.ind_good);
        hud_col.ind_inactive     = jcolor(jc, "ind_inactive",     hud_col.ind_inactive);
        hud_col.ind_fail         = jcolor(jc, "ind_fail",         hud_col.ind_fail);
        hud_col.compass_tick     = jcolor(jc, "compass_tick",     hud_col.compass_tick);
        hud_col.compass_glow     = jcolor(jc, "compass_glow",     hud_col.compass_glow);
        hud_col.compass_bg_color = jcolor(jc, "compass_bg_color", hud_col.compass_bg_color);
    }
    HudRenderer hud(hud_cfg, hud_col);
    hud.load(xr.glfw_window());

    // ── Camera manager ────────────────────────────────────────────────────────

    CameraManager cameras;
    cameras.init(owl_left, owl_right, usb1_cfg, usb2_cfg,
                 res("assets/shaders/nv12.vs").c_str(),
                 res("assets/shaders/nv12.fs").c_str());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.health.cam_owl_left  = cameras.owl_left_ok();
        state.health.cam_owl_right = cameras.owl_right_ok();
        state.health.cam_usb1      = cameras.usb1_ok();
        state.health.cam_usb2      = cameras.usb2_ok();
    }

    // Startup autofocus
    bool af_on_startup = false;
    if (cfg.contains("camera"))
        af_on_startup = cfg["camera"].value("autofocus_on_startup", false);
    if (af_on_startup) {
        if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
        if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
        std::cout << "[main] autofocus on startup\n";
    }

    // ── Beast built-in passthrough camera ────────────────────────────────────

    VitureCamera beast_cam;
    bool use_beast_cam = xr_cfg.use_beast_camera && xr.glasses_found();
    if (use_beast_cam) {
        use_beast_cam = beast_cam.start(xr.product_id());
        if (!use_beast_cam)
            std::cerr << "[main] Beast camera unavailable — using OWLsight\n";
    }

    // ── Serial devices ────────────────────────────────────────────────────────

    std::string teensy_port = "/dev/ttyACM0";
    std::string lora_port   = "/dev/ttyUSB0";
    std::string knob_port   = "/dev/ttyACM1";
    int         baud        = 115200;

    if (jser.contains("teensy"))    teensy_port = jser["teensy"].value("port",    teensy_port);
    if (jser.contains("lora"))      lora_port   = jser["lora"].value("port",      lora_port);
    if (jser.contains("smartknob")) knob_port   = jser["smartknob"].value("port", knob_port);

    TeensyController teensy(teensy_port, baud, state);
    LoRaRadio        lora  (lora_port,   baud, state);
    SmartKnob        knob  (knob_port,   baud, state);

    teensy.start();
    lora.start();
    knob.start();

    uint16_t sleep_tmo = 30;
    if (jser.contains("smartknob"))
        sleep_tmo = jser["smartknob"].value("sleep_timeout_s", 30);
    knob.set_sleep_timeout(sleep_tmo);

    // ── Android mirror ────────────────────────────────────────────────────────

    AndroidMirror android_mirror(and_cfg);
    bool          android_overlay_active = false;
    GLuint        tex_android            = 0;

    if (and_cfg.enabled) {
        std::thread([&android_mirror]() { android_mirror.start(); }).detach();
        android_overlay_active = true;
    }

    // ── Menu system ───────────────────────────────────────────────────────────

    bool pip_cam1_overlay_active = false;
    bool pip_cam2_overlay_active = false;

    // menu_ptr is set to &menu after construction so HUD menu lambdas can call
    // into MenuSystem without a circular dependency at build time.
    MenuSystem* menu_ptr = nullptr;
    MenuSystem menu(build_menu(&teensy, &xr, &cameras, &lora, &knob, &audio, state,
                               &android_mirror, &android_overlay_active,
                               &pip_overlay_cfg1, &pip_overlay_cfg2,
                               &pip_cam1_overlay_active, &pip_cam2_overlay_active,
                               &android_overlay_cfg,
                               &hud.colors(), &hud.config(), &menu_ptr,
                               &mpu9250));
    menu_ptr = &menu;

    // Restore menu style from a previous session.
    if (cfg.contains("menu_style")) {
        auto& jm = cfg["menu_style"];
        menu.set_accent_color(jcolor(jm, "accent_color", menu.accent_color()));
        menu.set_bg_color    (jcolor(jm, "bg_color",     menu.bg_color()));
        menu.set_bg_enabled  (jval  (jm, "bg_enabled",   menu.bg_enabled()));
    }

    menu.set_detent_callback([&knob, &menu](int count) {
        knob.set_detents(count);
        int depth = menu.menu_depth();
        uint8_t amp = 200, freq = 80, strength = 150;
        if      (depth >= 3) { amp = 150; freq = 60; strength = 100; }
        else if (depth == 1) { amp = 255; freq = 100; strength = 200; }
        knob.set_haptic(amp, freq, strength);
    });

    knob.on_move([&menu](int8_t dir, int) {
        if (menu.is_open()) menu.navigate(dir);
    });

    knob.on_status([&state](uint8_t status, uint8_t param) {
        if      (status == 0x01) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.knob_ready = true;
            std::cout << "[knob] ready\n";
        }
        else if (status == 0x02) std::cout << "[knob] entering sleep\n";
        else if (status == 0x03) std::cout << "[knob] woke up reason=" << (int)param << "\n";
    });

    // ── GPIO button input ─────────────────────────────────────────────────────

    bool gpio_enabled   = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "enabled",           false);
    int button_1_gpio   = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "button_1_gpio",     17);
    int button_2_gpio   = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "button_2_gpio",     27);
    int button_3_gpio   = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "button_3_gpio",     22);
    int af_trigger_ms   = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "af_trigger_time_ms", 1500);
    int pip_trigger_ms  = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "pip_trigger_time_ms",2000);

    GpioButtons buttons(button_1_gpio, button_2_gpio, button_3_gpio, af_trigger_ms, pip_trigger_ms);
    bool pip_left_active  = false, pip_right_active  = false;  // GPIO-driven
    bool kb_pip_left      = false, kb_pip_right      = false;  // keyboard-driven

    // Edge-detection state for direct GLFW key polling (keys 1-5)
    bool prev_key[6] = {};  // indexed by key number 1-5

    if (gpio_enabled) {
        if (buttons.init()) {
            buttons.on_af_left([&cameras]() {
                std::cout << "[gpio] AF left\n";
                if (cameras.owl_left()) cameras.owl_left()->start_autofocus();
            });
            buttons.on_af_right([&cameras]() {
                std::cout << "[gpio] AF right\n";
                if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
            });
            buttons.on_pip_left ([&pip_left_active] () { pip_left_active  = true; });
            buttons.on_pip_right([&pip_right_active]() { pip_right_active = true; });
            buttons.on_select   ([&menu]()             { if (menu.is_open()) menu.select(); });
        } else {
            std::cerr << "[main] GPIO button init failed\n";
        }
    }

    // ── GL texture handles for camera sources ─────────────────────────────────

    GLuint tex_usb1  = 0;
    GLuint tex_usb2  = 0;
    GLuint tex_beast = 0;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    teensy.request_status();

    // ── Main render loop ──────────────────────────────────────────────────────

    double prev_time = glfwGetTime();

    while (!glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {

        // ── Delta time ────────────────────────────────────────────────────────
        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - prev_time);
        prev_time  = now;

        // ── Start ImGui frame (also processes GLFW input events) ──────────────
        hud.begin_frame(dt);

        // ── Keyboard input (via ImGui, which owns GLFW callbacks) ─────────────
        if (key_pressed(ImGuiKey_Escape)) { state.quit = true; break; }
        // Ctrl+Q / Ctrl+K — force-kill (immediate exit, skips graceful cleanup)
        if (ImGui::GetIO().KeyCtrl &&
            (key_pressed(ImGuiKey_Q) || key_pressed(ImGuiKey_K))) {
            std::exit(0);
        }
        if (key_pressed(ImGuiKey_M)) {
            if (menu.is_open()) menu.close();
            else                menu.open();
        }
        if (menu.is_open()) {
            if (key_pressed(ImGuiKey_UpArrow))    menu.navigate(-1);
            if (key_pressed(ImGuiKey_DownArrow))  menu.navigate(+1);
            if (key_pressed(ImGuiKey_Enter))      menu.select();
            if (key_pressed(ImGuiKey_Backspace))  menu.back();
        }

        // ── Camera texture uploads (CPU paths) ────────────────────────────────
        if (use_beast_cam) beast_cam.get_frame(tex_beast);
        cameras.get_usb1(tex_usb1);
        cameras.get_usb2(tex_usb2);
        android_mirror.get_frame(tex_android);

        // ── GPIO PiP button state ─────────────────────────────────────────────
        if (gpio_enabled) {
            buttons.update_pip_state();
            pip_left_active  = buttons.pip_left_active();
            pip_right_active = buttons.pip_right_active();
        }

        // ── Keyboard button emulation (number keys, direct GLFW polling) ─────
        // Uses glfwGetKey() directly — independent of ImGui key mapping and
        // the glfwPollEvents/begin_frame ordering.  Edge detection via prev_key[].
        // 1/2 = toggle PiP left/right   4/5 = autofocus left/right
        // 3   = menu select
        {
            GLFWwindow* win = static_cast<GLFWwindow*>(xr.glfw_window());
            auto edge = [&](int n, int glfw_key) -> bool {
                bool now = (glfwGetKey(win, glfw_key) == GLFW_PRESS);
                bool fired = now && !prev_key[n];
                prev_key[n] = now;
                return fired;
            };
            if (!menu.is_open()) {
                if (edge(1, GLFW_KEY_1)) kb_pip_left  = !kb_pip_left;
                if (edge(2, GLFW_KEY_2)) kb_pip_right = !kb_pip_right;
            }
            if (edge(3, GLFW_KEY_3) && menu.is_open()) menu.select();
            if (edge(4, GLFW_KEY_4) && cameras.owl_left())  cameras.owl_left()->start_autofocus();
            if (edge(5, GLFW_KEY_5) && cameras.owl_right()) cameras.owl_right()->start_autofocus();
        }

        // ── USB camera / Android mirror health update ─────────────────────────
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.cam_usb1       = cameras.usb1_ok();
            state.health.cam_usb2       = cameras.usb2_ok();
            state.health.android_mirror = android_mirror.is_connected();
        }

        // ── Update AF / focus state from cameras (atomics, no mutex needed) ─────
        if (cameras.owl_left()) {
            state.focus_left.af_locked = cameras.owl_left()->is_af_locked();
            state.focus_left.af_active = cameras.owl_left()->is_af_scanning();
        }
        if (cameras.owl_right()) {
            state.focus_right.af_locked = cameras.owl_right()->is_af_locked();
            state.focus_right.af_active = cameras.owl_right()->is_af_scanning();
        }

        // ── State snapshot ────────────────────────────────────────────────────
        AppState snap;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            snap.face            = state.face;
            snap.health          = state.health;
            snap.knob            = state.knob;
            snap.audio           = state.audio;
            snap.lora_nodes      = state.lora_nodes;
            snap.lora_messages   = state.lora_messages;
            snap.compass_heading    = state.compass_heading;
            snap.compass_bg_enabled = state.compass_bg_enabled;
            snap.imu_pose           = state.imu_pose;
            snap.focus_left         = state.focus_left;
            snap.focus_right        = state.focus_right;
            snap.night_vision       = state.night_vision;
            snap.pp_cfg             = state.pp_cfg;
        }

        // Record render-time pose for timewarp
        timewarp.begin_frame(snap.imu_pose);

        // ── Apply camera settings (exposure, shutter) — only on change ────────
        // NV mode off: AE enabled + ExposureValue compensation.
        // NV mode on:  AE disabled + manual ExposureTime (shutter speed).
        // AeEnable must be sent before ExposureTime or it is silently ignored.
        {
            static NightVisionState s_last_nv{};
            static bool s_first = true;
            const auto& nv = snap.night_vision;
            if (s_first ||
                nv.nv_enabled  != s_last_nv.nv_enabled  ||
                nv.exposure_ev != s_last_nv.exposure_ev  ||
                nv.shutter_us  != s_last_nv.shutter_us) {
                auto apply = [&](DmaCamera* cam) {
                    if (!cam) return;
                    cam->set_ae_enable(!nv.nv_enabled);
                    if (nv.nv_enabled)
                        cam->set_shutter_speed_us(nv.shutter_us);
                    else
                        cam->set_exposure_ev(nv.exposure_ev);
                };
                apply(cameras.owl_left());
                apply(cameras.owl_right());
                s_last_nv = nv;
                s_first   = false;
            }
        }

        // ── Render cameras into per-eye FBOs ──────────────────────────────────
        // Left eye
        {
            xr.eye_left().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            bool drew = false;
            if (use_beast_cam && tex_beast != 0) {
                // Beast passthrough — blit via timewarp shader (same NDC quad path)
                // For now: TODO: render tex_beast as fullscreen quad
                drew = false;  // fallback to OWLsight below
            }
            if (!drew) drew = cameras.draw_owl_left();

            xr.eye_left().unbind();
        }

        // Right eye
        {
            xr.eye_right().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            bool drew = false;
            if (use_beast_cam && tex_beast != 0) {
                drew = false;  // fallback to OWLsight below
            }
            if (!drew) drew = cameras.draw_owl_right();

            xr.eye_right().unbind();
        }

        // ── Post-processing (Vision Assist depth cues) ────────────────────────
        GLuint left_src  = xr.eye_left().tex;
        GLuint right_src = xr.eye_right().tex;
        if (pp_ok && post_proc.any_enabled(snap.pp_cfg) &&
                pp_fbo_left.valid() && pp_fbo_right.valid()) {
            post_proc.process(xr.eye_left().tex,  pp_fbo_left,  snap.pp_cfg);
            post_proc.process(xr.eye_right().tex, pp_fbo_right, snap.pp_cfg);
            left_src  = pp_fbo_left.tex;
            right_src = pp_fbo_right.tex;
        }

        // ── Composite or timewarp ─────────────────────────────────────────────
        ImuPose current_pose = xr.get_latest_imu_pose();

        if (use_timewarp) {
            // Apply rotational timewarp: warp each eye FBO into its half of the
            // default framebuffer using the latest IMU pose.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            int ew = xr.eye_width();
            int eh = xr.eye_height();

            // SBS layout for both glasses and desktop preview.
            // Glasses: window is 3840×1080 → each half is exactly 1920×1080.
            // Desktop: window is 1920×1080 → each half is 960×1080 (squeezed
            //          horizontally, fine for verifying both cameras are alive).
            int half_w = xr.display_width() / 2;
            int dh     = xr.display_height();
            glViewport(0,      0, half_w, dh);
            timewarp.warp_frame(left_src,  ew, eh, current_pose);
            glViewport(half_w, 0, half_w, dh);
            timewarp.warp_frame(right_src, ew, eh, current_pose);
        } else {
            // Standard composite (no latency correction)
            xr.composite();
        }

        // ── ImGui HUD overlay (renders to default framebuffer, on top of camera) ──

        hud.draw_frame(snap, xr.eye_width(), xr.eye_height());
        menu.set_glow_enabled(hud.config().glow_enabled);
        menu.draw(xr.eye_width(), xr.eye_height());

        hud.draw_pip(tex_usb1, "Cam 1",
                     xr.eye_width(), xr.eye_height(),
                     pip_cam1_overlay_active || pip_left_active || kb_pip_left,
                     pip_overlay_cfg1,
                     snap.focus_left, snap.night_vision.nv_enabled);
        hud.draw_pip(tex_usb2, "Cam 2",
                     xr.eye_width(), xr.eye_height(),
                     pip_cam2_overlay_active || pip_right_active || kb_pip_right,
                     pip_overlay_cfg2,
                     snap.focus_right, snap.night_vision.nv_enabled);

        hud.draw_android_overlay(tex_android,
                                  xr.eye_width(), xr.eye_height(),
                                  android_overlay_active,
                                  android_mirror.is_running() && !android_mirror.is_connected(),
                                  android_overlay_cfg,
                                  android_mirror.frame_aspect());

        hud.render_overlay();

        // ── Swap ──────────────────────────────────────────────────────────────
        xr.present();
    }

    // ── Persist runtime settings ──────────────────────────────────────────────
    // Capture whatever colors/flags the user set via the HUD menu and write
    // them back to the config file so they survive a restart.
    try {
        auto& jc = cfg["hud_colors"];
        jc["glow_base"]        = color_to_json(hud.colors().glow_base);
        jc["text_fill"]        = color_to_json(hud.colors().text_fill);
        jc["ind_good"]         = color_to_json(hud.colors().ind_good);
        jc["ind_inactive"]     = color_to_json(hud.colors().ind_inactive);
        jc["ind_fail"]         = color_to_json(hud.colors().ind_fail);
        jc["compass_tick"]     = color_to_json(hud.colors().compass_tick);
        jc["compass_glow"]     = color_to_json(hud.colors().compass_glow);
        jc["compass_bg_color"] = color_to_json(hud.colors().compass_bg_color);

        cfg["hud"]["indicator_bg_enabled"] = hud.config().indicator_bg_enabled;
        cfg["hud"]["compass_bg"]           = state.compass_bg_enabled;

        auto& jpp = cfg["post_process"];
        jpp["edge_enabled"]       = state.pp_cfg.edge_enabled;
        jpp["edge_strength"]      = state.pp_cfg.edge_strength;
        jpp["edge_color"]         = color_to_json(state.pp_cfg.edge_color);
        jpp["desat_enabled"]      = state.pp_cfg.desat_enabled;
        jpp["desat_strength"]     = state.pp_cfg.desat_strength;
        jpp["contrast_threshold"] = state.pp_cfg.contrast_threshold;

        auto& jm = cfg["menu_style"];
        jm["accent_color"] = color_to_json(menu.accent_color());
        jm["bg_color"]     = color_to_json(menu.bg_color());
        jm["bg_enabled"]   = menu.bg_enabled();

        // Persist MPU-9250 calibration biases so they survive a restart
        if (mpu9250.is_running() || cfg.contains("mpu9250")) {
            float bx, by, bz;
            mpu9250.get_mag_bias(bx, by, bz);
            cfg["mpu9250"]["mag_bias"] = json::array({ bx, by, bz });
        }

        FILE* f = fopen(cfg_path.c_str(), "w");
        if (f) {
            std::string s = cfg.dump(2);
            fwrite(s.c_str(), 1, s.size(), f);
            fclose(f);
            std::cout << "[cfg] settings saved to " << cfg_path << "\n";
        } else {
            std::cerr << "[cfg] cannot write to " << cfg_path << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[cfg] save failed: " << e.what() << "\n";
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────

    mpu9250.stop();
    audio.stop();
    android_mirror.stop();
    hud.unload();
    beast_cam.stop();
    cameras.shutdown();
    teensy.stop();
    lora.stop();
    knob.stop();

    if (tex_usb1)    glDeleteTextures(1, &tex_usb1);
    if (tex_usb2)    glDeleteTextures(1, &tex_usb2);
    if (tex_beast)   glDeleteTextures(1, &tex_beast);
    if (tex_android) glDeleteTextures(1, &tex_android);

    pp_fbo_left.destroy();
    pp_fbo_right.destroy();
    post_proc.shutdown();

    xr.shutdown();
    return 0;
}
