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
        OverlayConfig* pip_cfg, OverlayConfig* android_cfg)
{
    (void)lora; (void)knob;

    // Face effects
    std::vector<MenuItem> effects;
    for (uint8_t id = 0; id < 10; id++) {
        const char* names[] = { "Idle","Blink","Angry","Happy","Sad",
                                 "Shocked","Rainbow","Pulse","Wave","Custom" };
        effects.push_back({ names[id], [teensy, id]() { teensy->set_effect(id); }, {} });
    }

    // Face colors
    std::vector<MenuItem> colors = {
        { "Teal",   [teensy]{ teensy->set_color(0,220,180);   }, {} },
        { "Cyan",   [teensy]{ teensy->set_color(0,180,255);   }, {} },
        { "Red",    [teensy]{ teensy->set_color(220,30,30);   }, {} },
        { "Green",  [teensy]{ teensy->set_color(30,220,60);   }, {} },
        { "Purple", [teensy]{ teensy->set_color(180,30,220);  }, {} },
        { "White",  [teensy]{ teensy->set_color(255,255,255); }, {} },
    };

    // GIFs
    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "GIF #%d", i);
        gifs.push_back({ lbl, [teensy, i]() { teensy->play_gif(i); }, {} });
    }

    // Face brightness
    std::vector<MenuItem> face_brightness = {
        { "25%",  [teensy]{ teensy->set_brightness(64);  }, {} },
        { "50%",  [teensy]{ teensy->set_brightness(128); }, {} },
        { "75%",  [teensy]{ teensy->set_brightness(192); }, {} },
        { "100%", [teensy]{ teensy->set_brightness(255); }, {} },
    };

    // Glasses display brightness (via VITURE SDK)
    std::vector<MenuItem> glasses_brightness = {
        { "Low",    [xr]{ xr->set_brightness(1); }, {} },
        { "Medium", [xr]{ xr->set_brightness(3); }, {} },
        { "High",   [xr]{ xr->set_brightness(5); }, {} },
        { "Max",    [xr]{ xr->set_brightness(7); }, {} },
    };

    // Camera controls
    std::vector<MenuItem> focus_modes = {
        { "Manual", [&state]{ state.focus_left.mode = CameraFocusState::Mode::MANUAL;
                              state.focus_right.mode = CameraFocusState::Mode::MANUAL; }, {} },
        { "Auto",   [&state]{ state.focus_left.mode = CameraFocusState::Mode::AUTO;
                              state.focus_right.mode = CameraFocusState::Mode::AUTO; }, {} },
        { "Slave",  [&state]{ state.focus_left.mode = CameraFocusState::Mode::SLAVE;
                              state.focus_right.mode = CameraFocusState::Mode::SLAVE; }, {} },
    };

    std::vector<MenuItem> af_triggers = {
        { "Left",  [cameras, &state]{
            if (!cameras) return;
            cameras->owl_left()->start_autofocus();
            if (state.focus_left.mode == CameraFocusState::Mode::SLAVE && cameras->owl_right()) {
                std::thread([cameras]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (cameras->owl_left() && cameras->owl_right())
                        cameras->owl_right()->set_focus_position(
                            cameras->owl_left()->get_focus_position());
                }).detach();
            }
        }, {} },
        { "Right", [cameras, &state]{
            if (!cameras) return;
            cameras->owl_right()->start_autofocus();
            if (state.focus_right.mode == CameraFocusState::Mode::SLAVE && cameras->owl_left()) {
                std::thread([cameras]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (cameras->owl_left() && cameras->owl_right())
                        cameras->owl_left()->set_focus_position(
                            cameras->owl_right()->get_focus_position());
                }).detach();
            }
        }, {} },
        { "Both",  [cameras, &state]{
            if (!cameras) return;
            cameras->owl_left()->start_autofocus();
            cameras->owl_right()->start_autofocus();
            if ((state.focus_left.mode  == CameraFocusState::Mode::SLAVE ||
                 state.focus_right.mode == CameraFocusState::Mode::SLAVE) &&
                cameras->owl_left() && cameras->owl_right()) {
                std::thread([cameras]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    int avg = (cameras->owl_left()->get_focus_position()
                             + cameras->owl_right()->get_focus_position()) / 2;
                    cameras->owl_left()->set_focus_position(avg);
                    cameras->owl_right()->set_focus_position(avg);
                }).detach();
            }
        }, {} },
    };

    std::vector<MenuItem> exposure_ev = {
        { "-3.0", [&state]{ state.night_vision.exposure_ev = -3.0f; }, {} },
        { "-2.0", [&state]{ state.night_vision.exposure_ev = -2.0f; }, {} },
        { "-1.0", [&state]{ state.night_vision.exposure_ev = -1.0f; }, {} },
        { "0.0",  [&state]{ state.night_vision.exposure_ev =  0.0f; }, {} },
        { "+1.0", [&state]{ state.night_vision.exposure_ev =  1.0f; }, {} },
        { "+2.0", [&state]{ state.night_vision.exposure_ev =  2.0f; }, {} },
        { "+3.0", [&state]{ state.night_vision.exposure_ev =  3.0f; }, {} },
    };

    std::vector<MenuItem> shutter_speeds = {
        { "1/4000", [&state]{ state.night_vision.shutter_us =   250; }, {} },
        { "1/2000", [&state]{ state.night_vision.shutter_us =   500; }, {} },
        { "1/1000", [&state]{ state.night_vision.shutter_us =  1000; }, {} },
        { "1/500",  [&state]{ state.night_vision.shutter_us =  2000; }, {} },
        { "1/250",  [&state]{ state.night_vision.shutter_us =  4000; }, {} },
        { "1/125",  [&state]{ state.night_vision.shutter_us =  8000; }, {} },
        { "1/60",   [&state]{ state.night_vision.shutter_us = 16667; }, {} },
        { "1/30",   [&state]{ state.night_vision.shutter_us = 33333; }, {} },
        { "1/25",   [&state]{ state.night_vision.shutter_us = 40000; }, {} },
    };

    std::vector<MenuItem> focus_positions;
    for (int i = 0; i <= 10; i++) {
        int pos = i * 100;
        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", pos);
        focus_positions.push_back({ lbl, [cameras, pos]{
            if (cameras) {
                if (cameras->owl_left())  cameras->owl_left()->set_focus_position(pos);
                if (cameras->owl_right()) cameras->owl_right()->set_focus_position(pos);
            }
        }, {} });
    }

    // ── Resolution presets ────────────────────────────────────────────────────
    // Preset list: {label, width, height, fps}
    // libcamera will snap to the nearest sensor mode it supports.
    struct ResPreset { const char* label; int w, h, fps; };
    static const ResPreset RES_PRESETS[] = {
        { "640×400  @120fps",  640,  400, 120 },
        { "1280×800  @60fps", 1280,  800,  60 },  // default
        { "1920×1080 @30fps", 1920, 1080,  30 },
        { "2560×1440 @15fps", 2560, 1440,  15 },
    };

    std::vector<MenuItem> resolution_presets;
    for (const auto& p : RES_PRESETS) {
        resolution_presets.push_back({
            p.label,
            [cameras, &state, w = p.w, h = p.h, fps = p.fps]() {
                if (!cameras) return;
                if (cameras->set_resolution(w, h, fps)) {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.camera_resolution = { w, h, fps };
                }
            },
            {}
        });
    }

    std::vector<MenuItem> camera_menu = {
        { "Resolution",     nullptr, std::move(resolution_presets) },
        { "Focus Mode",     nullptr, std::move(focus_modes)        },
        { "Focus Position", nullptr, std::move(focus_positions)    },
        { "Autofocus",      nullptr, std::move(af_triggers)        },
        { "Exposure (EV)",  nullptr, std::move(exposure_ev)        },
        { "Shutter Speed",  nullptr, std::move(shutter_speeds)     },
    };

    // VITURE Headset controls
    std::vector<MenuItem> dimming_levels;
    for (int i = 0; i <= 9; i++) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "Level %d", i);
        dimming_levels.push_back({ lbl, [xr, i]{ if (xr) xr->set_dimming(i); }, {} });
    }

    std::vector<MenuItem> hud_brightness_levels;
    for (int i = 1; i <= 9; i++) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "Level %d", i);
        hud_brightness_levels.push_back({ lbl, [xr, i]{ if (xr) xr->set_hud_brightness(i); }, {} });
    }

    std::vector<MenuItem> headset_menu = {
        { "Dimming",    nullptr, std::move(dimming_levels)        },
        { "HUD Bright", nullptr, std::move(hud_brightness_levels) },
        { "Recenter",   [xr]{ if (xr) xr->recenter_tracking(); }, {} },
        { "Gaze Lock",  [xr]{ if (xr) xr->toggle_gaze_lock(); },  {} },
        { "3D SBS",     [xr]{ if (xr) xr->set_3d_mode(true); },   {} },
    };

    // Audio controls — beamforming and noise suppression are handled by the
    // RP2350; CM5 only controls output routing and volume.
    std::vector<MenuItem> volume_levels;
    for (int pct : {25, 50, 75, 100, 125, 150}) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d%%", pct);
        float g = pct / 100.0f;
        volume_levels.push_back({ lbl, [audio, g]{ if (audio) audio->set_master_gain(g); }, {} });
    }

    std::vector<MenuItem> output_menu = {
        { "VITURE",     [audio]{ if (audio) audio->set_output(AudioOutput::VITURE);     }, {} },
        { "Headphones", [audio]{ if (audio) audio->set_output(AudioOutput::HEADPHONES); }, {} },
        { "HDMI",       [audio]{ if (audio) audio->set_output(AudioOutput::HDMI);       }, {} },
    };

    std::vector<MenuItem> audio_menu = {
        { "Enable",  [audio]{ if (audio) audio->set_enabled(true);  }, {} },
        { "Disable", [audio]{ if (audio) audio->set_enabled(false); }, {} },
        { "Volume",  nullptr, std::move(volume_levels) },
        { "Output",  nullptr, std::move(output_menu)   },
    };

    // ── Shared helpers for overlay position / size menus ─────────────────────
    using A = OverlayConfig::Anchor;

    auto make_position_items = [](OverlayConfig* cfg) {
        return std::vector<MenuItem>{
            { "Top Left",      [cfg]{ cfg->anchor = A::TOP_LEFT;      }, {} },
            { "Top Center",    [cfg]{ cfg->anchor = A::TOP_CENTER;    }, {} },
            { "Top Right",     [cfg]{ cfg->anchor = A::TOP_RIGHT;     }, {} },
            { "Bottom Left",   [cfg]{ cfg->anchor = A::BOTTOM_LEFT;   }, {} },
            { "Bottom Center", [cfg]{ cfg->anchor = A::BOTTOM_CENTER; }, {} },
            { "Bottom Right",  [cfg]{ cfg->anchor = A::BOTTOM_RIGHT;  }, {} },
        };
    };

    auto make_size_items = [](OverlayConfig* cfg) {
        return std::vector<MenuItem>{
            { "15%", [cfg]{ cfg->size = 0.15f; }, {} },
            { "20%", [cfg]{ cfg->size = 0.20f; }, {} },
            { "25%", [cfg]{ cfg->size = 0.25f; }, {} },
            { "30%", [cfg]{ cfg->size = 0.30f; }, {} },
            { "40%", [cfg]{ cfg->size = 0.40f; }, {} },
            { "50%", [cfg]{ cfg->size = 0.50f; }, {} },
            { "60%", [cfg]{ cfg->size = 0.60f; }, {} },
        };
    };

    // ── USB camera PiP layout ─────────────────────────────────────────────────
    std::vector<MenuItem> cam1_menu = {
        { "Open",  [cameras]{ cameras->open_usb1();  }, {} },
        { "Close", [cameras]{ cameras->close_usb1(); }, {} },
    };
    std::vector<MenuItem> cam2_menu = {
        { "Open",  [cameras]{ cameras->open_usb2();  }, {} },
        { "Close", [cameras]{ cameras->close_usb2(); }, {} },
    };
    std::vector<MenuItem> pip_menu = {
        { "Cam 1",    nullptr, std::move(cam1_menu)          },
        { "Cam 2",    nullptr, std::move(cam2_menu)          },
        { "Position", nullptr, make_position_items(pip_cfg)  },
        { "Size",     nullptr, make_size_items(pip_cfg)      },
    };

    // ── Android mirror ────────────────────────────────────────────────────────
    std::vector<MenuItem> android_menu = {
        { "Start Mirror", [android_mirror]() {
            std::thread([android_mirror]() { android_mirror->start(); }).detach();
        }, {} },
        { "Stop Mirror",  [android_mirror]() { android_mirror->stop(); }, {} },
        { "Show Overlay", [android_overlay]() { *android_overlay = true;  }, {} },
        { "Hide Overlay", [android_overlay]() { *android_overlay = false; }, {} },
        { "Position",     nullptr, make_position_items(android_cfg) },
        { "Size",         nullptr, make_size_items(android_cfg)     },
    };

    return {
        { "Face Effects",    nullptr, std::move(effects)            },
        { "Face Color",      nullptr, std::move(colors)             },
        { "Play GIF",        nullptr, std::move(gifs)               },
        { "Face Brightness", nullptr, std::move(face_brightness)    },
        { "Lens Brightness", nullptr, std::move(glasses_brightness) },
        { "Camera",          nullptr, std::move(camera_menu)        },
        { "USB Cameras",     nullptr, std::move(pip_menu)           },
        { "Audio",           nullptr, std::move(audio_menu)         },
        { "Headset",         nullptr, std::move(headset_menu)       },
        { "Android Mirror",  nullptr, std::move(android_menu)       },
        { "Request Status",  [teensy]{ teensy->request_status(); }, {} },
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
    }
    if (jcam.contains("usb_cam_2")) {
        usb2_cfg.device = jcam["usb_cam_2"].value("device", "/dev/video3");
        usb2_cfg.width  = jcam["usb_cam_2"].value("width",  1280);
        usb2_cfg.height = jcam["usb_cam_2"].value("height",  720);
    }

    HudConfig hud_cfg;
    hud_cfg.compass_height        = jval(jhud, "compass_height_px",        60);
    hud_cfg.compass_bottom_margin = jval(jhud, "compass_bottom_margin_px",  20);
    hud_cfg.panel_width          = jval(jhud, "panel_width_px",       200);
    hud_cfg.health_panel_opacity = jval(jhud, "health_panel_opacity", 0.71f);
    hud_cfg.opacity              = jval(jdisp,"hud_opacity",          0.85f);
    hud_cfg.scale                = jval(jdisp,"hud_scale",            1.0f);

    AndroidMirrorConfig and_cfg;
    OverlayConfig       pip_overlay_cfg;
    OverlayConfig       android_overlay_cfg;

    if (cfg.contains("pip")) {
        auto& jpip = cfg["pip"];
        pip_overlay_cfg.size   = jval(jpip, "size",   0.25f);
        pip_overlay_cfg.anchor = parse_anchor(jpip.value("anchor", std::string("top_center")));
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
    state.max_messages = jval(jhud, "lora_message_history", 50);

    if (cfg.contains("night_vision")) {
        auto& jnv = cfg["night_vision"];
        state.night_vision.exposure_ev = jnv.value("exposure_ev",  0.0f);
        state.night_vision.shutter_us  = jnv.value("shutter_us",  33333);
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
    xr.on_imu_pose([&state](float roll, float pitch, float yaw) {
        float bearing = fmod(360.0f - yaw, 360.0f);
        std::lock_guard<std::mutex> lk(state.mtx);
        state.compass_heading = bearing;
        state.imu_pose = { roll, pitch, yaw };
    });

    xr.on_state_changed([](int id, int val) {
        std::cout << "[xr] state change id=" << id << " val=" << val << "\n";
    });

    // ── Async Timewarp ────────────────────────────────────────────────────────

    CameraIntrinsics K { 1920.0f, 1920.0f, 960.0f, 540.0f };
    AsyncTimewarp timewarp(K,
        res("assets/shaders/timewarp.vs").c_str(),
        res("assets/shaders/timewarp.fs").c_str());
    bool use_timewarp = timewarp.init();
    if (!use_timewarp)
        std::cerr << "[main] timewarp shader unavailable — skipping\n";

    // ── Spatial audio ─────────────────────────────────────────────────────────

    if (!audio.start())
        std::cerr << "[main] spatial audio unavailable — continuing without audio\n";

    // ── HUD renderer ──────────────────────────────────────────────────────────
    // Must be after xr.init() so the GLFW window + GL context exist.

    HudColors   hud_col;
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

    MenuSystem menu(build_menu(&teensy, &xr, &cameras, &lora, &knob, &audio, state,
                               &android_mirror, &android_overlay_active,
                               &pip_overlay_cfg, &android_overlay_cfg));

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
    bool pip_left_active = false, pip_right_active = false;

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

        // ── USB camera / Android mirror health update ─────────────────────────
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.cam_usb1       = cameras.usb1_ok();
            state.health.cam_usb2       = cameras.usb2_ok();
            state.health.android_mirror = android_mirror.is_connected();
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
            snap.compass_heading = state.compass_heading;
            snap.imu_pose        = state.imu_pose;
        }

        // Record render-time pose for timewarp
        timewarp.begin_frame(snap.imu_pose);

        // ── Apply camera settings (exposure, shutter) ─────────────────────────
        if (cameras.owl_left()) {
            cameras.owl_left()->set_exposure_ev(state.night_vision.exposure_ev);
            cameras.owl_left()->set_shutter_speed_us(state.night_vision.shutter_us);
        }
        if (cameras.owl_right()) {
            cameras.owl_right()->set_exposure_ev(state.night_vision.exposure_ev);
            cameras.owl_right()->set_shutter_speed_us(state.night_vision.shutter_us);
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
            timewarp.warp_frame(xr.eye_left().tex,  ew, eh, current_pose);
            glViewport(half_w, 0, half_w, dh);
            timewarp.warp_frame(xr.eye_right().tex, ew, eh, current_pose);
        } else {
            // Standard composite (no latency correction)
            xr.composite();
        }

        // ── ImGui HUD overlay (renders to default framebuffer, on top of camera) ──

        hud.draw_frame(snap, xr.eye_width(), xr.eye_height());
        menu.draw(xr.eye_width(), xr.eye_height());

        if (pip_left_active || pip_right_active) {
            hud.draw_pip(tex_usb1, tex_usb2,
                         xr.eye_width(), xr.eye_height(),
                         pip_left_active || pip_right_active,
                         pip_overlay_cfg);
        }

        hud.draw_android_overlay(tex_android,
                                  xr.eye_width(), xr.eye_height(),
                                  android_overlay_active,
                                  android_mirror.is_running() && !android_mirror.is_connected(),
                                  android_overlay_cfg);

        hud.render_overlay();

        // ── Swap ──────────────────────────────────────────────────────────────
        xr.present();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────

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

    xr.shutdown();
    return 0;
}
