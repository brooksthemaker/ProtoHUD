#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <ctime>

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
#include "input/gamepad_input.h"
#include "input/wireless_controller.h"
#include "serial/face_controller.h"
#include "serial/protoface_controller.h"
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
#include "sys/system_monitor.h"
#include "net/wifi_monitor.h"
#include "net/ping_monitor.h"
#include "net/bt_monitor.h"
#include "crash_reporter.h"
#include "capture.h"
#include "qr_scanner.h"
#include "splash.h"

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

using json = nlohmann::json;

// ── Serial port auto-discovery ────────────────────────────────────────────────
// Tries the configured path first; if it doesn't exist, scans up to 8 nodes
// with the same prefix (ttyACM/ttyUSB) and optionally matches by USB VID:PID
// read from sysfs. Returns the best match (or the original path so the
// caller's open() fails with the right error message).
static std::string resolve_serial_port(const std::string& configured,
                                        uint16_t vid = 0, uint16_t pid = 0) {
    if (access(configured.c_str(), F_OK) == 0) return configured;

    // Determine prefix (ttyACM / ttyUSB)
    const char* pfx = nullptr;
    if (configured.find("/dev/ttyACM") == 0) pfx = "/dev/ttyACM";
    else if (configured.find("/dev/ttyUSB") == 0) pfx = "/dev/ttyUSB";
    if (!pfx) return configured;

    std::string tty_prefix(pfx);
    std::string sys_prefix = "/sys/class/tty/" + tty_prefix.substr(5); // strip /dev/

    std::string first_available;
    for (int n = 0; n < 8; n++) {
        std::string dev = tty_prefix + std::to_string(n);
        if (access(dev.c_str(), F_OK) != 0) continue;

        // If VID:PID were provided, check sysfs uevent for a match
        if (vid != 0) {
            std::string uevent = sys_prefix + std::to_string(n) + "/device/uevent";
            std::ifstream f(uevent);
            bool match = false;
            if (f) {
                std::string line;
                while (std::getline(f, line)) {
                    if (line.find("PRODUCT=") != 0) continue;
                    unsigned rv = 0, rp = 0;
                    if (sscanf(line.c_str(), "PRODUCT=%x/%x", &rv, &rp) == 2)
                        match = (rv == vid && rp == pid);
                    break;
                }
            }
            if (match) {
                std::cerr << "[serial] " << configured << " not found — using "
                          << dev << " (VID:PID match)\n";
                return dev;
            }
        }
        if (first_available.empty()) first_available = dev;
    }

    // No VID:PID match found; fall back to first available node with same prefix
    if (!first_available.empty() && vid == 0) {
        std::cerr << "[serial] " << configured << " not found — trying "
                  << first_available << "\n";
        return first_available;
    }
    return configured; // give up; caller logs the real open() error
}

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

// Convert legacy anchor string → fractional (anchor_x, anchor_y).
// Used only when reading old config files that lack anchor_x/anchor_y keys.
static void apply_legacy_anchor(const std::string& s, OverlayConfig& cfg) {
    if      (s == "top_left")      { cfg.anchor_x = 0.0f; cfg.anchor_y = 0.0f; }
    else if (s == "top_center")    { cfg.anchor_x = 0.5f; cfg.anchor_y = 0.0f; }
    else if (s == "top_right")     { cfg.anchor_x = 1.0f; cfg.anchor_y = 0.0f; }
    else if (s == "bottom_left")   { cfg.anchor_x = 0.0f; cfg.anchor_y = 1.0f; }
    else if (s == "bottom_center") { cfg.anchor_x = 0.5f; cfg.anchor_y = 1.0f; }
    else if (s == "bottom_right")  { cfg.anchor_x = 1.0f; cfg.anchor_y = 1.0f; }
    else                           { cfg.anchor_x = 0.5f; cfg.anchor_y = 0.0f; } // top_center default
}

static OverlayConfig::Rotation parse_rotation(const std::string& s) {
    if (s == "portrait")          return OverlayConfig::Rotation::Portrait;
    if (s == "landscape_flipped") return OverlayConfig::Rotation::LandscapeFlipped;
    if (s == "portrait_flipped")  return OverlayConfig::Rotation::PortraitFlipped;
    return OverlayConfig::Rotation::Landscape;
}
static const char* rotation_to_str(OverlayConfig::Rotation r) {
    using R = OverlayConfig::Rotation;
    switch (r) {
        case R::Portrait:         return "portrait";
        case R::LandscapeFlipped: return "landscape_flipped";
        case R::PortraitFlipped:  return "portrait_flipped";
        default:                  return "landscape";
    }
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

// Fires on initial key-down, then again after kDelay seconds, then every kRate seconds.
struct KeyRepeat {
    double press_t = -1.0;
    double last_t  = -1.0;
    static constexpr double kDelay = 0.65;
    static constexpr double kRate  = 0.08;

    bool tick(bool down) {
        double t = glfwGetTime();
        if (!down) { press_t = -1.0; return false; }
        if (press_t < 0.0) { press_t = last_t = t; return true; }
        if (t - press_t < kDelay) return false;
        if (t - last_t  >= kRate) { last_t = t;  return true; }
        return false;
    }
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

// ── GPIO poll (sysfs) ─────────────────────────────────────────────────────────
// Called from main loop ~1 Hz. Reads each monitored pin via sysfs export path.
static void poll_gpio_states(AppState& state) {
    // Export unexported pins and read values
    for (auto& ps : state.gpio_states) {
        // Try export (idempotent — ignore EBUSY)
        {
            int efd = open("/sys/class/gpio/export", O_WRONLY);
            if (efd >= 0) {
                char buf[16];
                int n = snprintf(buf, sizeof(buf), "%d", ps.pin);
                (void)write(efd, buf, n);
                close(efd);
            }
        }
        // Set direction to "in" (idempotent)
        {
            char path[64];
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", ps.pin);
            int dfd = open(path, O_WRONLY);
            if (dfd >= 0) {
                (void)write(dfd, "in", 2);
                close(dfd);
            }
        }
        // Read value
        {
            char path[64];
            snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", ps.pin);
            int vfd = open(path, O_RDONLY);
            if (vfd >= 0) {
                char val = '?';
                if (read(vfd, &val, 1) == 1)
                    ps.value = (val == '1') ? 1 : 0;
                close(vfd);
            } else {
                ps.value = -1;
            }
        }
    }
}

// ── Menu definition ───────────────────────────────────────────────────────────

static std::vector<MenuItem> build_menu(
        IFaceController* teensy, XRDisplay* xr, CameraManager* cameras,
        LoRaRadio* lora, SmartKnob* knob, AudioEngine* audio, AppState& state,
        AndroidMirror* android_mirror, bool* android_overlay,
        OverlayConfig* pip_cfg1, OverlayConfig* pip_cfg2, OverlayConfig* pip_cfg3,
        bool* pip_cam1_overlay, bool* pip_cam2_overlay, bool* pip_cam3_overlay,
        OverlayConfig* android_cfg,
        HudColors* hud_col, HudConfig* hud_cfg, MenuSystem** menu_sys_pp,
        Mpu9250* mpu9250,
        const std::vector<std::string>& gif_names,
        BtMonitor* bt_mon,
        bool* sys_panel_active,
        bool* fps_overlay_active,
        AppState* state_ptr,
        // Face Source switching — pass null fp_option to hide Protoface entry
        IFaceController** active_face_pp  = nullptr,
        IFaceController*  teensy_option   = nullptr,
        IFaceController*  fp_option       = nullptr,
        // Panel preview toggle (Protoface shm → ProtoHUD ImGui window)
        bool*             panel_preview_pp = nullptr,
        std::string       map_dir         = "/home/user/Pictures/protohud/maps")
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

    auto face_picker = [](std::string lbl, int face_count,
                           std::function<int()>     get_fn,
                           std::function<void(int)> set_fn) -> MenuItem {
        MenuItem m;
        m.label                   = std::move(lbl);
        m.type                    = MenuItemType::FACE_PICKER;
        m.face_picker.face_count  = face_count;
        m.face_picker.get_face    = std::move(get_fn);
        m.face_picker.set_face    = std::move(set_fn);
        return m;
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

    // ── GIFs ─────────────────────────────────────────────────────────────────
    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) {
        std::string lbl = (i < gif_names.size() && !gif_names[i].empty())
                          ? gif_names[i]
                          : "GIF #" + std::to_string(i);
        gifs.push_back(leaf(lbl, [teensy, i]{ teensy->play_gif(i); }));
    }

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
        toggle("Night Vision",
            [&state]{ return state.night_vision.nv_enabled; },
            [&state](bool v){
                state.night_vision.nv_enabled  = v;
                state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
                state.night_vision.shutter_us  = v ? 40000 : 16667;
            }),
        toggle("Auto Night Vision",
            [&state]{ return state.night_vision.auto_nv; },
            [&state](bool v){ state.night_vision.auto_nv = v; }),
        slider("Auto NV Gain Threshold", 1.5f, 16.f, 0.5f, "x",
            [&state]{ return state.night_vision.auto_nv_gain_threshold; },
            [&state](float v){ state.night_vision.auto_nv_gain_threshold = v; }),
        slider("Exposure (EV)", -3.f, 3.f, 0.5f, " EV",
            [&state]{ return state.night_vision.exposure_ev; },
            [&state](float v){ state.night_vision.exposure_ev = v; }),
        submenu("Shutter Speed", std::move(shutter_speeds)),
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

        return std::vector<MenuItem>{
            submenu("Focus",          std::move(fm)),
            submenu("White Balance",  std::move(wbm)),
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
        zoom_level_menu.push_back(leaf_sel(
            z.label,
            [&state, zoom = z.zoom]{
                state.zoom_left.zoom  = zoom;
                state.zoom_right.zoom = zoom;
            },
            [&state, zoom = z.zoom]{ return state.zoom_left.zoom == zoom; }
        ));
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
        crop_center_menu.push_back(leaf_sel(
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
        ));
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

    std::vector<MenuItem> main_cameras_menu = {
        toggle("Theater Mode",
            [&state]{ return state.theater_mode; },
            [&state](bool v){ state.theater_mode = v; }),
        submenu("Theater Position", std::move(theater_pos_menu)),
        toggle("Swap Cameras",
            [&state]{ return state.cameras_swapped; },
            [&state](bool v){ state.cameras_swapped = v; }),
        submenu("Resolution",    std::move(resolution_presets)),
        submenu("Digital Zoom",  std::move(zoom_menu)),
        submenu("Mirror Crop",   std::move(mirror_crop_menu)),
        submenu("Single Camera", std::move(single_cam_menu)),
        submenu("Left Camera",   std::move(left_cam_menu)),
        submenu("Right Camera",  std::move(right_cam_menu)),
        submenu("Night Vision",  std::move(nv_menu)),
        submenu("Autofocus Both", std::move(af_both_menu)),
        submenu("Capture Photo", std::move(capture_menu)),
        submenu("QR Scan",       std::move(qr_menu)),
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

    // Snap position presets — sets anchor_x/y and resets any pan offset.
    auto make_position_items = [&leaf, &leaf_sel, &submenu](OverlayConfig* cfg) {
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

    auto make_size_slider = [&slider](std::string lbl, OverlayConfig* cfg) -> MenuItem {
        return slider(std::move(lbl), 15.f, 60.f, 5.f, " %",
            [cfg]{ return cfg->size * 100.f; },
            [cfg](float v){ cfg->size = v / 100.f; });
    };

    auto make_rotation_items = [&leaf_sel](OverlayConfig* cfg) {
        using R = OverlayConfig::Rotation;
        return std::vector<MenuItem>{
            leaf_sel("Landscape",         [cfg]{ cfg->rotation = R::Landscape;        }, [cfg]{ return cfg->rotation == R::Landscape;        }),
            leaf_sel("Portrait",          [cfg]{ cfg->rotation = R::Portrait;          }, [cfg]{ return cfg->rotation == R::Portrait;          }),
            leaf_sel("Landscape Flipped", [cfg]{ cfg->rotation = R::LandscapeFlipped; }, [cfg]{ return cfg->rotation == R::LandscapeFlipped; }),
            leaf_sel("Portrait Flipped",  [cfg]{ cfg->rotation = R::PortraitFlipped;  }, [cfg]{ return cfg->rotation == R::PortraitFlipped;  }),
        };
    };

    // ── USB camera menus ──────────────────────────────────────────────────────
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

    std::vector<MenuItem> cam1_overlay_menu = {
        toggle("Show Overlay",
            [pip_cam1_overlay]{ return *pip_cam1_overlay; },
            [pip_cam1_overlay](bool v){ *pip_cam1_overlay = v; }),
        submenu("Position", make_position_items(pip_cfg1)),
        make_size_slider("Size", pip_cfg1),
        submenu("Rotation", make_rotation_items(pip_cfg1)),
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
            [pip_cam1_overlay](bool v){ *pip_cam1_overlay = v; }),
        submenu("Select Device", make_dev_select(
            [cameras]{ return cameras ? cameras->usb1_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb1(p); })),
        toggle("Auto Brightness",
            [cameras]{ return cameras && cameras->usb1_cfg().auto_brightness; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.auto_brightness = v;
                cameras->update_usb1_cfg(c);
            }),
        slider("Brightness Target", 40.f, 220.f, 5.f, "",
            [cameras]{ return cameras ? cameras->usb1_cfg().auto_brightness_target : 100.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb1_cfg(); c.auto_brightness_target = v;
                cameras->update_usb1_cfg(c);
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
        submenu("Rotation", make_rotation_items(pip_cfg2)),
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
            [pip_cam2_overlay](bool v){ *pip_cam2_overlay = v; }),
        submenu("Select Device", make_dev_select(
            [cameras]{ return cameras ? cameras->usb2_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb2(p); })),
        toggle("Auto Brightness",
            [cameras]{ return cameras && cameras->usb2_cfg().auto_brightness; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.auto_brightness = v;
                cameras->update_usb2_cfg(c);
            }),
        slider("Brightness Target", 40.f, 220.f, 5.f, "",
            [cameras]{ return cameras ? cameras->usb2_cfg().auto_brightness_target : 100.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb2_cfg(); c.auto_brightness_target = v;
                cameras->update_usb2_cfg(c);
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

    std::vector<MenuItem> cam3_overlay_menu = {
        toggle("Show Overlay",
            [pip_cam3_overlay]{ return *pip_cam3_overlay; },
            [pip_cam3_overlay](bool v){ *pip_cam3_overlay = v; }),
        submenu("Position", make_position_items(pip_cfg3)),
        make_size_slider("Size", pip_cfg3),
        submenu("Rotation", make_rotation_items(pip_cfg3)),
    };
    std::vector<MenuItem> usb3_brightness_menu = {
        leaf_sel("50%",  [cameras]{ if (cameras) cameras->set_usb3_brightness(0.5f); }, [cameras]{ return cameras && cameras->usb3_brightness() == 0.5f; }),
        leaf_sel("100%", [cameras]{ if (cameras) cameras->set_usb3_brightness(1.0f); }, [cameras]{ return cameras && cameras->usb3_brightness() == 1.0f; }),
        leaf_sel("150%", [cameras]{ if (cameras) cameras->set_usb3_brightness(1.5f); }, [cameras]{ return cameras && cameras->usb3_brightness() == 1.5f; }),
        leaf_sel("200%", [cameras]{ if (cameras) cameras->set_usb3_brightness(2.0f); }, [cameras]{ return cameras && cameras->usb3_brightness() == 2.0f; }),
        leaf_sel("300%", [cameras]{ if (cameras) cameras->set_usb3_brightness(3.0f); }, [cameras]{ return cameras && cameras->usb3_brightness() == 3.0f; }),
    };
    std::vector<MenuItem> usb3_exposure_menu = {
        toggle("Auto Exposure",
            [cameras]{ return !cameras || cameras->usb3_cfg().auto_exposure; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.auto_exposure = v;
                cameras->update_usb3_cfg(c);
                cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_AUTO, v ? 3 : 1);
            }),
        slider("Exposure Time", 1.f, 5000.f, 10.f, "",
            [cameras]{ return cameras ? (float)cameras->usb3_cfg().exposure_time : 157.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.exposure_time = (int)v;
                cameras->update_usb3_cfg(c);
                cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, (int)v);
            }),
        toggle("Auto White Balance",
            [cameras]{ return !cameras || cameras->usb3_cfg().auto_wb; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.auto_wb = v;
                cameras->update_usb3_cfg(c);
                cameras->set_usb3_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, v ? 1 : 0);
            }),
        slider("WB Temperature", 2800.f, 6500.f, 100.f, "K",
            [cameras]{ return cameras ? (float)cameras->usb3_cfg().wb_temp : 4600.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.wb_temp = (int)v;
                cameras->update_usb3_cfg(c);
                cameras->set_usb3_ctrl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, (int)v);
            }),
        toggle("Dynamic Framerate",
            [cameras]{ return cameras && cameras->usb3_cfg().dynamic_framerate; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.dynamic_framerate = v;
                cameras->update_usb3_cfg(c);
                cameras->set_usb3_ctrl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, v ? 1 : 0);
            }),
    };
    std::vector<MenuItem> usb_cam3_menu = {
        toggle("Open Stream",
            [cameras]{ return cameras && cameras->usb3_ok(); },
            [pip_cam3_overlay](bool v){ *pip_cam3_overlay = v; }),
        submenu("Select Device", make_dev_select(
            [cameras]{ return cameras ? cameras->usb3_cfg().device : ""; },
            [cameras](const std::string& p){ if (cameras) cameras->reassign_usb3(p); })),
        toggle("Auto Brightness",
            [cameras]{ return cameras && cameras->usb3_cfg().auto_brightness; },
            [cameras](bool v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.auto_brightness = v;
                cameras->update_usb3_cfg(c);
            }),
        slider("Brightness Target", 40.f, 220.f, 5.f, "",
            [cameras]{ return cameras ? cameras->usb3_cfg().auto_brightness_target : 100.f; },
            [cameras](float v){
                if (!cameras) return;
                UsbCamConfig c = cameras->usb3_cfg(); c.auto_brightness_target = v;
                cameras->update_usb3_cfg(c);
            }),
        submenu("Overlay",     std::move(cam3_overlay_menu)),
        submenu("Brightness",  std::move(usb3_brightness_menu)),
        submenu("Exposure",    std::move(usb3_exposure_menu)),
        leaf("Scan for Camera", [cameras, &state]{
            if (cameras) {
                bool ok = cameras->scan_usb3();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.health.cam_usb3 = ok;
            }
        }),
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
        leaf("Scan for Cameras", [cameras, &state]{
            if (!cameras) return;
            bool ok1 = cameras->scan_usb1();
            bool ok2 = cameras->scan_usb2();
            bool ok3 = cameras->scan_usb3();
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.cam_usb1 = ok1;
            state.health.cam_usb2 = ok2;
            state.health.cam_usb3 = ok3;
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

    // ── Face Source (Teensy vs Protoface) ────────────────────────────────────
    std::vector<MenuItem> face_source_menu;
    if (active_face_pp && teensy_option && fp_option) {
        face_source_menu.push_back(leaf_sel("Teensy (ProtoTracer)",
            [active_face_pp, teensy_option]{ *active_face_pp = teensy_option; },
            [active_face_pp, teensy_option]{ return *active_face_pp == teensy_option; }
        ));
        face_source_menu.push_back(leaf_sel("Protoface",
            [active_face_pp, fp_option]{ *active_face_pp = fp_option; },
            [active_face_pp, fp_option]{ return *active_face_pp == fp_option; }
        ));
    }

    // ── Prototracer (face controller) submenu ─────────────────────────────────
    std::vector<MenuItem> prototracer_menu = {
        submenu("Faces",      std::move(effects)),
        submenu("Color",          std::move(colors)),
        submenu("Material Color", std::move(proto_colors)),
        submenu("Animations",     std::move(gifs)),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
        slider("Lens Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
        leaf("Release Control", [teensy]{ teensy->release_control(); }),
        face_picker("Face", 10,
            [&state]{ return static_cast<int>(state.face.face_index); },
            [&state, teensy](int v){
                teensy->set_menu_item(0, static_cast<uint8_t>(v));
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.face_index = static_cast<uint8_t>(v);
            }),
        slider("Accent Bright", 0.f, 10.f, 1.f, "",
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
        toggle("Boop Sensor",
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
    if (!face_source_menu.empty())
        prototracer_menu.push_back(submenu("Face Source", std::move(face_source_menu)));
    if (panel_preview_pp)
        prototracer_menu.push_back(toggle("Panel Preview",
            [panel_preview_pp]{ return *panel_preview_pp; },
            [panel_preview_pp](bool v){ *panel_preview_pp = v; }));

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

    std::vector<MenuItem> compass_menu = {
        submenu("IMU Axis",            std::move(imu_axis_menu)),
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

    std::vector<MenuItem> themes_menu = {
        leaf("Halo", [apply_theme]{
            apply_theme(IM_COL32(255,255,255,255), IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,255,255,255), IM_COL32(255,255,255,180),
                        true, 5.f, SelectionStyle::FILLED_ROW,
                        false, true,
                        IM_COL32(255,255,255,255));
        }),
        leaf("Solar", [apply_theme]{
            apply_theme(IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        IM_COL32(255,255,255,255),
                        IM_COL32(255,160, 32,255), IM_COL32(255,160, 32,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
                        IM_COL32(255,160, 32,255));
        }),
        leaf("Fallout", [apply_theme]{
            apply_theme(IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        IM_COL32(  0,255, 80,255),
                        IM_COL32(  0,200, 50,255), IM_COL32(  0,200, 50,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
                        IM_COL32(  0,200, 50,255));
        }),
        leaf("Space", [apply_theme]{
            apply_theme(IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        IM_COL32(200,220,255,255),
                        IM_COL32( 80,100,255,255), IM_COL32( 80,100,255,255),
                        true, 1.5f, SelectionStyle::ACCENT_BAR,
                        true, false,
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
        leaf_sel("Small  (200px)", [&state]{ state.map_overlay.size_px = 200.f; }, [&state]{ return state.map_overlay.size_px == 200.f; }),
        leaf_sel("Medium (300px)", [&state]{ state.map_overlay.size_px = 300.f; }, [&state]{ return state.map_overlay.size_px == 300.f; }),
        leaf_sel("Large  (450px)", [&state]{ state.map_overlay.size_px = 450.f; }, [&state]{ return state.map_overlay.size_px == 450.f; }),
        leaf_sel("Full   (600px)", [&state]{ state.map_overlay.size_px = 600.f; }, [&state]{ return state.map_overlay.size_px == 600.f; }),
    };

    std::vector<MenuItem> map_overlay_menu = {
        toggle("Enabled",
            [&state]{ return state.map_overlay.enabled; },
            [&state](bool v){ state.map_overlay.enabled = v; }),
        submenu("Select Map",           std::move(map_select_items)),
        submenu("Move Map",             std::move(map_move_menu)),
        submenu("Rotate Image",         std::move(map_rotate_menu)),
        submenu("Size",                 std::move(map_size_menu)),
        slider("Map Zoom", 1.f, 4.f, 0.1f, "x",
            [&state]{ return state.map_overlay.zoom; },
            [&state](float v){ state.map_overlay.zoom = v; }),
        toggle("Rotate with Heading",
            [&state]{ return state.map_overlay.rotate_with_heading; },
            [&state](bool v){ state.map_overlay.rotate_with_heading = v; }),
        leaf("Set My Direction", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.map_north_deg = state.compass_heading;
            state.map_overlay.calibrated    = true;
        }),
        toggle("Circle Window",
            [&state]{ return state.map_overlay.circle_window; },
            [&state](bool v){ state.map_overlay.circle_window = v; }),
        slider("Transparency", 0.f, 1.f, 0.05f, "",
            [&state]{ return state.map_overlay.opacity; },
            [&state](float v){ state.map_overlay.opacity = v; }),
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
        submenu("Map Overlay",   std::move(map_overlay_menu)),
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

    std::vector<MenuItem> software_menu = {
        leaf("Check for Updates", []{
            system("git -C /home/user/ProtoHUD fetch origin main 2>&1 | logger -t protohud &");
        }),
        leaf("Pull && Rebuild", []{
            system("cd /home/user/ProtoHUD && git pull origin main && ./scripts/build.sh 2>&1 | logger -t protohud &");
        }),
    };

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

    std::vector<MenuItem> system_menu = {
        submenu("Headset",          std::move(headset_menu)),
        submenu("Audio",            std::move(audio_menu)),
        submenu("Timers and Alarm", std::move(timers_alarm_menu)),
        toggle("System Panel",
            [sys_panel_active]{ return sys_panel_active && *sys_panel_active; },
            [sys_panel_active](bool v){ if (sys_panel_active) *sys_panel_active = v; }),
        toggle("FPS Overlay",
            [fps_overlay_active]{ return fps_overlay_active && *fps_overlay_active; },
            [fps_overlay_active](bool v){ if (fps_overlay_active) *fps_overlay_active = v; }),
        submenu("FPS Average",   std::move(fps_interval_menu)),
        submenu("Diagnostics",   std::move(diagnostics_menu)),
        toggle("SSH Access",
            [state_ptr]{ return state_ptr && state_ptr->ssh.active; },
            [state_ptr](bool v){
                system(v ? "systemctl start ssh 2>&1 | logger -t protohud &"
                         : "systemctl stop ssh 2>&1 | logger -t protohud &");
                if (state_ptr) {
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->ssh.active = v;
                }
            }),
        leaf("Refresh Bluetooth", [bt_mon]{ if (bt_mon) bt_mon->refresh(); }),
        submenu("Software",   std::move(software_menu)),
        submenu("Demo Mode",  std::move(demo_menu)),
        leaf("Request Status", [teensy]{ teensy->request_status(); }),
        leaf("Close Program",  [&state]{ state.quit = true; }),
    };

    return {
        submenu("Vision",       std::move(cameras_menu)),
        submenu("HUD",          std::move(hud_menu)),
        submenu("Face Display", std::move(prototracer_menu)),
        submenu("LoRa",         std::move(lora_menu)),
        submenu("System",       std::move(system_menu)),
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
    //   1. <bin_dir>/../config/config.json         — user's live config (gitignored,
    //                                                written on exit with current settings)
    //   2. <bin_dir>/../config/config.example.json — tracked defaults; used on a fresh
    //                                                clone before any run has written a
    //                                                live config. The app writes its
    //                                                settings to config.json on exit, so
    //                                                the example is only read once.
    //   3. <bin_dir>/config.json                   — installed/packaged layout fallback.
    std::string cfg_path;   // write path
    std::string cfg_load;   // read path (may differ from cfg_path on first run)
    if (argc > 1) {
        cfg_path = cfg_load = argv[1];
    } else {
        std::string dev_cfg     = bin_dir + "/../config/config.json";
        std::string example_cfg = bin_dir + "/../config/config.example.json";
        std::string def_cfg     = res("config.json");
        try {
            if (fs::exists(dev_cfg)) {
                cfg_path = cfg_load = dev_cfg;
            } else if (fs::exists(example_cfg)) {
                cfg_load = example_cfg;  // read defaults from example
                cfg_path = dev_cfg;      // write user settings to config.json
                std::cout << "[cfg] first run — loading defaults from config.example.json, "
                             "will write to config.json\n";
            } else {
                cfg_path = cfg_load = def_cfg;
            }
        } catch (...) {
            cfg_path = cfg_load = def_cfg;
        }
    }
    json cfg = load_config(cfg_load);

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
        auto& jl              = jcam["owlsight_left"];
        owl_left.libcamera_id = jl.value("libcamera_id", 0);
        owl_left.model_name   = jl.value("model_name",   std::string(""));
        owl_left.width        = jl.value("width",  1280);
        owl_left.height       = jl.value("height",  800);
        owl_left.fps          = jl.value("fps",      60);
    }
    if (jcam.contains("owlsight_right")) {
        auto& jr               = jcam["owlsight_right"];
        owl_right.libcamera_id = jr.value("libcamera_id", 1);
        owl_right.model_name   = jr.value("model_name",   std::string(""));
        owl_right.width        = jr.value("width",  1280);
        owl_right.height       = jr.value("height",  800);
        owl_right.fps          = jr.value("fps",      60);
    }
    // Override both eyes from the persisted resolution section (set by in-app preset menu)
    if (cfg.contains("resolution")) {
        const auto& jres = cfg["resolution"];
        int w   = jres.value("width",  owl_left.width);
        int h   = jres.value("height", owl_left.height);
        int fps = jres.value("fps",    owl_left.fps);
        owl_left.width   = owl_right.width   = w;
        owl_left.height  = owl_right.height  = h;
        owl_left.fps     = owl_right.fps     = fps;
    }

    UsbCamConfig usb1_cfg, usb2_cfg, usb3_cfg;
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
        usb1_cfg.flip                    = j1.value("flip",                    false);
        usb1_cfg.auto_brightness         = j1.value("auto_brightness",         false);
        usb1_cfg.auto_brightness_target  = j1.value("auto_brightness_target",  100.f);
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
        usb2_cfg.flip                    = j2.value("flip",                    false);
        usb2_cfg.auto_brightness         = j2.value("auto_brightness",         false);
        usb2_cfg.auto_brightness_target  = j2.value("auto_brightness_target",  100.f);
    }
    if (jcam.contains("usb_cam_3")) {
        auto& j3 = jcam["usb_cam_3"];
        usb3_cfg.device             = j3.value("device",             "");
        usb3_cfg.width              = j3.value("width",               1280);
        usb3_cfg.height             = j3.value("height",               720);
        usb3_cfg.fps                = j3.value("fps",                   30);
        usb3_cfg.brightness         = j3.value("brightness",           1.0f);
        usb3_cfg.dynamic_framerate  = j3.value("dynamic_framerate",    false);
        usb3_cfg.auto_exposure      = j3.value("auto_exposure",        true);
        usb3_cfg.exposure_time      = j3.value("exposure_time",        157);
        usb3_cfg.auto_wb            = j3.value("auto_wb",              true);
        usb3_cfg.wb_temp            = j3.value("wb_temp",              4600);
        usb3_cfg.flip                    = j3.value("flip",                    false);
        usb3_cfg.auto_brightness         = j3.value("auto_brightness",         false);
        usb3_cfg.auto_brightness_target  = j3.value("auto_brightness_target",  100.f);
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
        mpu_cfg.mount_rotation  = jval(jm, "mount_rotation",  0);
        mpu_cfg.heading_axes    = jval(jm, "heading_axes",    0);
        if (jm.contains("mag_bias") && jm["mag_bias"].is_array() &&
            jm["mag_bias"].size() >= 3) {
            mpu_cfg.mag_bias_x = jm["mag_bias"][0].get<float>();
            mpu_cfg.mag_bias_y = jm["mag_bias"][1].get<float>();
            mpu_cfg.mag_bias_z = jm["mag_bias"][2].get<float>();
        }
    }

    AndroidMirrorConfig and_cfg;
    OverlayConfig       pip_overlay_cfg1, pip_overlay_cfg2, pip_overlay_cfg3;
    OverlayConfig       android_overlay_cfg;

    if (cfg.contains("pip")) {
        auto& jpip = cfg["pip"];
        float def_size = jval(jpip, "size", 0.25f);

        auto load_pip_cfg = [&](OverlayConfig& ocfg, const json& jc,
                                float def_ax, float def_ay) {
            ocfg.size     = jval(jc, "size", def_size);
            ocfg.rotation = parse_rotation(jc.value("rotation", std::string("landscape")));
            if (jc.contains("anchor_x")) {
                ocfg.anchor_x = jval(jc, "anchor_x", def_ax);
                ocfg.anchor_y = jval(jc, "anchor_y", def_ay);
            } else if (jc.contains("anchor")) {
                apply_legacy_anchor(jc["anchor"].get<std::string>(), ocfg);
            } else {
                ocfg.anchor_x = def_ax; ocfg.anchor_y = def_ay;
            }
            ocfg.pan_x = jval(jc, "pan_x", 0.f);
            ocfg.pan_y = jval(jc, "pan_y", 0.f);
        };

        auto& jc1 = jpip.contains("cam1") ? jpip["cam1"] : jpip;
        load_pip_cfg(pip_overlay_cfg1, jc1, 0.0f, 0.0f);   // default: top-left
        auto& jc2 = jpip.contains("cam2") ? jpip["cam2"] : jpip;
        load_pip_cfg(pip_overlay_cfg2, jc2, 1.0f, 0.0f);   // default: top-right
    }

    if (cfg.contains("android")) {
        auto& jand = cfg["android"];
        and_cfg.enabled    = jval(jand, "enabled",   false);
        and_cfg.v4l2_sink  = jand.value("v4l2_sink", std::string("/dev/video4"));
        and_cfg.adb_serial = jand.value("adb_serial",std::string(""));
        and_cfg.max_size   = jval(jand, "max_size",   1080);
        and_cfg.fps        = jval(jand, "fps",         30);
        android_overlay_cfg.size = jval(jand, "overlay_size", 0.40f);
        if (jand.contains("anchor_x")) {
            android_overlay_cfg.anchor_x = jval(jand, "anchor_x", 0.0f);
            android_overlay_cfg.anchor_y = jval(jand, "anchor_y", 1.0f);
        } else {
            apply_legacy_anchor(jand.value("anchor", std::string("bottom_left")),
                                android_overlay_cfg);
        }
        android_overlay_cfg.pan_x = jval(jand, "pan_x", 0.f);
        android_overlay_cfg.pan_y = jval(jand, "pan_y", 0.f);
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
        state.night_vision.exposure_ev            = jnv.value("exposure_ev",             0.0f);
        state.night_vision.shutter_us             = jnv.value("shutter_us",              33333);
        state.night_vision.auto_nv                = jnv.value("auto_nv",                 false);
        state.night_vision.auto_nv_gain_threshold = jnv.value("auto_nv_gain_threshold",  4.0f);
        state.night_vision.csi_awb_left           = jnv.value("csi_awb_left",            true);
        state.night_vision.csi_awb_right          = jnv.value("csi_awb_right",           true);
    }

    state.cameras_swapped = jcam.value("swapped", false);
    {
        using TA = AppState::TheaterAnchor;
        static const std::pair<const char*, TA> kAnchors[] = {
            {"center",  TA::Center},  {"outside", TA::Outside},
            {"left",    TA::Left},    {"right",   TA::Right},
            {"top",     TA::Top},     {"bottom",  TA::Bottom},
            // legacy values → nearest equivalent
            {"top_left", TA::Outside}, {"top_right", TA::Outside},
            {"bottom_left", TA::Outside}, {"bottom_right", TA::Outside},
        };
        std::string a = jcam.value("theater_anchor", "center");
        for (auto& [k, v] : kAnchors) if (a == k) { state.theater_anchor = v; break; }
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
        state.pp_cfg.color_protect   = jpp.value("color_protect",   0.5f);
        state.pp_cfg.edge_dilate     = jpp.value("edge_dilate",     1.0f);
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

    // System monitor / network config
    std::string cfg_ping_host  = "8.8.8.8";
    std::string cfg_wifi_iface = "wlan0";
    std::string cfg_crash_dir  = "/tmp";
    const char* home_env = getenv("HOME");
    std::string home_dir = home_env ? home_env : "/home/user";
    std::string cfg_photo_dir  = home_dir + "/Pictures/protohud";
    std::string cfg_map_dir    = home_dir + "/Pictures/protohud/maps";
    int         cfg_ssh_port   = 22;
    if (cfg.contains("system")) {
        auto& js         = cfg["system"];
        cfg_ping_host    = js.value("ping_host",  cfg_ping_host);
        cfg_wifi_iface   = js.value("wifi_iface", cfg_wifi_iface);
        cfg_crash_dir    = js.value("crash_dir",  cfg_crash_dir);
        { auto v = js.value("photo_dir", std::string{}); if (!v.empty()) cfg_photo_dir = v; }
        { auto v = js.value("map_dir",   std::string{}); if (!v.empty()) cfg_map_dir   = v; }
        cfg_ssh_port     = js.value("ssh_port",   cfg_ssh_port);
    }
    // Ensure the maps directory exists
    { std::error_code ec; std::filesystem::create_directories(cfg_map_dir, ec); }
    state.ssh.port = cfg_ssh_port;

    // Map overlay persistent settings
    if (cfg.contains("map")) {
        const auto& jm = cfg["map"];
        auto& mo = state.map_overlay;
        mo.enabled             = jm.value("enabled",             mo.enabled);
        mo.opacity             = jm.value("opacity",             mo.opacity);
        mo.size_px             = jm.value("size_px",             mo.size_px);
        mo.rotate_with_heading = jm.value("rotate_with_heading", mo.rotate_with_heading);
        mo.image_rotate_deg    = jm.value("image_rotate_deg",    mo.image_rotate_deg);
        mo.anchor_x            = jm.value("anchor_x",            mo.anchor_x);
        mo.anchor_y            = jm.value("anchor_y",            mo.anchor_y);
        mo.circle_window       = jm.value("circle_window",       mo.circle_window);
        mo.zoom                = jm.value("zoom",                mo.zoom);
        { auto v = jm.value("map_path", std::string{}); if (!v.empty()) mo.map_path = v; }
    }

    // Compass IMU axis selection
    if (cfg.contains("compass")) {
        auto& jc = cfg["compass"];
        std::string axis = jc.value("axis", std::string("roll"));
        if      (axis == "pitch") state.compass_axis = AppState::CompassAxis::Pitch;
        else if (axis == "yaw")   state.compass_axis = AppState::CompassAxis::Yaw;
        else                      state.compass_axis = AppState::CompassAxis::Roll;
        state.compass_invert = jc.value("invert", false);
    }

    // QR scan persistent settings
    if (cfg.contains("qr")) {
        state.qr_scan_main = cfg["qr"].value("scan_main", state.qr_scan_main);
        state.qr_scan_usb  = cfg["qr"].value("scan_usb",  state.qr_scan_usb);
    }

    // FPS average interval
    state.fps_avg_interval_s = jval(cfg, "fps_avg_interval_s", 1);

    // I2C scanner bus
    state.i2c_scan_bus = cfg.value("i2c_scan_bus", std::string("/dev/i2c-1"));

    // GPIO monitor pins (array of integers in config)
    if (cfg.contains("gpio_monitor_pins") && cfg["gpio_monitor_pins"].is_array()) {
        for (auto& p : cfg["gpio_monitor_pins"])
            state.gpio_states.push_back({ p.get<int>(), -1 });
    }

    SplashConfig splash_cfg;
    if (cfg.contains("splash")) {
        auto& js             = cfg["splash"];
        splash_cfg.enabled       = js.value("enabled",       splash_cfg.enabled);
        splash_cfg.animated      = js.value("animated",      splash_cfg.animated);
        splash_cfg.image_path    = js.value("image",         splash_cfg.image_path);
        splash_cfg.min_display_s = js.value("min_display_s", splash_cfg.min_display_s);
        splash_cfg.title         = js.value("title",         splash_cfg.title);
        splash_cfg.subtitle      = js.value("subtitle",      splash_cfg.subtitle);
    }
    // Resolve relative image path against the binary directory.
    if (!splash_cfg.image_path.empty() && splash_cfg.image_path[0] != '/')
        splash_cfg.image_path = res(splash_cfg.image_path);

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
        std::lock_guard<std::mutex> lk(state.mtx);
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
        if (!xr_fresh) {
            // Circular low-pass filter — operate on sin/cos to handle 0/360 wrap.
            // alpha=0.1 at 50 Hz → ~0.19 s time constant; raise to smooth more.
            constexpr float kAlpha = 0.1f;
            constexpr float kDeg2Rad = 3.14159265f / 180.f;
            float prev = state.compass_heading;
            float fs = std::sinf(prev * kDeg2Rad) + kAlpha * (std::sinf(heading * kDeg2Rad) - std::sinf(prev * kDeg2Rad));
            float fc = std::cosf(prev * kDeg2Rad) + kAlpha * (std::cosf(heading * kDeg2Rad) - std::cosf(prev * kDeg2Rad));
            float filtered = std::atan2f(fs, fc) / kDeg2Rad;
            if (filtered < 0.f) filtered += 360.f;
            state.compass_heading = filtered;
        }
    });

    if (!mpu9250.start() && mpu_cfg.enabled)
        std::cerr << "[main] MPU-9250 backup compass unavailable\n";

    // ── Dev/debug monitors ────────────────────────────────────────────────────

    SystemMonitor sys_mon;
    WifiMonitor   wifi_mon;
    PingMonitor   ping_mon;
    BtMonitor     bt_mon;

    sys_mon.start(&state);
    wifi_mon.start(&state, cfg_wifi_iface);
    ping_mon.start(&state, cfg_ping_host);
    bt_mon.start(&state);
    CrashReporter::install(&state, cfg_crash_dir, GIT_HASH);

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

    // ── Splash screen ─────────────────────────────────────────────────────────
    // Render a splash frame between heavy init steps so the display isn't black
    // while cameras, serial ports, and the menu system initialise.

    SplashScreen splash(splash_cfg);
    if (!splash_cfg.image_path.empty())
        splash.load_image(splash_cfg.image_path);

    double splash_t0 = glfwGetTime();

    auto splash_frame = [&](const char* status, float progress) {
        if (!splash_cfg.enabled) return;
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(xr.glfw_window(), &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        hud.set_dt(0.016f);
        hud.begin_menu_frame();
        float t = static_cast<float>(glfwGetTime() - splash_t0);
        splash.draw(ImGui::GetBackgroundDrawList(),
                    static_cast<float>(fw), static_cast<float>(fh),
                    t, status, progress);
        hud.render_menu_overlay();
        xr.present();
    };

    splash_frame("Initializing cameras...", 0.20f);

    // ── Camera manager ────────────────────────────────────────────────────────

    CameraManager cameras;
    cameras.init(owl_left, owl_right, usb1_cfg, usb2_cfg, usb3_cfg,
                 res("assets/shaders/nv12.vs").c_str(),
                 res("assets/shaders/nv12.fs").c_str());
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.health.cam_owl_left  = cameras.owl_left_ok();
        state.health.cam_owl_right = cameras.owl_right_ok();
        state.health.cam_usb1      = cameras.usb1_ok();
        state.health.cam_usb2      = cameras.usb2_ok();
        state.health.cam_usb3      = cameras.usb3_ok();
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

    splash_frame("Connecting serial devices...", 0.50f);

    // ── Serial devices ────────────────────────────────────────────────────────

    std::string teensy_port    = "/dev/ttyACM0";
    std::string lora_port      = "/dev/ttyUSB0";
    std::string knob_port      = "/dev/ttyACM1";
    std::string wireless_port  = "/dev/ttyACM3";
    bool        wireless_enabled = false;
    int         baud           = 115200;

    if (jser.contains("teensy"))    teensy_port = jser["teensy"].value("port",    teensy_port);
    if (jser.contains("lora"))      lora_port   = jser["lora"].value("port",      lora_port);
    if (jser.contains("smartknob")) knob_port   = jser["smartknob"].value("port", knob_port);
    if (jser.contains("wireless_controller")) {
        const auto& jwc = jser["wireless_controller"];
        wireless_port    = jwc.value("port",    wireless_port);
        wireless_enabled = jwc.value("enabled", false);
    }

    // Auto-discover serial ports by USB VID:PID when configured paths are absent.
    // Teensy 4.1: VID=0x16C0, PID=0x0483.  LoRa CH340: VID=0x1A86, PID=0x7523.
    // SmartKnob (ESP32-S3 dev board): scan ttyACM without VID filter (varies by board).
    teensy_port = resolve_serial_port(teensy_port, 0x16C0, 0x0483);
    lora_port   = resolve_serial_port(lora_port,   0x1A86, 0x7523);
    knob_port   = resolve_serial_port(knob_port);

    TeensyController     teensy  (teensy_port,   baud, state);
    ProtoFaceController  protoface_ctrl;
    LoRaRadio            lora    (lora_port,     baud, state);
    SmartKnob            knob    (knob_port,     baud, state);
    WirelessController   wireless(wireless_port, baud);

    if (!teensy.start()) std::cerr << "[main] Teensy not available on " << teensy_port << "\n";
    protoface_ctrl.start();   // connects async; no-op if socket absent

    // QR / barcode scanner — active when either scan toggle is enabled.
    QrScanner qr_scanner;
    qr_scanner.set_callback([&state, cfg_photo_dir](const std::string& text,
                                                       const std::string& type) {
        // Honour mute window (set by "MUTE 1m" action)
        if (static_cast<int64_t>(time(nullptr)) < state.qr_mute_until_s.load()) return;

        const bool is_url = text.size() > 7 &&
                            (text.substr(0, 7) == "http://" ||
                             text.substr(0, 8) == "https://");

        Notification n;
        n.type           = NotifType::App;
        n.title          = type + " Detected";
        n.body           = text;
        n.auto_dismiss_s = 20.f;   // longer so user has time to act

        if (is_url) {
            n.actions.push_back({"OPEN", [text](AppState&) {
                // xdg-open in background; single-quote the URL to avoid expansion
                std::string safe = text;
                // Replace any single-quotes in the URL with %27 before shell-quoting
                for (auto& c : safe) if (c == '\'') c = ' ';
                std::string cmd = "xdg-open '" + safe + "' >/dev/null 2>&1 &";
                system(cmd.c_str());
            }});
        }

        n.actions.push_back({"SAVE", [text, cfg_photo_dir](AppState&) {
            if (cfg_photo_dir.empty()) return;
            time_t now = time(nullptr);
            struct tm tm{}; localtime_r(&now, &tm);
            char ts[20]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
            std::string path = cfg_photo_dir + "/qr_" + ts + ".txt";
            std::ofstream f(path);
            if (f) f << text << "\n";
        }});

        n.actions.push_back({"MUTE 1m", [](AppState& s) {
            s.qr_mute_until_s.store(static_cast<int64_t>(time(nullptr)) + 60);
        }});

        std::lock_guard lk(state.mtx);
        state.notifs.push(std::move(n));
    });
    cameras.set_qr_scanner(&qr_scanner);
    if (!lora.start())   std::cerr << "[main] LoRa not available on "   << lora_port   << "\n";
    if (!knob.start())   std::cerr << "[main] SmartKnob not available on " << knob_port << "\n";

    // Active face backend: prefer Protoface if its socket already exists at startup.
    IFaceController* active_face = ProtoFaceController::socket_exists()
        ? static_cast<IFaceController*>(&protoface_ctrl)
        : static_cast<IFaceController*>(&teensy);
    FaceProxy face_proxy(&active_face);

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

    splash_frame("Building menu...", 0.75f);

    // ── Menu system ───────────────────────────────────────────────────────────

    bool pip_cam1_overlay_active = false;
    bool pip_cam2_overlay_active = false;
    bool pip_cam3_overlay_active = false;
    bool sys_panel_active        = false;
    bool fps_overlay_active      = false;

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
    bool panel_preview_enabled = false;
    MenuSystem* menu_ptr = nullptr;
    MenuSystem menu(build_menu(&face_proxy, &xr, &cameras, &lora, &knob, &audio, state,
                               &android_mirror, &android_overlay_active,
                               &pip_overlay_cfg1, &pip_overlay_cfg2, &pip_overlay_cfg3,
                               &pip_cam1_overlay_active, &pip_cam2_overlay_active, &pip_cam3_overlay_active,
                               &android_overlay_cfg,
                               &hud.colors(), &hud.config(), &menu_ptr,
                               &mpu9250, gif_names,
                               &bt_mon, &sys_panel_active, &fps_overlay_active, &state,
                               &active_face,
                               static_cast<IFaceController*>(&teensy),
                               static_cast<IFaceController*>(&protoface_ctrl),
                               &panel_preview_enabled,
                               cfg_map_dir));
    menu_ptr = &menu;

    // Wire wireless controller callbacks now that menu exists
    if (wireless_enabled) {
        wireless.on_menu([&menu]{
            if (menu.is_open()) menu.close(); else menu.open();
        });
        wireless.on_select([&menu, &hud, &state]{
            if      (menu.is_open())          menu.select();
            else if (hud.toast_has_focused()) hud.toast_select(state);
        });
        wireless.on_back([&menu, &hud]{
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        });
        wireless.on_nav_up   ([&menu]{ if (menu.is_open()) menu.navigate(-1); });
        wireless.on_nav_down ([&menu]{ if (menu.is_open()) menu.navigate(+1); });
        wireless.on_nav_left ([&menu, &hud]{
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        });
        wireless.on_nav_right([&menu, &hud, &state]{
            if      (hud.toast_has_focused()) hud.toast_navigate(+1);
            else if (menu.is_open())          menu.select();
        });
        wireless.on_af([&cameras]{
            if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
            if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
        });
        wireless.on_capture([&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            state.capture_request = CaptureRequest::Stereo;
        });
        wireless.start();
    }

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
        // if (hud.popup_active())    hud.popup_navigate(dir);  // modal popup disabled
        if      (menu.is_open())        menu.navigate(dir);
        else if (hud.toast_has_focused()) hud.toast_navigate(dir);
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
    int af_trigger_ms      = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "af_trigger_time_ms",      1500);
    int pip_trigger_ms     = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "pip_trigger_time_ms",     2000);
    int capture_trigger_ms = jval(cfg.contains("gpio") ? cfg["gpio"] : empty, "capture_trigger_time_ms", 5000);

    GpioButtons buttons(button_1_gpio, button_2_gpio, button_3_gpio,
                        af_trigger_ms, pip_trigger_ms, capture_trigger_ms);
    bool pip_left_active  = false, pip_right_active  = false;  // GPIO-driven
    bool kb_pip_left      = false, kb_pip_right      = false;  // keyboard-driven

    // Edge-detection state for direct GLFW key polling
    bool prev_key[12] = {};  // slots: 1=K1 2=K2 3=K3 4=K4 5=, 6=. 7=K5 8=K6 9=K7

    // USB stream lifecycle: track previous combined pip-active state per slot
    bool prev_p1 = false, prev_p2 = false, prev_p3 = false;

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
            buttons.on_capture_left([&state]() {
                std::cout << "[gpio] capture left\n";
                std::lock_guard lk(state.mtx);
                state.capture_request = CaptureRequest::Left;
            });
            buttons.on_capture_right([&state]() {
                std::cout << "[gpio] capture right\n";
                std::lock_guard lk(state.mtx);
                state.capture_request = CaptureRequest::Right;
            });
            buttons.on_pip_left ([&pip_left_active] () { pip_left_active  = true; });
            buttons.on_pip_right([&pip_right_active]() { pip_right_active = true; });
            buttons.on_select   ([&menu, &hud, &state]() {
                // if (hud.popup_active()) hud.popup_select();  // modal popup disabled
                if      (menu.is_open())           menu.select();
                else if (hud.toast_has_focused())  hud.toast_select(state);
            });
        } else {
            std::cerr << "[main] GPIO button init failed\n";
        }
    }

    // ── Gamepad (SDL2, optional) ──────────────────────────────────────────────
    GamepadInput gamepad;
    gamepad.init();
    gamepad.on_menu([&menu]{
        if (menu.is_open()) menu.close(); else menu.open();
    });
    gamepad.on_select([&menu, &hud, &state]{
        if      (menu.is_open())          menu.select();
        else if (hud.toast_has_focused()) hud.toast_select(state);
    });
    gamepad.on_back([&menu, &hud]{
        if      (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_up   ([&menu]{ if (menu.is_open()) menu.navigate(-1); });
    gamepad.on_nav_down ([&menu]{ if (menu.is_open()) menu.navigate(+1); });
    gamepad.on_nav_left ([&menu, &hud]{
        if      (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_right([&menu, &hud, &state]{
        if      (hud.toast_has_focused()) hud.toast_navigate(+1);
        else if (menu.is_open())          menu.select();
    });
    gamepad.on_pip_left ([&kb_pip_left] { kb_pip_left  = !kb_pip_left;  });
    gamepad.on_pip_right([&kb_pip_right]{ kb_pip_right = !kb_pip_right; });
    gamepad.on_af([&cameras]{
        if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
        if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
    });
    gamepad.on_capture([&state]{
        std::lock_guard<std::mutex> lk(state.mtx);
        state.capture_request = CaptureRequest::Stereo;
    });

    // ── GL texture handles for camera sources ─────────────────────────────────

    GLuint tex_usb1  = 0;
    GLuint tex_usb2  = 0;
    GLuint tex_usb3  = 0;
    GLuint tex_beast = 0;

    teensy.request_status();

    // ── Splash hold ───────────────────────────────────────────────────────────
    // Keep splash visible until minimum display time elapses, then show "Ready"
    // for a brief moment so the animation completes cleanly.
    if (splash_cfg.enabled) {
        splash_frame("Ready", 1.0f);
        while (static_cast<float>(glfwGetTime() - splash_t0) < splash_cfg.min_display_s) {
            splash_frame("Ready", 1.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        // One extra frame with full bar so the glow pulse completes
        for (int i = 0; i < 6; ++i)
            splash_frame("Ready", 1.0f);
    }

    // ── Main render loop ──────────────────────────────────────────────────────

    KeyRepeat rep_nav_up, rep_nav_down, rep_toast_prev, rep_toast_next;

    // M long-press state: short tap = toggle map; hold 1.5 s = cycle next map
    double m_press_t    = -1.0;
    bool   m_long_fired = false;

    double prev_time = glfwGetTime();

    while (!glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {

        // ── Delta time ────────────────────────────────────────────────────────
        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - prev_time);
        prev_time  = now;

        // ── Start frame: tick HUD state + begin ImGui for input/menu ─────────
        hud.set_dt(dt);
        hud.begin_menu_frame();

        // ── Keyboard input (via ImGui, which owns GLFW callbacks) ─────────────
        if (key_pressed(ImGuiKey_Escape) || key_pressed(ImGuiKey_P)) { state.quit = true; break; }
        // Ctrl+Q / Ctrl+K — force-kill (immediate exit, skips graceful cleanup)
        if (ImGui::GetIO().KeyCtrl &&
            (key_pressed(ImGuiKey_Q) || key_pressed(ImGuiKey_K))) {
            std::exit(0);
        }
        if (key_pressed(ImGuiKey_I)) {
            if (menu.is_open()) menu.close();
            else                menu.open();
        }
        // Modal alarm/timer popup disabled — toasts handle notification display.
        // if (hud.popup_active()) {
        //     if (key_pressed(ImGuiKey_LeftArrow))  hud.popup_navigate(-1);
        //     if (key_pressed(ImGuiKey_RightArrow)) hud.popup_navigate(+1);
        //     if (key_pressed(ImGuiKey_Enter))      hud.popup_select();
        // } else
        if (hud.toast_has_focused()) {
            if (rep_toast_prev.tick(ImGui::IsKeyDown(ImGuiKey_LeftArrow)))  hud.toast_navigate(-1);
            if (rep_toast_next.tick(ImGui::IsKeyDown(ImGuiKey_RightArrow))) hud.toast_navigate(+1);
            if (key_pressed(ImGuiKey_Enter))      hud.toast_select(state);
        } else if (menu.is_open()) {
            if (rep_nav_up  .tick(ImGui::IsKeyDown(ImGuiKey_UpArrow)))   menu.navigate(-1);
            if (rep_nav_down.tick(ImGui::IsKeyDown(ImGuiKey_DownArrow))) menu.navigate(+1);
            if (key_pressed(ImGuiKey_Enter) ||
                key_pressed(ImGuiKey_RightArrow)) menu.select();
            if (key_pressed(ImGuiKey_Backspace) ||
                key_pressed(ImGuiKey_LeftArrow))  menu.back();
        }
        // Vision-assist hotkeys — work whether or not the menu is open
        if (key_pressed(ImGuiKey_E)) state.pp_cfg.edge_enabled   = !state.pp_cfg.edge_enabled;
        if (key_pressed(ImGuiKey_Q)) state.pp_cfg.desat_enabled  = !state.pp_cfg.desat_enabled;
        if (key_pressed(ImGuiKey_W)) state.pp_cfg.motion_enabled = !state.pp_cfg.motion_enabled;
        if (key_pressed(ImGuiKey_D)) sys_panel_active            = !sys_panel_active;
        // C — capture stereo screenshot
        if (key_pressed(ImGuiKey_C)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.capture_request = CaptureRequest::Stereo;
        }
        // F — toggle FPS overlay
        if (key_pressed(ImGuiKey_F)) fps_overlay_active = !fps_overlay_active;
        // Shift+M — calibrate north (Set My Direction); edge-only
        if (ImGui::GetIO().KeyShift && key_pressed(ImGuiKey_M)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.map_north_deg = state.compass_heading;
            state.map_overlay.calibrated    = true;
        }
        // M (no Shift) — short tap: toggle map overlay;
        //               hold 1.5 s: cycle to next map in the maps folder
        {
            const bool m_held = ImGui::IsKeyDown(ImGuiKey_M) && !ImGui::GetIO().KeyShift;
            if (m_held && m_press_t < 0.0) {
                m_press_t    = glfwGetTime();
                m_long_fired = false;
            } else if (!m_held && m_press_t >= 0.0) {
                if (!m_long_fired)
                    state.map_overlay.enabled = !state.map_overlay.enabled;
                m_press_t = -1.0;
            }
            if (m_held && !m_long_fired && m_press_t >= 0.0 &&
                    glfwGetTime() - m_press_t >= 1.5) {
                std::vector<std::string> maps;
                std::error_code ec;
                for (auto& e : std::filesystem::directory_iterator(cfg_map_dir, ec)) {
                    auto ext = e.path().extension().string();
                    if (ext==".png"||ext==".jpg"||ext==".jpeg"||
                        ext==".PNG"||ext==".JPG"||ext==".JPEG")
                        maps.push_back(e.path().string());
                }
                std::sort(maps.begin(), maps.end());
                if (!maps.empty()) {
                    auto it = std::find(maps.begin(), maps.end(), state.map_overlay.map_path);
                    if (it == maps.end() || std::next(it) == maps.end())
                        state.map_overlay.map_path = maps.front();
                    else
                        state.map_overlay.map_path = *std::next(it);
                    state.map_overlay.enabled = true;
                }
                m_long_fired = true;
            }
        }
        // Space — dismiss focused toast or close menu (back)
        if (key_pressed(ImGuiKey_Space)) {
            if (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())     menu.back();
        }

        // ── USB camera stream lifecycle ───────────────────────────────────────
        // Rising edge  → open stream in background (window appears on first frame).
        // Falling edge → close stream, clear texture (no stale frame on re-open).
        bool p1 = pip_cam1_overlay_active || pip_left_active  || kb_pip_left  || wc_pip_left;
        bool p2 = pip_cam2_overlay_active || pip_right_active || kb_pip_right || wc_pip_right;
        bool p3 = pip_cam3_overlay_active;
        if (p1 && !prev_p1) { tex_usb1 = 0; std::thread([&cameras]{ cameras.open_usb1(); }).detach(); }
        if (!p1 && prev_p1) { cameras.close_usb1(); tex_usb1 = 0; }
        if (p2 && !prev_p2) { tex_usb2 = 0; std::thread([&cameras]{ cameras.open_usb2(); }).detach(); }
        if (!p2 && prev_p2) { cameras.close_usb2(); tex_usb2 = 0; }
        if (p3 && !prev_p3) { tex_usb3 = 0; std::thread([&cameras]{ cameras.open_usb3(); }).detach(); }
        if (!p3 && prev_p3) { cameras.close_usb3(); tex_usb3 = 0; }
        prev_p1 = p1; prev_p2 = p2; prev_p3 = p3;

        // ── Camera texture uploads (CPU paths) ────────────────────────────────
        if (use_beast_cam) beast_cam.get_frame(tex_beast);
        if (p1) cameras.get_usb1(tex_usb1);
        if (p2) cameras.get_usb2(tex_usb2);
        if (p3) cameras.get_usb3(tex_usb3);
        android_mirror.get_frame(tex_android);

        // ── GPIO PiP button state ─────────────────────────────────────────────
        if (gpio_enabled) {
            buttons.update_pip_state();
            pip_left_active  = buttons.pip_left_active();
            pip_right_active = buttons.pip_right_active();
        }

        // ── Gamepad poll ──────────────────────────────────────────────────────
        gamepad.poll();

        // ── Wireless controller pip state ─────────────────────────────────────
        bool wc_pip_left  = wireless_enabled && wireless.pip_left_active();
        bool wc_pip_right = wireless_enabled && wireless.pip_right_active();

        // ── Keyboard button emulation (direct GLFW polling, edge-detected) ──
        // 1/2/3     = toggle USB cam PiP 1/2/3   Shift+1/2/3 = autofocus that cam
        // 0         = toggle manual/auto focus    4 = autofocus both cameras
        // - / =     = focus near / far (step 20 of 0-1000)
        {
            GLFWwindow* win = static_cast<GLFWwindow*>(xr.glfw_window());
            auto edge = [&](int n, int glfw_key) -> bool {
                bool now = (glfwGetKey(win, glfw_key) == GLFW_PRESS);
                bool fired = now && !prev_key[n];
                prev_key[n] = now;
                return fired;
            };
            // 0: toggle manual/auto focus
            if (edge(0, GLFW_KEY_0) && !menu.is_open()) {
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
            // - / = : manual focus step (near / far)
            constexpr int FOCUS_STEP = 20;
            if (edge(5, GLFW_KEY_MINUS) && cameras.owl_left()) {
                int pos = std::max(0, cameras.owl_left()->get_focus_position() - FOCUS_STEP);
                cameras.owl_left()->set_focus_position(pos);
                if (cameras.owl_right()) cameras.owl_right()->set_focus_position(pos);
            }
            if (edge(6, GLFW_KEY_EQUAL) && cameras.owl_left()) {
                int pos = std::min(1000, cameras.owl_left()->get_focus_position() + FOCUS_STEP);
                cameras.owl_left()->set_focus_position(pos);
                if (cameras.owl_right()) cameras.owl_right()->set_focus_position(pos);
            }
            // 1/2/3 — toggle USB cam PiP;  Shift+1/2/3 — trigger autofocus on that cam
            {
                bool shift = (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                if (!menu.is_open()) {
                    if (edge(7, GLFW_KEY_1)) {
                        if (shift) cameras.set_usb1_ctrl(V4L2_CID_FOCUS_AUTO, 1);
                        else       pip_cam1_overlay_active = !pip_cam1_overlay_active;
                    }
                    if (edge(8, GLFW_KEY_2)) {
                        if (shift) cameras.set_usb2_ctrl(V4L2_CID_FOCUS_AUTO, 1);
                        else       pip_cam2_overlay_active = !pip_cam2_overlay_active;
                    }
                    if (edge(9, GLFW_KEY_3)) {
                        if (shift) cameras.set_usb3_ctrl(V4L2_CID_FOCUS_AUTO, 1);
                        else       pip_cam3_overlay_active = !pip_cam3_overlay_active;
                    }
                }
            }
        }

        // ── USB camera / Android mirror health update ─────────────────────────
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.cam_usb1       = cameras.usb1_ok();
            state.health.cam_usb2       = cameras.usb2_ok();
            state.health.cam_usb3       = cameras.usb3_ok();
            state.health.android_mirror = android_mirror.is_connected();
        }

        // ── Timer / alarm expiry check ────────────────────────────────────────
        {
            auto& ta = state.timer_alarm;
            time_t now_t = time(nullptr);
            if (ta.timer_active && now_t >= ta.timer_end) {
                ta.timer_active    = false;
                ta.timer_triggered = true;
                Notification n;
                n.type  = NotifType::Timer;
                n.title = "Timer Expired";
                n.timestamp = static_cast<int64_t>(now_t);
                n.auto_dismiss_s = 0.f;
                n.actions.push_back({"DISMISS", [](AppState& s){ s.timer_alarm.timer_triggered = false; }});
                n.actions.push_back({"+2 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+120; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }});
                n.actions.push_back({"+5 MIN",  [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+300; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }});
                n.actions.push_back({"+10 MIN", [](AppState& s){ s.timer_alarm.timer_end = time(nullptr)+600; s.timer_alarm.timer_active=true; s.timer_alarm.timer_triggered=false; }});
                state.notifs.push(std::move(n));
            }
            if (ta.alarm_active && now_t >= ta.alarm_fire_at) {
                ta.alarm_active    = false;
                ta.alarm_triggered = true;
                Notification n;
                n.type  = NotifType::Alarm;
                n.title = "!! ALARM !!";
                n.timestamp = static_cast<int64_t>(now_t);
                n.auto_dismiss_s = 0.f;
                n.actions.push_back({"DISMISS", [](AppState& s){ s.timer_alarm.alarm_triggered = false; }});
                state.notifs.push(std::move(n));
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
        // Also update render-thread metrics (frame time, knob event age) here
        // so they're included in the snapshot under a single lock acquisition.
        AppState snap;
        {
            std::lock_guard<std::mutex> lk(state.mtx);

            // Frame time / FPS into SysMetrics
            {
                auto& m = state.sys_metrics;
                m.frame_time_ms = dt * 1000.f;
                m.fps_avg       = dt > 0.f ? 1.f / dt : 0.f;
                const int h2    = (m.ft_history_head + 1) % kSysHistLen;
                m.ft_history[h2]   = m.frame_time_ms;
                m.ft_history_head  = h2;

                // EMA-smoothed FPS — alpha = exp(-dt / interval)
                static float fps_ema = 0.f;
                const float  fps_inst  = m.fps_avg;
                const float  interval  = static_cast<float>(state.fps_avg_interval_s > 0
                                             ? state.fps_avg_interval_s : 1);
                const float  alpha     = (dt > 0.f) ? expf(-dt / interval) : 0.99f;
                fps_ema = (fps_ema > 0.f)
                    ? fps_ema * alpha + fps_inst * (1.f - alpha)
                    : fps_inst;
                m.fps_avg_smooth = fps_ema;
            }
            // Knob event age (computed on render thread, written here)
            state.serial_metrics.knob_event_age_ms = knob.event_age_ms();

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
            snap.map_overlay        = state.map_overlay;
            snap.sys_metrics        = state.sys_metrics;
            snap.wifi               = state.wifi;
            snap.ping               = state.ping;
            snap.ssh                = state.ssh;
            snap.bt_devices         = state.bt_devices;
            snap.serial_metrics     = state.serial_metrics;
            snap.camera_resolution  = state.camera_resolution;
            snap.zoom_left          = state.zoom_left;
            snap.zoom_right         = state.zoom_right;
            snap.theater_mode       = state.theater_mode;
            snap.theater_anchor     = state.theater_anchor;
            snap.cameras_swapped    = state.cameras_swapped;
            snap.mirror_crop        = state.mirror_crop;
            snap.cam_single         = state.cam_single;
            snap.capture_request    = state.capture_request;
            snap.qr_scan_main       = state.qr_scan_main;
            snap.qr_scan_usb        = state.qr_scan_usb;
            snap.notifs             = state.notifs;
            snap.i2c_scan_results   = state.i2c_scan_results;
            snap.i2c_scan_busy      = state.i2c_scan_busy;
            snap.i2c_scan_bus       = state.i2c_scan_bus;
            snap.gpio_states        = state.gpio_states;
            memcpy(snap.lora_node_colors, state.lora_node_colors,
                   sizeof(state.lora_node_colors));
        }

        // If XR glasses IMU is fresh, recompute compass heading from the selected
        // axis on the render thread.  axis/invert are only read and written on the
        // render thread so no mutex is needed — avoids the data race that made the
        // menu setting invisible to the SDK IMU callback thread.
        {
            auto now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            bool xr_fresh = (now_us - last_xr_imu_us.load()) < 2'000'000ULL;
            if (xr_fresh) {
                const float vals[3] = {
                    snap.imu_pose.roll, snap.imu_pose.pitch, snap.imu_pose.yaw
                };
                float raw = vals[static_cast<int>(state.compass_axis)];
                snap.compass_heading = state.compass_invert
                    ? fmod(raw + 360.0f, 360.0f)
                    : fmod(360.0f - raw, 360.0f);
            }
            // else: snap.compass_heading already holds the MPU-9250 value from the snapshot
        }

        // Smooth compass heading on the render thread so the rate is constant
        // and independent of IMU callback frequency.  Circular lerp (sin/cos)
        // handles the 0/360 wrap.  tau=0.35 s → ~350 ms time constant.
        {
            static float    s_smooth  = -1.f;
            static uint64_t s_last_us = 0;
            auto now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            float dt = s_last_us ? (now_us - s_last_us) * 1e-6f : 0.f;
            s_last_us = now_us;
            float raw = snap.compass_heading;
            if (s_smooth < 0.f || dt > 1.f) {
                s_smooth = raw;
            } else {
                constexpr float kTau = 0.35f;
                float alpha = 1.f - std::expf(-dt / kTau);
                constexpr float kD2R = 3.14159265f / 180.f;
                float fs = std::sinf(s_smooth * kD2R) + alpha * (std::sinf(raw * kD2R) - std::sinf(s_smooth * kD2R));
                float fc = std::cosf(s_smooth * kD2R) + alpha * (std::cosf(raw * kD2R) - std::cosf(s_smooth * kD2R));
                s_smooth = std::atan2f(fs, fc) / kD2R;
                if (s_smooth < 0.f) s_smooth += 360.f;
            }
            snap.compass_heading = s_smooth;
        }

        // GPIO monitor: poll sysfs ~1 Hz (every ~60 frames at 60 FPS).
        // gpio_states is render-thread-only; no mutex needed for either access.
        if (!state.gpio_states.empty()) {
            static int gpio_frame_ctr = 0;
            if (++gpio_frame_ctr >= 60) {
                gpio_frame_ctr = 0;
                poll_gpio_states(state);
                snap.gpio_states = state.gpio_states;
            }
        }

        // Overlay-active flags mirror the exact condition used in draw_pip calls.
        snap.health.cam_usb1_overlay = p1;
        snap.health.cam_usb2_overlay = p2;
        snap.health.cam_usb3_overlay = p3;
        snap.health.gamepad_ok          = gamepad.connected();
        snap.health.wireless_ok         = wireless_enabled && wireless.connected();
        snap.health.wireless_battery_pct = wireless_enabled ? wireless.battery_pct() : -1;

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
            auto& nv = snap.night_vision;

            // Auto-NV: enable/disable NV based on ISP analogue gain as a dark proxy.
            // High gain (bright amplification) means the ISP is compensating for low light.
            if (nv.auto_nv) {
                float gain_l = cameras.owl_left()  ? cameras.owl_left()->analogue_gain()  : 1.f;
                float gain_r = cameras.owl_right() ? cameras.owl_right()->analogue_gain() : 1.f;
                float gain   = std::max(gain_l, gain_r);
                bool  want   = (gain >= nv.auto_nv_gain_threshold);
                if (want != nv.nv_enabled) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.night_vision.nv_enabled = want;
                    if (want) {
                        state.night_vision.exposure_ev = 3.0f;
                        state.night_vision.shutter_us  = 40000;
                    } else {
                        state.night_vision.exposure_ev = 0.0f;
                        state.night_vision.shutter_us  = 16667;
                    }
                    nv = state.night_vision;
                }
            }

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

        // ── Mirror crop override ──────────────────────────────────────────────
        // When enabled, both eyes zoom to the same level and pan inward (toward
        // the nose). Left eye pans right (+inner_bias), right eye pans left
        // (-inner_bias). The vertical position is shared.
        if (snap.mirror_crop.enabled) {
            float cy = (snap.mirror_crop.vertical == CropVertical::Top)    ? 0.25f
                     : (snap.mirror_crop.vertical == CropVertical::Bottom) ? 0.75f
                     : 0.5f;
            snap.zoom_left.zoom      = snap.mirror_crop.zoom;
            snap.zoom_right.zoom     = snap.mirror_crop.zoom;
            snap.zoom_left.center_x  = 0.5f + snap.mirror_crop.inner_bias;
            snap.zoom_right.center_x = 0.5f - snap.mirror_crop.inner_bias;
            snap.zoom_left.center_y  = cy;
            snap.zoom_right.center_y = cy;
        }

        // ── Render cameras into per-eye FBOs ──────────────────────────────────
        // Theater mode: compute a letterbox/pillarbox sub-viewport that preserves
        // the camera's native aspect ratio. Black bars are filled by glClear().
        // When theater_mode is false the camera fills the entire FBO (legacy).
        //
        // TODO (USB layout): when placing USB camera feeds in the black region,
        // shift the main feed flush to the opposite edge so all empty space
        // is a single contiguous rectangle (e.g. pillarbox → main feed flush
        // left, one wide black column on the right; letterbox → main feed flush
        // top, one tall black strip at the bottom). That gives a clean
        // rectangular region to fill with the USB feed rather than two thin
        // split bars. vp_x / vp_y below should be set to 0 (or 0, fh-vp_h
        // respectively) instead of the centered offset.
        // right_eye: true for the right eye FBO so horizontal placement mirrors left.
        // Center:  left eye pushes right, right eye pushes left  → cameras touch at seam.
        // Outside: left eye pushes left,  right eye pushes right → cameras on outer edges.
        // Left:    both feeds on screen-right side, black on screen-left.
        // Right:   both feeds on screen-left side, black on screen-right.
        auto make_theater_vp = [&snap](int fw, int fh, bool right_eye) -> std::array<int,4> {
            using TA = AppState::TheaterAnchor;
            float cam_ar  = (float)snap.camera_resolution.width
                          / snap.camera_resolution.height;
            float disp_ar = (float)fw / fh;
            int vp_w, vp_h, vp_x, vp_y;
            if (cam_ar < disp_ar) {   // pillarbox: black left/right
                vp_h = fh; vp_w = (int)(fh * cam_ar); vp_y = 0;
                switch (snap.theater_anchor) {
                    case TA::Center:
                        // inner edge: left eye pushes right, right eye pushes left
                        vp_x = right_eye ? 0 : fw - vp_w;
                        break;
                    case TA::Outside:
                        // outer edge: left eye pushes left, right eye pushes right
                        vp_x = right_eye ? fw - vp_w : 0;
                        break;
                    case TA::Left:
                        // both feeds on screen-right: both FBOs push to their right edge
                        vp_x = fw - vp_w;
                        break;
                    case TA::Right:
                        // both feeds on screen-left: both FBOs push to their left edge
                        vp_x = 0;
                        break;
                    default:
                        vp_x = (fw - vp_w) / 2;
                        break;
                }
            } else {                  // letterbox: black top/bottom
                vp_w = fw; vp_h = (int)(fw / cam_ar); vp_x = 0;
                switch (snap.theater_anchor) {
                    case TA::Top:    vp_y = fh - vp_h;        break;  // GL Y=0 at bottom
                    case TA::Bottom: vp_y = 0;                 break;
                    default:         vp_y = (fh - vp_h) / 2;  break;
                }
            }
            return { vp_x, vp_y, vp_w, vp_h };
        };

        // Left eye
        {
            xr.eye_left().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (snap.theater_mode) {
                auto vp = make_theater_vp(xr.eye_left().w, xr.eye_left().h, false);
                glViewport(vp[0], vp[1], vp[2], vp[3]);
            }

            bool drew = false;
            if (use_beast_cam && tex_beast != 0) {
                // Beast passthrough — blit via timewarp shader (same NDC quad path)
                // For now: TODO: render tex_beast as fullscreen quad
                drew = false;  // fallback to OWLsight below
            }
            if (!drew) drew = snap.cameras_swapped
                ? cameras.draw_owl_right(snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y)
                : cameras.draw_owl_left (snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y);

            if (snap.theater_mode)
                glViewport(0, 0, xr.eye_left().w, xr.eye_left().h);

            xr.eye_left().unbind();
        }

        // Right eye
        {
            xr.eye_right().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (snap.theater_mode) {
                auto vp = make_theater_vp(xr.eye_right().w, xr.eye_right().h, true);
                glViewport(vp[0], vp[1], vp[2], vp[3]);
            }

            bool drew = false;
            if (use_beast_cam && tex_beast != 0) {
                drew = false;  // fallback to OWLsight below
            }
            if (!drew) drew = snap.cameras_swapped
                ? cameras.draw_owl_left (snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y)
                : cameras.draw_owl_right(snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y);

            if (snap.theater_mode)
                glViewport(0, 0, xr.eye_right().w, xr.eye_right().h);

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

        // ── Photo capture ─────────────────────────────────────────────────────
        // Reads directly from the camera eye FBOs (no HUD). Async PNG write.
        if (snap.capture_request != CaptureRequest::None)
            do_capture(snap.capture_request, xr, cfg_photo_dir, state);

        // ── QR scan — main cameras ────────────────────────────────────────────
        // Periodic glReadPixels from the left eye FBO (rate-limited to 2 Hz by
        // the timer below; ZBar's own interval also guards the worker thread).
        cameras.enable_qr_usb(snap.qr_scan_usb);
        if (snap.qr_scan_main) {
            static auto s_last_qr = std::chrono::steady_clock::now();
            auto now_qr = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_qr - s_last_qr).count() >= 500) {
                s_last_qr = now_qr;
                const int qw = xr.eye_left().w, qh = xr.eye_left().h;
                std::vector<uint8_t> px(static_cast<size_t>(qw * qh * 4));
                xr.eye_left().bind();
                glReadPixels(0, 0, qw, qh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                xr.eye_left().unbind();
                qr_scanner.submit_rgba(px.data(), qw, qh);
            }
        }

        // ── Composite or timewarp ─────────────────────────────────────────────
        ImuPose current_pose = xr.get_latest_imu_pose();

        if (snap.cam_single.enabled) {
            // Single-camera fill: draw one eye's (post-processed) feed to an
            // anchor region of the screen; the rest is cleared to black.
            GLuint src = snap.cam_single.use_right ? right_src : left_src;
            xr.composite_single(src, snap.cam_single.anchor);
        } else if (use_timewarp) {
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

        // ── Phase 1: NanoVG PiP underlay then HUD chrome ─────────────────────
        // Reset viewport to full framebuffer — timewarp leaves it at right-eye half.
        // Pass full display dimensions so NanoVG covers both eye halves.
        glViewport(0, 0, xr.display_width(), xr.display_height());
        hud.draw_pip_underlays(tex_usb1, p1, pip_overlay_cfg1,
                               tex_usb2, p2, pip_overlay_cfg2,
                               tex_usb3, p3, pip_overlay_cfg3,
                               xr.eye_width(), xr.eye_height());
        hud.draw_hud_frame(snap, xr.display_width(), xr.display_height(), fps_overlay_active);
        hud.draw_toasts(state.notifs, xr.display_width(), xr.display_height());

        // ── Phase 2: ImGui overlays (menu, popups) ────────────────────────
        menu.set_glow_enabled(hud.config().glow_enabled);
        menu.draw(xr.eye_width(), xr.eye_height());

        hud.draw_android_overlay(tex_android,
                                  xr.eye_width(), xr.eye_height(),
                                  android_overlay_active,
                                  android_mirror.is_running() && !android_mirror.is_connected(),
                                  android_overlay_cfg,
                                  android_mirror.frame_aspect());

        // Protoface LED preview (top-right corner, above popups).
        if (panel_preview_enabled) {
            GLuint panel_tex = 0;
            protoface_ctrl.get_frame_texture(panel_tex);
            hud.draw_panel_preview(panel_tex, xr.eye_width(), xr.eye_height());
        }

        // System status panel (CPU/RAM/WiFi/ping/BT/SSH/perf/serial).
        hud.draw_sys_panel(snap, xr.eye_width(), xr.eye_height(), sys_panel_active);

        // Alarm / timer-expired popups — disabled, toasts handle these now.
        // hud.draw_popups(state, xr.eye_width(), xr.eye_height());

        hud.render_menu_overlay();

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
        {
            static const char* kAxes[] = { "roll", "pitch", "yaw" };
            cfg["compass"]["axis"]   = kAxes[static_cast<int>(state.compass_axis)];
            cfg["compass"]["invert"] = state.compass_invert;
        }
        cfg["hud"]["flip_vertical"]             = hud.config().hud_flip_vertical;
        cfg["hud"]["effects"]["type"]           = static_cast<int>(state.effects_cfg.effect);
        cfg["hud"]["effects"]["palette"]        = static_cast<int>(state.effects_cfg.palette);

        cfg["night_vision"]["exposure_ev"]            = state.night_vision.exposure_ev;
        cfg["night_vision"]["shutter_us"]             = state.night_vision.shutter_us;
        cfg["night_vision"]["auto_nv"]                = state.night_vision.auto_nv;
        cfg["night_vision"]["auto_nv_gain_threshold"] = state.night_vision.auto_nv_gain_threshold;
        cfg["night_vision"]["csi_awb_left"]           = state.night_vision.csi_awb_left;
        cfg["night_vision"]["csi_awb_right"]          = state.night_vision.csi_awb_right;

        cfg["clock"]["use_24h"]         = state.clock_cfg.use_24h;
        cfg["clock"]["show_seconds"]    = state.clock_cfg.show_seconds;
        cfg["clock"]["show_date"]       = state.clock_cfg.show_date;
        cfg["clock"]["font_scale"]      = state.clock_cfg.font_scale;
        cfg["clock"]["manual_offset_s"] = state.clock_cfg.manual_offset_s;

        cfg["pip"]["cam1"]["anchor_x"]  = pip_overlay_cfg1.anchor_x;
        cfg["pip"]["cam1"]["anchor_y"]  = pip_overlay_cfg1.anchor_y;
        cfg["pip"]["cam1"]["pan_x"]     = pip_overlay_cfg1.pan_x;
        cfg["pip"]["cam1"]["pan_y"]     = pip_overlay_cfg1.pan_y;
        cfg["pip"]["cam1"]["size"]      = pip_overlay_cfg1.size;
        cfg["pip"]["cam1"]["rotation"]  = rotation_to_str(pip_overlay_cfg1.rotation);
        cfg["pip"]["cam2"]["anchor_x"]  = pip_overlay_cfg2.anchor_x;
        cfg["pip"]["cam2"]["anchor_y"]  = pip_overlay_cfg2.anchor_y;
        cfg["pip"]["cam2"]["pan_x"]     = pip_overlay_cfg2.pan_x;
        cfg["pip"]["cam2"]["pan_y"]     = pip_overlay_cfg2.pan_y;
        cfg["pip"]["cam2"]["size"]      = pip_overlay_cfg2.size;
        cfg["pip"]["cam2"]["rotation"]  = rotation_to_str(pip_overlay_cfg2.rotation);

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
        jpp["color_protect"]      = state.pp_cfg.color_protect;
        jpp["edge_dilate"]        = state.pp_cfg.edge_dilate;
        jpp["motion_enabled"]     = state.pp_cfg.motion_enabled;
        jpp["motion_strength"]    = state.pp_cfg.motion_strength;
        jpp["motion_thresh"]      = state.pp_cfg.motion_thresh;
        jpp["motion_radius"]      = state.pp_cfg.motion_radius;
        jpp["motion_line"]        = state.pp_cfg.motion_line;
        jpp["motion_update_rate"] = state.pp_cfg.motion_update_rate;
        jpp["motion_color"]       = color_to_json(state.pp_cfg.motion_color);

        cfg["cameras"]["usb_cam_1"]["device"]            = cameras.usb1_cfg().device;
        cfg["cameras"]["usb_cam_1"]["brightness"]        = cameras.usb1_brightness();
        cfg["cameras"]["usb_cam_1"]["dynamic_framerate"] = cameras.usb1_cfg().dynamic_framerate;
        cfg["cameras"]["usb_cam_1"]["auto_exposure"]     = cameras.usb1_cfg().auto_exposure;
        cfg["cameras"]["usb_cam_1"]["exposure_time"]     = cameras.usb1_cfg().exposure_time;
        cfg["cameras"]["usb_cam_1"]["auto_wb"]           = cameras.usb1_cfg().auto_wb;
        cfg["cameras"]["usb_cam_1"]["wb_temp"]           = cameras.usb1_cfg().wb_temp;
        cfg["cameras"]["usb_cam_1"]["flip"]                   = cameras.usb1_cfg().flip;
        cfg["cameras"]["usb_cam_1"]["auto_brightness"]        = cameras.usb1_cfg().auto_brightness;
        cfg["cameras"]["usb_cam_1"]["auto_brightness_target"] = cameras.usb1_cfg().auto_brightness_target;
        cfg["cameras"]["usb_cam_2"]["device"]            = cameras.usb2_cfg().device;
        cfg["cameras"]["usb_cam_2"]["brightness"]        = cameras.usb2_brightness();
        cfg["cameras"]["usb_cam_2"]["dynamic_framerate"] = cameras.usb2_cfg().dynamic_framerate;
        cfg["cameras"]["usb_cam_2"]["auto_exposure"]     = cameras.usb2_cfg().auto_exposure;
        cfg["cameras"]["usb_cam_2"]["exposure_time"]     = cameras.usb2_cfg().exposure_time;
        cfg["cameras"]["usb_cam_2"]["auto_wb"]           = cameras.usb2_cfg().auto_wb;
        cfg["cameras"]["usb_cam_2"]["wb_temp"]           = cameras.usb2_cfg().wb_temp;
        cfg["cameras"]["usb_cam_2"]["flip"]                   = cameras.usb2_cfg().flip;
        cfg["cameras"]["usb_cam_2"]["auto_brightness"]        = cameras.usb2_cfg().auto_brightness;
        cfg["cameras"]["usb_cam_2"]["auto_brightness_target"] = cameras.usb2_cfg().auto_brightness_target;
        cfg["cameras"]["usb_cam_3"]["device"]            = cameras.usb3_cfg().device;
        cfg["cameras"]["usb_cam_3"]["brightness"]        = cameras.usb3_brightness();
        cfg["cameras"]["usb_cam_3"]["dynamic_framerate"] = cameras.usb3_cfg().dynamic_framerate;
        cfg["cameras"]["usb_cam_3"]["auto_exposure"]     = cameras.usb3_cfg().auto_exposure;
        cfg["cameras"]["usb_cam_3"]["exposure_time"]     = cameras.usb3_cfg().exposure_time;
        cfg["cameras"]["usb_cam_3"]["auto_wb"]           = cameras.usb3_cfg().auto_wb;
        cfg["cameras"]["usb_cam_3"]["wb_temp"]           = cameras.usb3_cfg().wb_temp;
        cfg["cameras"]["usb_cam_3"]["flip"]                   = cameras.usb3_cfg().flip;
        cfg["cameras"]["usb_cam_3"]["auto_brightness"]        = cameras.usb3_cfg().auto_brightness;
        cfg["cameras"]["usb_cam_3"]["auto_brightness_target"] = cameras.usb3_cfg().auto_brightness_target;
        cfg["cameras"]["swapped"] = state.cameras_swapped;
        {
            static const char* kNames[] = { "center","outside","left","right","top","bottom" };
            cfg["cameras"]["theater_anchor"] = kNames[static_cast<int>(state.theater_anchor)];
        }

        cfg["qr"]["scan_main"] = state.qr_scan_main;
        cfg["qr"]["scan_usb"]  = state.qr_scan_usb;

        cfg["fps_avg_interval_s"] = state.fps_avg_interval_s;
        cfg["i2c_scan_bus"]       = state.i2c_scan_bus;

        {
            const auto& mo          = state.map_overlay;
            cfg["map"]["enabled"]             = mo.enabled;
            cfg["map"]["map_path"]            = mo.map_path;
            cfg["map"]["opacity"]             = mo.opacity;
            cfg["map"]["size_px"]             = mo.size_px;
            cfg["map"]["rotate_with_heading"] = mo.rotate_with_heading;
            cfg["map"]["image_rotate_deg"]    = mo.image_rotate_deg;
            cfg["map"]["anchor_x"]            = mo.anchor_x;
            cfg["map"]["anchor_y"]            = mo.anchor_y;
            cfg["map"]["circle_window"]       = mo.circle_window;
            cfg["map"]["zoom"]                = mo.zoom;
        }

        cfg["resolution"]["width"]  = state.camera_resolution.width;
        cfg["resolution"]["height"] = state.camera_resolution.height;
        cfg["resolution"]["fps"]    = state.camera_resolution.fps;

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
            cfg["mpu9250"]["mag_bias"]       = json::array({ bx, by, bz });
            cfg["mpu9250"]["mount_rotation"] = mpu9250.get_mount_rotation();
            cfg["mpu9250"]["heading_axes"]   = mpu9250.get_heading_axes();
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

    bt_mon.stop();
    ping_mon.stop();
    wifi_mon.stop();
    sys_mon.stop();
    mpu9250.stop();
    audio.stop();
    android_mirror.stop();
    hud.unload();
    beast_cam.stop();
    cameras.shutdown();
    teensy.stop();
    protoface_ctrl.stop();
    lora.stop();
    knob.stop();

    if (tex_usb1)    glDeleteTextures(1, &tex_usb1);
    if (tex_usb2)    glDeleteTextures(1, &tex_usb2);
    if (tex_usb3)    glDeleteTextures(1, &tex_usb3);
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
