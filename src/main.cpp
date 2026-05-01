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
#include <linux/videodev2.h>
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
        Mpu9250* mpu9250,
        const std::vector<std::string>& gif_names)
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

    // leaf with a radio-indicator getter — shows filled dot when get_state() is true
    auto leaf_sel = [](std::string lbl, std::function<void()> fn,
                       std::function<bool()> state_fn) -> MenuItem {
        MenuItem m;
        m.label     = std::move(lbl);
        m.type      = MenuItemType::LEAF;
        m.action    = std::move(fn);
        m.get_state = std::move(state_fn);
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

    // ── GIFs ─────────────────────────────────────────────────────────────────
    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) {
        std::string lbl = (i < gif_names.size() && !gif_names[i].empty())
                          ? gif_names[i]
                          : "GIF #" + std::to_string(i);
        gifs.push_back(leaf(lbl, [teensy, i]{ teensy->play_gif(i); }));
    }

    // ── Camera controls ───────────────────────────────────────────────────────
    std::vector<MenuItem> focus_modes = {
        leaf_sel("Manual", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::MANUAL;
            state.focus_right.mode = CameraFocusState::Mode::MANUAL;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->stop_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->stop_autofocus();
            }
        }, [&state]{ return state.focus_left.mode == CameraFocusState::Mode::MANUAL; }),
        leaf_sel("Auto", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::AUTO;
            state.focus_right.mode = CameraFocusState::Mode::AUTO;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->start_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->start_autofocus();
            }
        }, [&state]{ return state.focus_left.mode == CameraFocusState::Mode::AUTO; }),
        leaf_sel("Slave", [cameras, &state]{
            state.focus_left.mode  = CameraFocusState::Mode::SLAVE;
            state.focus_right.mode = CameraFocusState::Mode::SLAVE;
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->stop_autofocus();
                if (cameras->owl_right()) cameras->owl_right()->stop_autofocus();
            }
        }, [&state]{ return state.focus_left.mode == CameraFocusState::Mode::SLAVE; }),
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
    using A = OverlayConfig::Anchor;

    auto make_position_items = [&leaf, &leaf_sel](OverlayConfig* cfg) {
        return std::vector<MenuItem>{
            leaf_sel("Top Left",      [cfg]{ cfg->anchor = A::TOP_LEFT;      }, [cfg]{ return cfg->anchor == A::TOP_LEFT;      }),
            leaf_sel("Top Center",    [cfg]{ cfg->anchor = A::TOP_CENTER;    }, [cfg]{ return cfg->anchor == A::TOP_CENTER;    }),
            leaf_sel("Top Right",     [cfg]{ cfg->anchor = A::TOP_RIGHT;     }, [cfg]{ return cfg->anchor == A::TOP_RIGHT;     }),
            leaf_sel("Bottom Left",   [cfg]{ cfg->anchor = A::BOTTOM_LEFT;   }, [cfg]{ return cfg->anchor == A::BOTTOM_LEFT;   }),
            leaf_sel("Bottom Center", [cfg]{ cfg->anchor = A::BOTTOM_CENTER; }, [cfg]{ return cfg->anchor == A::BOTTOM_CENTER; }),
            leaf_sel("Bottom Right",  [cfg]{ cfg->anchor = A::BOTTOM_RIGHT;  }, [cfg]{ return cfg->anchor == A::BOTTOM_RIGHT;  }),
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
    std::vector<MenuItem> usb1_brightness_menu = {
        leaf_sel("50%",  [cameras]{ if (cameras) cameras->set_usb1_brightness(0.5f); }, [cameras]{ return cameras && cameras->usb1_brightness() == 0.5f; }),
        leaf_sel("100%", [cameras]{ if (cameras) cameras->set_usb1_brightness(1.0f); }, [cameras]{ return cameras && cameras->usb1_brightness() == 1.0f; }),
        leaf_sel("150%", [cameras]{ if (cameras) cameras->set_usb1_brightness(1.5f); }, [cameras]{ return cameras && cameras->usb1_brightness() == 1.5f; }),
        leaf_sel("200%", [cameras]{ if (cameras) cameras->set_usb1_brightness(2.0f); }, [cameras]{ return cameras && cameras->usb1_brightness() == 2.0f; }),
        leaf_sel("300%", [cameras]{ if (cameras) cameras->set_usb1_brightness(3.0f); }, [cameras]{ return cameras && cameras->usb1_brightness() == 3.0f; }),
    };
    std::vector<MenuItem> usb1_exposure_menu = {
        toggle("Auto Exposure",
            [cameras]{ return !cameras || cameras->usb1_cfg().auto_exposure; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.auto_exposure = v;
                cameras->update_usb1_cfg(c);
                cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1);
            }),
        slider("Exposure Time", 1.f, 5000.f, 10.f, "",
            [cameras]{ return cameras ? (float)cameras->usb1_cfg().exposure_time : 157.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.exposure_time = (int)v;
                cameras->update_usb1_cfg(c);
                cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v);
            }),
        toggle("Auto White Balance",
            [cameras]{ return !cameras || cameras->usb1_cfg().auto_wb; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.auto_wb = v;
                cameras->update_usb1_cfg(c);
                cameras->set_usb1_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0);
            }),
        slider("WB Temperature", 2800.f, 6500.f, 100.f, "K",
            [cameras]{ return cameras ? (float)cameras->usb1_cfg().wb_temp : 4600.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.wb_temp = (int)v;
                cameras->update_usb1_cfg(c);
                cameras->set_usb1_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v);
            }),
        toggle("Dynamic Framerate",
            [cameras]{ return cameras && cameras->usb1_cfg().dynamic_framerate; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.dynamic_framerate = v;
                cameras->update_usb1_cfg(c);
                cameras->set_usb1_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0);
            }),
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
        submenu("Overlay",     std::move(cam1_overlay_menu)),
        submenu("Brightness",  std::move(usb1_brightness_menu)),
        submenu("Exposure",    std::move(usb1_exposure_menu)),
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
    std::vector<MenuItem> usb2_brightness_menu = {
        leaf_sel("50%",  [cameras]{ if (cameras) cameras->set_usb2_brightness(0.5f); }, [cameras]{ return cameras && cameras->usb2_brightness() == 0.5f; }),
        leaf_sel("100%", [cameras]{ if (cameras) cameras->set_usb2_brightness(1.0f); }, [cameras]{ return cameras && cameras->usb2_brightness() == 1.0f; }),
        leaf_sel("150%", [cameras]{ if (cameras) cameras->set_usb2_brightness(1.5f); }, [cameras]{ return cameras && cameras->usb2_brightness() == 1.5f; }),
        leaf_sel("200%", [cameras]{ if (cameras) cameras->set_usb2_brightness(2.0f); }, [cameras]{ return cameras && cameras->usb2_brightness() == 2.0f; }),
        leaf_sel("300%", [cameras]{ if (cameras) cameras->set_usb2_brightness(3.0f); }, [cameras]{ return cameras && cameras->usb2_brightness() == 3.0f; }),
    };
    std::vector<MenuItem> usb2_exposure_menu = {
        toggle("Auto Exposure",
            [cameras]{ return !cameras || cameras->usb2_cfg().auto_exposure; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.auto_exposure = v;
                cameras->update_usb2_cfg(c);
                cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1);
            }),
        slider("Exposure Time", 1.f, 5000.f, 10.f, "",
            [cameras]{ return cameras ? (float)cameras->usb2_cfg().exposure_time : 157.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.exposure_time = (int)v;
                cameras->update_usb2_cfg(c);
                cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v);
            }),
        toggle("Auto White Balance",
            [cameras]{ return !cameras || cameras->usb2_cfg().auto_wb; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.auto_wb = v;
                cameras->update_usb2_cfg(c);
                cameras->set_usb2_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0);
            }),
        slider("WB Temperature", 2800.f, 6500.f, 100.f, "K",
            [cameras]{ return cameras ? (float)cameras->usb2_cfg().wb_temp : 4600.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.wb_temp = (int)v;
                cameras->update_usb2_cfg(c);
                cameras->set_usb2_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v);
            }),
        toggle("Dynamic Framerate",
            [cameras]{ return cameras && cameras->usb2_cfg().dynamic_framerate; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.dynamic_framerate = v;
                cameras->update_usb2_cfg(c);
                cameras->set_usb2_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0);
            }),
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
        submenu("Overlay",    std::move(cam2_overlay_menu)),
        submenu("Brightness", std::move(usb2_brightness_menu)),
        submenu("Exposure",   std::move(usb2_exposure_menu)),
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
        submenu("Faces",      std::move(effects)),
        submenu("Color",      std::move(colors)),
        submenu("Animations", std::move(gifs)),
        slider("Brightness", 0.f, 255.f, 1.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
        slider("Lens Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
        leaf("Release Control", [teensy]{ teensy->release_control(); }),
    };

    // ── HUD settings ──────────────────────────────────────────────────────────

    auto make_color_items = [&leaf, &leaf_sel](
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

    std::vector<MenuItem> compass_menu = {
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
            bool border, float thickness, SelectionStyle style, ImU32 menu_accent) {
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
        }
    };

    std::vector<MenuItem> themes_menu = {
        leaf("Halo", [apply_theme]{
            apply_theme(IM_COL32(255,255,255,255), IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255), IM_COL32(255,255,255,180),
                        true, 5.f, SelectionStyle::FILLED_ROW,
                        IM_COL32(255,255,255,255));
        }),
        leaf("Solar", [apply_theme]{
            apply_theme(IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        IM_COL32(255,160, 32,255));
        }),
        leaf("Fallout", [apply_theme]{
            apply_theme(IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        IM_COL32(  0,255, 80,255),
                        IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        IM_COL32(  0,200, 50,255));
        }),
        leaf("Space", [apply_theme]{
            apply_theme(IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        IM_COL32(200,220,255,255),
                        IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        IM_COL32( 80,100,255,255));
        }),
    };

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

    std::vector<MenuItem> hud_menu = {
        toggle("Flip to Top",
            [hud_cfg]{ return hud_cfg->hud_flip_vertical; },
            [hud_cfg](bool v){ hud_cfg->hud_flip_vertical = v; }),
        slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
        submenu("Compass",       std::move(compass_menu)),
        submenu("Clock",         std::move(clock_menu)),
        submenu("Color",         std::move(color_options_menu)),
        submenu("Menu Position", std::move(menu_position_menu)),
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

    std::vector<MenuItem> settings_menu = {
        submenu("Headset",        std::move(headset_menu)),
        submenu("Audio",          std::move(audio_menu)),
        submenu("HUD",            std::move(hud_menu)),
        submenu("Android Mirror", std::move(android_menu)),
    };

    cameras_menu.push_back(submenu("Vision Assist", std::move(vision_menu)));

    return {
        submenu("Cameras",          std::move(cameras_menu)),
        submenu("Prototracer",      std::move(prototracer_menu)),
        submenu("Settings",         std::move(settings_menu)),
        submenu("Timers and Alarm", std::move(timers_alarm_menu)),
        leaf("Request Status",      [teensy]{ teensy->request_status(); }),
        leaf("Close Program",       [&state]{ state.quit = true; }),
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
        auto& j1 = jcam["usb_cam_1"];
        usb1_cfg.device             = j1.value("device",             "/dev/video2");
        usb1_cfg.width              = j1.value("width",               1280);
        usb1_cfg.height             = j1.value("height",               720);
        usb1_cfg.fps                = j1.value("fps",                   30);
        usb1_cfg.brightness         = j1.value("brightness",           1.0f);
        usb1_cfg.dynamic_framerate  = j1.value("dynamic_framerate",    false);
        usb1_cfg.auto_exposure      = j1.value("auto_exposure",        true);
        usb1_cfg.exposure_time      = j1.value("exposure_time",        157);
        usb1_cfg.auto_wb            = j1.value("auto_wb",              true);
        usb1_cfg.wb_temp            = j1.value("wb_temp",              4600);
    }
    if (jcam.contains("usb_cam_2")) {
        auto& j2 = jcam["usb_cam_2"];
        usb2_cfg.device             = j2.value("device",             "/dev/video3");
        usb2_cfg.width              = j2.value("width",               1280);
        usb2_cfg.height             = j2.value("height",               720);
        usb2_cfg.fps                = j2.value("fps",                   30);
        usb2_cfg.brightness         = j2.value("brightness",           1.0f);
        usb2_cfg.dynamic_framerate  = j2.value("dynamic_framerate",    false);
        usb2_cfg.auto_exposure      = j2.value("auto_exposure",        true);
        usb2_cfg.exposure_time      = j2.value("exposure_time",        157);
        usb2_cfg.auto_wb            = j2.value("auto_wb",              true);
        usb2_cfg.wb_temp            = j2.value("wb_temp",              4600);
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
    hud_cfg.indicator_bg_enabled  = jval(jhud, "indicator_bg_enabled", true);
    hud_cfg.glow_intensity        = jval(jhud, "glow_intensity",       1.0f);
    hud_cfg.hud_flip_vertical     = jval(jhud, "flip_vertical",        false);

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
    state.compass_bg_enabled  = jhud.value("compass_bg", true);

    if (jhud.contains("effects")) {
        auto& jfx = jhud["effects"];
        state.effects_cfg.effect  = static_cast<EffectType> (jval(jfx, "type",    0));
        state.effects_cfg.palette = static_cast<EffectPalette>(jval(jfx, "palette", 0));
    }

    if (cfg.contains("night_vision")) {
        auto& jnv = cfg["night_vision"];
        state.night_vision.exposure_ev = jnv.value("exposure_ev",  0.0f);
        state.night_vision.shutter_us  = jnv.value("shutter_us",  33333);
    }

    if (cfg.contains("clock")) {
        auto& jck = cfg["clock"];
        state.clock_cfg.use_24h         = jck.value("use_24h",         true);
        state.clock_cfg.show_seconds    = jck.value("show_seconds",     true);
        state.clock_cfg.show_date       = jck.value("show_date",        false);
        state.clock_cfg.font_scale      = jck.value("font_scale",       1.5f);
        state.clock_cfg.manual_offset_s = jck.value("manual_offset_s",  0);
    }

    if (cfg.contains("post_process")) {
        auto& jpp = cfg["post_process"];
        state.pp_cfg.edge_enabled       = jpp.value("edge_enabled",       false);
        state.pp_cfg.edge_strength      = jpp.value("edge_strength",      0.7f);
        state.pp_cfg.desat_enabled      = jpp.value("desat_enabled",      false);
        state.pp_cfg.desat_strength     = jpp.value("desat_strength",     0.8f);
        state.pp_cfg.contrast_threshold = jpp.value("contrast_threshold", 0.15f);
        state.pp_cfg.edge_scale         = jpp.value("edge_scale",         3.0f);
        state.pp_cfg.edge_threshold     = jpp.value("edge_threshold",     0.15f);
        state.pp_cfg.focus_str          = jpp.value("focus_str",          0.0f);
        state.pp_cfg.edge_gate_scale    = jpp.value("edge_gate_scale",    2.0f);
        if (jpp.contains("edge_color") && jpp["edge_color"].is_array() &&
            jpp["edge_color"].size() >= 3) {
            auto& jc = jpp["edge_color"];
            uint8_t r = jc[0], g = jc[1], b = jc[2];
            uint8_t a = jpp["edge_color"].size() >= 4 ? (uint8_t)jc[3] : 255;
            state.pp_cfg.edge_color = IM_COL32(r, g, b, a);
        }
        state.pp_cfg.motion_enabled  = jpp.value("motion_enabled",  false);
        state.pp_cfg.motion_strength = jpp.value("motion_strength", 0.9f);
        state.pp_cfg.motion_thresh   = jpp.value("motion_thresh",   0.04f);
        state.pp_cfg.motion_radius   = jpp.value("motion_radius",   3.0f);
        state.pp_cfg.motion_line        = jpp.value("motion_line",        1.0f);
        state.pp_cfg.motion_update_rate = jpp.value("motion_update_rate", 0.5f);
        if (jpp.contains("motion_color") && jpp["motion_color"].is_array() &&
            jpp["motion_color"].size() >= 3) {
            auto& jc = jpp["motion_color"];
            uint8_t r = jc[0], g = jc[1], b = jc[2];
            uint8_t a = jpp["motion_color"].size() >= 4 ? (uint8_t)jc[3] : 255;
            state.pp_cfg.motion_color = IM_COL32(r, g, b, a);
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
    // Ping-pong pairs for motion EMA reference frames (GLES 2.0 can't read+write same FBO)
    gl::Fbo pp_prev_left[2], pp_prev_right[2];
    bool    pp_ping_left  = false;   // index of the current READ buffer (0 or 1)
    bool    pp_ping_right = false;
    const bool pp_ok = post_proc.init(
        xr.eye_width(), xr.eye_height(),
        res("assets/shaders/postprocess.vs").c_str(),
        res("assets/shaders/postprocess.fs").c_str());
    if (pp_ok) {
        pp_fbo_left   = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_fbo_right  = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_prev_left[0]  = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_prev_left[1]  = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_prev_right[0] = gl::make_fbo(xr.eye_width(), xr.eye_height());
        pp_prev_right[1] = gl::make_fbo(xr.eye_width(), xr.eye_height());
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

    std::vector<std::string> gif_names;
    if (jser.contains("teensy")) {
        const auto& jt = jser["teensy"];
        if (jt.contains("gif_names") && jt["gif_names"].is_array()) {
            for (const auto& n : jt["gif_names"])
                gif_names.push_back(n.get<std::string>());
        }
    }
    gif_names.resize(8);

    // menu_ptr is set to &menu after construction so HUD menu lambdas can call
    // into MenuSystem without a circular dependency at build time.
    MenuSystem* menu_ptr = nullptr;
    MenuSystem menu(build_menu(&teensy, &xr, &cameras, &lora, &knob, &audio, state,
                               &android_mirror, &android_overlay_active,
                               &pip_overlay_cfg1, &pip_overlay_cfg2,
                               &pip_cam1_overlay_active, &pip_cam2_overlay_active,
                               &android_overlay_cfg,
                               &hud.colors(), &hud.config(), &menu_ptr,
                               &mpu9250, gif_names));
    menu_ptr = &menu;

    // Restore menu style from a previous session.
    if (cfg.contains("menu_style")) {
        auto& jm = cfg["menu_style"];
        menu.set_accent_color    (jcolor(jm, "accent_color",     menu.accent_color()));
        menu.set_bg_color        (jcolor(jm, "bg_color",         menu.bg_color()));
        menu.set_bg_enabled      (jval  (jm, "bg_enabled",       menu.bg_enabled()));
        menu.set_border_color    (jcolor(jm, "border_color",     menu.border_color()));
        menu.set_border_thickness(jval  (jm, "border_thickness", menu.border_thickness()));
        menu.set_border_enabled  (jval  (jm, "border_enabled",   menu.border_enabled()));
        {
            std::string ss = jm.value("selection_style", "filled_row");
            menu.set_selection_style(ss == "accent_bar"
                ? SelectionStyle::ACCENT_BAR : SelectionStyle::FILLED_ROW);
        }
        {
            std::string a = jm.value("anchor", "top_left");
            if      (a == "top_right")    menu.set_anchor(MenuAnchor::TopRight);
            else if (a == "bottom_left")  menu.set_anchor(MenuAnchor::BottomLeft);
            else if (a == "bottom_right") menu.set_anchor(MenuAnchor::BottomRight);
            else                          menu.set_anchor(MenuAnchor::TopLeft);
        }
    }

    menu.set_detent_callback([&knob, &menu](int count) {
        knob.set_detents(count);
        int depth = menu.menu_depth();
        uint8_t amp = 200, freq = 80, strength = 150;
        if      (depth >= 3) { amp = 150; freq = 60; strength = 100; }
        else if (depth == 1) { amp = 255; freq = 100; strength = 200; }
        knob.set_haptic(amp, freq, strength);
    });

    knob.on_move([&menu, &hud](int8_t dir, int) {
        if      (hud.popup_active())  hud.popup_navigate(dir);
        else if (menu.is_open())      menu.navigate(dir);
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
    bool prev_key[8] = {};  // indexed by key number 1-6 + extras

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
            buttons.on_select   ([&menu, &hud]() {
                if      (hud.popup_active()) hud.popup_select();
                else if (menu.is_open())     menu.select();
            });
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
        if (key_pressed(ImGuiKey_Escape) || key_pressed(ImGuiKey_P)) { state.quit = true; break; }
        // Ctrl+Q / Ctrl+K — force-kill (immediate exit, skips graceful cleanup)
        if (ImGui::GetIO().KeyCtrl &&
            (key_pressed(ImGuiKey_Q) || key_pressed(ImGuiKey_K))) {
            std::exit(0);
        }
        if (key_pressed(ImGuiKey_M)) {
            if (menu.is_open()) menu.close();
            else                menu.open();
        }
        if (hud.popup_active()) {
            if (key_pressed(ImGuiKey_LeftArrow))  hud.popup_navigate(-1);
            if (key_pressed(ImGuiKey_RightArrow)) hud.popup_navigate(+1);
            if (key_pressed(ImGuiKey_Enter))      hud.popup_select();
        } else if (menu.is_open()) {
            if (key_pressed(ImGuiKey_UpArrow))    menu.navigate(-1);
            if (key_pressed(ImGuiKey_DownArrow))  menu.navigate(+1);
            if (key_pressed(ImGuiKey_Enter) ||
                key_pressed(ImGuiKey_RightArrow)) menu.select();
            if (key_pressed(ImGuiKey_Backspace) ||
                key_pressed(ImGuiKey_LeftArrow))  menu.back();
        }
        // Vision-assist hotkeys — work whether or not the menu is open
        if (key_pressed(ImGuiKey_E)) state.pp_cfg.edge_enabled   = !state.pp_cfg.edge_enabled;
        if (key_pressed(ImGuiKey_D)) state.pp_cfg.desat_enabled  = !state.pp_cfg.desat_enabled;
        if (key_pressed(ImGuiKey_W)) state.pp_cfg.motion_enabled = !state.pp_cfg.motion_enabled;

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

        // ── Keyboard button emulation (direct GLFW polling, edge-detected) ──
        // 1/2  = toggle PiP left/right    3 = menu select (when open)
        // 3    = toggle manual focus      4 = autofocus both cameras
        // ,/.  = focus in / focus out (step 20 of 0-1000)
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
            bool key3 = edge(3, GLFW_KEY_3);
            if (key3 && menu.is_open()) menu.select();
            // 3: toggle manual/auto focus
            if (key3 && !menu.is_open()) {
                bool go_manual = (state.focus_left.mode != CameraFocusState::Mode::MANUAL);
                if (go_manual) {
                    if (cameras.owl_left())  cameras.owl_left()->stop_autofocus();
                    if (cameras.owl_right()) cameras.owl_right()->stop_autofocus();
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.focus_left.mode  = CameraFocusState::Mode::MANUAL;
                    state.focus_right.mode = CameraFocusState::Mode::MANUAL;
                } else {
                    if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
                    if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.focus_left.mode  = CameraFocusState::Mode::AUTO;
                    state.focus_right.mode = CameraFocusState::Mode::AUTO;
                }
            }
            // 4: autofocus both cameras
            if (edge(4, GLFW_KEY_4)) {
                if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
                if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.focus_left.mode  = CameraFocusState::Mode::AUTO;
                state.focus_right.mode = CameraFocusState::Mode::AUTO;
            }
            // , / . : manual focus step (near / far)
            constexpr int FOCUS_STEP = 20;
            if (edge(5, GLFW_KEY_COMMA) && cameras.owl_left()) {
                int pos = std::max(0, cameras.owl_left()->get_focus_position() - FOCUS_STEP);
                cameras.owl_left()->set_focus_position(pos);
                if (cameras.owl_right()) cameras.owl_right()->set_focus_position(pos);
            }
            if (edge(6, GLFW_KEY_PERIOD) && cameras.owl_left()) {
                int pos = std::min(1000, cameras.owl_left()->get_focus_position() + FOCUS_STEP);
                cameras.owl_left()->set_focus_position(pos);
                if (cameras.owl_right()) cameras.owl_right()->set_focus_position(pos);
            }
        }

        // ── USB camera / Android mirror health update ─────────────────────────
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.cam_usb1       = cameras.usb1_ok();
            state.health.cam_usb2       = cameras.usb2_ok();
            state.health.android_mirror = android_mirror.is_connected();
        }

        // ── Timer / alarm expiry check ────────────────────────────────────────
        {
            auto& ta = state.timer_alarm;
            time_t now_t = time(nullptr);
            if (ta.timer_active && now_t >= ta.timer_end) {
                ta.timer_active    = false;
                ta.timer_triggered = true;  // shows timer-expired popup
            }
            if (ta.alarm_active && now_t >= ta.alarm_fire_at) {
                ta.alarm_active    = false;
                ta.alarm_triggered = true;  // shows alarm popup
            }
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
            snap.clock_cfg          = state.clock_cfg;
            snap.pp_cfg             = state.pp_cfg;
            snap.timer_alarm        = state.timer_alarm;
            snap.effects_cfg        = state.effects_cfg;
            memcpy(snap.lora_node_colors, state.lora_node_colors,
                   sizeof(state.lora_node_colors));
        }
        // Overlay-active flags mirror the exact condition used in draw_pip calls.
        snap.health.cam_usb1_overlay = pip_cam1_overlay_active || pip_left_active  || kb_pip_left;
        snap.health.cam_usb2_overlay = pip_cam2_overlay_active || pip_right_active || kb_pip_right;

        // Inject live AF lens position into the snapshot (atomic read, no lock needed)
        if (cameras.owl_left())
            snap.pp_cfg.focus_lens_pos = cameras.owl_left()->get_focus_position();

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
            post_proc.process(xr.eye_left().tex,
                              pp_prev_left[pp_ping_left],   pp_prev_left[!pp_ping_left],
                              pp_fbo_left,  snap.pp_cfg);
            pp_ping_left = !pp_ping_left;
            post_proc.process(xr.eye_right().tex,
                              pp_prev_right[pp_ping_right], pp_prev_right[!pp_ping_right],
                              pp_fbo_right, snap.pp_cfg);
            pp_ping_right = !pp_ping_right;
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

        // Alarm / timer-expired popups render on top of everything.
        hud.draw_popups(state, xr.eye_width(), xr.eye_height());

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
        cfg["hud"]["glow_intensity"]      = hud.config().glow_intensity;
        cfg["hud"]["compass_bg"]          = state.compass_bg_enabled;
        cfg["hud"]["flip_vertical"]             = hud.config().hud_flip_vertical;
        cfg["hud"]["effects"]["type"]           = static_cast<int>(state.effects_cfg.effect);
        cfg["hud"]["effects"]["palette"]        = static_cast<int>(state.effects_cfg.palette);

        cfg["clock"]["use_24h"]         = state.clock_cfg.use_24h;
        cfg["clock"]["show_seconds"]    = state.clock_cfg.show_seconds;
        cfg["clock"]["show_date"]       = state.clock_cfg.show_date;
        cfg["clock"]["font_scale"]      = state.clock_cfg.font_scale;
        cfg["clock"]["manual_offset_s"] = state.clock_cfg.manual_offset_s;

        auto& jpp = cfg["post_process"];
        jpp["edge_enabled"]       = state.pp_cfg.edge_enabled;
        jpp["edge_strength"]      = state.pp_cfg.edge_strength;
        jpp["edge_color"]         = color_to_json(state.pp_cfg.edge_color);
        jpp["desat_enabled"]      = state.pp_cfg.desat_enabled;
        jpp["desat_strength"]     = state.pp_cfg.desat_strength;
        jpp["contrast_threshold"] = state.pp_cfg.contrast_threshold;
        jpp["edge_scale"]         = state.pp_cfg.edge_scale;
        jpp["edge_threshold"]     = state.pp_cfg.edge_threshold;
        jpp["focus_str"]          = state.pp_cfg.focus_str;
        jpp["edge_gate_scale"]    = state.pp_cfg.edge_gate_scale;
        jpp["motion_enabled"]     = state.pp_cfg.motion_enabled;
        jpp["motion_strength"]    = state.pp_cfg.motion_strength;
        jpp["motion_thresh"]      = state.pp_cfg.motion_thresh;
        jpp["motion_radius"]      = state.pp_cfg.motion_radius;
        jpp["motion_line"]        = state.pp_cfg.motion_line;
        jpp["motion_update_rate"] = state.pp_cfg.motion_update_rate;
        jpp["motion_color"]       = color_to_json(state.pp_cfg.motion_color);

        cfg["cameras"]["usb_cam_1"]["brightness"]        = cameras.usb1_brightness();
        cfg["cameras"]["usb_cam_1"]["dynamic_framerate"] = cameras.usb1_cfg().dynamic_framerate;
        cfg["cameras"]["usb_cam_1"]["auto_exposure"]     = cameras.usb1_cfg().auto_exposure;
        cfg["cameras"]["usb_cam_1"]["exposure_time"]     = cameras.usb1_cfg().exposure_time;
        cfg["cameras"]["usb_cam_1"]["auto_wb"]           = cameras.usb1_cfg().auto_wb;
        cfg["cameras"]["usb_cam_1"]["wb_temp"]           = cameras.usb1_cfg().wb_temp;
        cfg["cameras"]["usb_cam_2"]["brightness"]        = cameras.usb2_brightness();
        cfg["cameras"]["usb_cam_2"]["dynamic_framerate"] = cameras.usb2_cfg().dynamic_framerate;
        cfg["cameras"]["usb_cam_2"]["auto_exposure"]     = cameras.usb2_cfg().auto_exposure;
        cfg["cameras"]["usb_cam_2"]["exposure_time"]     = cameras.usb2_cfg().exposure_time;
        cfg["cameras"]["usb_cam_2"]["auto_wb"]           = cameras.usb2_cfg().auto_wb;
        cfg["cameras"]["usb_cam_2"]["wb_temp"]           = cameras.usb2_cfg().wb_temp;

        auto& jm = cfg["menu_style"];
        jm["accent_color"]     = color_to_json(menu.accent_color());
        jm["bg_color"]         = color_to_json(menu.bg_color());
        jm["bg_enabled"]       = menu.bg_enabled();
        jm["border_color"]     = color_to_json(menu.border_color());
        jm["border_thickness"]  = menu.border_thickness();
        jm["border_enabled"]    = menu.border_enabled();
        jm["selection_style"]   = (menu.selection_style() == SelectionStyle::FILLED_ROW)
                                  ? "filled_row" : "accent_bar";
        {
            const char* a = "top_left";
            switch (menu.anchor()) {
                case MenuAnchor::TopRight:    a = "top_right";    break;
                case MenuAnchor::BottomLeft:  a = "bottom_left";  break;
                case MenuAnchor::BottomRight: a = "bottom_right"; break;
                default: break;
            }
            jm["anchor"] = a;
        }

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
    pp_prev_left[0].destroy();  pp_prev_left[1].destroy();
    pp_prev_right[0].destroy(); pp_prev_right[1].destroy();
    post_proc.shutdown();

    xr.shutdown();
    return 0;
}
