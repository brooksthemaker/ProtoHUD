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
#include "menu/build_menu.h"
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

// parse_failed (out) is set true ONLY when the file exists but is malformed —
// so the caller can refuse to overwrite it on exit (a missing file is a normal
// first run and does not set the flag).
static json load_config(const std::string& path, bool* parse_failed = nullptr) {
    if (parse_failed) *parse_failed = false;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        std::cerr << "[cfg] cannot open " << path << " — using defaults\n";
        return json::object();
    }
    json j = json::parse(f, nullptr, false);
    fclose(f);
    if (j.is_discarded()) {
        std::cerr << "[cfg] parse error in " << path
                  << " — using defaults (will NOT overwrite it; fix the JSON)\n";
        if (parse_failed) *parse_failed = true;
        return json::object();
    }
    return j;
}

template<typename T>
T jval(const json& j, const std::string& key, T def) {
    try { return j.value(key, def); }
    catch (...) { return def; }
}

// Mark the minimap module always-on. The actual placement (minimap flush-right,
// info panel mirrored-left, top/bottom + v_offset) is computed per frame in the
// render loop, where the live display size and minimap radius are known — that's
// what lets the compass cardinals sit flush against the screen edges.
void apply_hud_dock(AppState& s) {
    // Minimap and info panel are both always-on for now.
    s.map_overlay.enabled = true;
    s.info_panel.enabled  = true;
}

// ── Native face renderer config ───────────────────────────────────────────────
// Build a face::RenderConfig from config.json's "protoface" section. Falls back
// to the standard 2-panel mirrored layout (face_left + face_right) and to the
// Protoface submodule's asset folders under $HOME/protohud/Protoface.

// IMU heading picker — replaces the old "Viture wins, MPU is backup"
// hardcoded path. Reads state.imu_source plus the per-slot freshness
// timestamps and picks the heading that should drive the HUD compass
// this frame. Auto mode prefers BNO055 (on-chip 9-DOF fusion) over
// MPU9250 (compass-only) over Viture (least-trusted; can drift wildly
// with the headset off-face). Explicit modes force their source even if
// stale — caller is responsible for downgrading if that's not desired.
static float pick_imu_heading(const AppState& s, int64_t now_us) {
    constexpr int64_t kStaleUs = 2'000'000;   // 2 s
    auto fresh = [now_us](const AppState::ImuSlot& slot) {
        return slot.last_us > 0 && (now_us - slot.last_us) < kStaleUs;
    };
    switch (s.imu_source) {
    case AppState::ImuSource::Bno08x:  return s.imu_bno08x.heading_deg;
    case AppState::ImuSource::Bno055:  return s.imu_bno.heading_deg;
    case AppState::ImuSource::Mpu9250: return s.imu_mpu.heading_deg;
    case AppState::ImuSource::Viture:  return s.imu_viture.heading_deg;
    case AppState::ImuSource::None:    return s.compass_heading;   // freeze
    case AppState::ImuSource::Auto:
    default:
        // Best fresh source wins. If nothing's fresh, hold the most recent
        // value rather than snapping to zero. BNO086 (mag-referenced, no yaw
        // drift) is preferred over the BNO055 when present.
        if (fresh(s.imu_bno08x))             return s.imu_bno08x.heading_deg;
        if (fresh(s.imu_bno))                return s.imu_bno   .heading_deg;
        if (fresh(s.imu_mpu))                return s.imu_mpu   .heading_deg;
        if (fresh(s.imu_viture))             return s.imu_viture.heading_deg;
        if (s.imu_bno08x.last_us > 0)        return s.imu_bno08x.heading_deg;
        if (s.imu_bno   .last_us > 0)        return s.imu_bno   .heading_deg;
        if (s.imu_mpu   .last_us > 0)        return s.imu_mpu   .heading_deg;
        if (s.imu_viture.last_us > 0)        return s.imu_viture.heading_deg;
        return s.compass_heading;
    }
}

// ── Per-CSI-camera control persistence ────────────────────────────────────────
// The menu writes AF/AE/WB/ISP/HDR controls straight onto the DmaCamera. These
// helpers carry them through config: parse_* (config → state) and apply_* (state
// → camera) run at startup; read_* (camera → state) + write_* (state → config)
// run on save. State is the canonical persisted copy so a value survives even
// when the camera is briefly absent.
static void parse_cam_controls(const json& j, CameraControlsState& s) {
    if (!j.is_object() || !j.contains("controls") || !j["controls"].is_object()) return;
    const auto& c = j["controls"];
    s.af_range        = c.value("af_range",         s.af_range);
    s.af_speed        = c.value("af_speed",         s.af_speed);
    s.gain            = c.value("gain",             s.gain);
    s.ae_metering     = c.value("ae_metering",      s.ae_metering);
    s.ae_constraint   = c.value("ae_constraint",    s.ae_constraint);
    s.ae_exp_mode     = c.value("ae_exposure_mode", s.ae_exp_mode);
    s.flicker         = c.value("flicker",          s.flicker);
    s.awb_mode        = c.value("awb_mode",         s.awb_mode);
    s.brightness      = c.value("brightness",       s.brightness);
    s.contrast        = c.value("contrast",         s.contrast);
    s.saturation      = c.value("saturation",       s.saturation);
    s.sharpness       = c.value("sharpness",        s.sharpness);
    s.noise_reduction = c.value("noise_reduction",  s.noise_reduction);
    s.hdr             = c.value("hdr",              s.hdr);
}

static void write_cam_controls(json& j, const CameraControlsState& s) {
    j["af_range"]         = s.af_range;
    j["af_speed"]         = s.af_speed;
    j["gain"]             = s.gain;
    j["ae_metering"]      = s.ae_metering;
    j["ae_constraint"]    = s.ae_constraint;
    j["ae_exposure_mode"] = s.ae_exp_mode;
    j["flicker"]          = s.flicker;
    j["awb_mode"]         = s.awb_mode;
    j["brightness"]       = s.brightness;
    j["contrast"]         = s.contrast;
    j["saturation"]       = s.saturation;
    j["sharpness"]        = s.sharpness;
    j["noise_reduction"]  = s.noise_reduction;
    j["hdr"]              = s.hdr;
}

static void apply_cam_controls(DmaCamera* c, const CameraControlsState& s) {
    if (!c) return;
    c->set_af_range(s.af_range);
    c->set_af_speed(s.af_speed);
    if (s.gain > 0.0f) c->set_analogue_gain(s.gain);
    c->set_ae_metering(s.ae_metering);
    c->set_ae_constraint(s.ae_constraint);
    c->set_ae_exposure_mode(s.ae_exp_mode);
    c->set_flicker_mode(s.flicker);
    // Only push a WB preset if one was actually chosen — set_awb_mode forces
    // AWB on, which would override manual WB; default Auto (0) leaves it be.
    if (s.awb_mode > 0) c->set_awb_mode(s.awb_mode);
    c->set_brightness(s.brightness);
    c->set_contrast(s.contrast);
    c->set_saturation(s.saturation);
    c->set_sharpness(s.sharpness);
    c->set_noise_reduction(s.noise_reduction);
    c->set_hdr_mode(s.hdr);
}

static void read_cam_controls(DmaCamera* c, CameraControlsState& s) {
    if (!c) return;   // camera absent → keep the last-known (loaded) values
    s.af_range        = c->af_range();
    s.af_speed        = c->af_speed();
    s.gain            = c->analogue_gain_target();
    s.ae_metering     = c->ae_metering();
    s.ae_constraint   = c->ae_constraint();
    s.ae_exp_mode     = c->ae_exposure_mode();
    s.flicker         = c->flicker_mode();
    s.awb_mode        = c->awb_mode();
    s.brightness      = c->brightness();
    s.contrast        = c->contrast();
    s.saturation      = c->saturation();
    s.sharpness       = c->sharpness();
    s.noise_reduction = c->noise_reduction();
    s.hdr             = c->hdr_mode();
}

// Launch the panel_driver.py piomatter shim as a detached child. Used at
// startup AND from the menu's backend hot-swap when switching back to HUB75.
// fork()+setsid() (instead of `system("... &")`) so SIGINT to the parent
// doesn't take the driver down with it.
// Geometry (panel_w/panel_h/chain/parallel) must match the canvas the renderer
// pushes into /dev/shm, otherwise panel_driver.py's piomatter framebuffer is a
// different shape than the frame it reads and the blit throws on the first
// frame (panels never light). The defaults below mirror panel_driver.py's own
// argparse defaults so existing callers behave unchanged.
static void pf_launch_panel_driver(const std::string& bin_dir,
                                   int canvas_w, int canvas_h,
                                   int panel_w = 64, int panel_h = 32,
                                   int chain = 2, int parallel = 1,
                                   const std::string& pinout = "adafruit_bonnet",
                                   const std::string& color_order = "auto",
                                   bool camera_mode = false,
                                   int camera_planes = 10,
                                   int camera_temporal_planes = 8) {
    std::string drv = bin_dir + "/../scripts/panel_driver.py";
    // Validate host-side — an unknown value would hit the driver's argparse
    // choices and exit, leaving the panels dark with only a log to explain.
    static const char* kOrders[] = {"auto","rgb","rbg","grb","gbr","brg","bgr"};
    std::string order = "auto";
    for (const char* o : kOrders)
        if (color_order == o) { order = color_order; break; }
    std::string cw  = std::to_string(canvas_w);
    std::string chh = std::to_string(canvas_h);
    std::string pw  = std::to_string(panel_w);
    std::string ph  = std::to_string(panel_h);
    std::string ch  = std::to_string(chain);
    std::string par = std::to_string(parallel);
    std::string cm  = camera_mode ? "1" : "0";
    std::string cp  = std::to_string(camera_planes);
    std::string ctp = std::to_string(camera_temporal_planes);
    // Stop any existing driver and WAIT for it to actually exit before
    // relaunching. piomatter (RP1 PIO + DMA + /dev/mem) can't be initialised
    // by two processes at once, so an immediate relaunch races the dying
    // process and the panels stay blank ("blanked but nothing restarted").
    // SIGTERM first so piomatter cleans up its PIO/DMA, poll up to ~2s for it
    // to go, then SIGKILL only stragglers and reap our old child.
    std::system("pkill -f panel_driver.py 2>/dev/null");
    bool gone = false;
    for (int i = 0; i < 20; ++i) {
        if (std::system("pgrep -f panel_driver.py >/dev/null 2>&1") != 0) { gone = true; break; }
        usleep(100 * 1000);
    }
    if (!gone) {
        std::system("pkill -9 -f panel_driver.py 2>/dev/null");
        usleep(300 * 1000);   // give the kernel time to tear down DMA + free /dev/mem
    }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}   // reap exited driver children

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int lf = ::open("/tmp/panel_driver.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); ::close(lf); }
        int nf = ::open("/dev/null", O_RDONLY);
        if (nf >= 0) { dup2(nf, 0); ::close(nf); }
        execlp("python3", "python3", "-u", drv.c_str(),
               "--canvas-w", cw.c_str(), "--canvas-h", chh.c_str(),
               "--panel-w", pw.c_str(), "--panel-h", ph.c_str(),
               "--chain", ch.c_str(), "--parallel", par.c_str(),
               "--pinout", pinout.c_str(),
               "--order", order.c_str(),
               "--camera-mode", cm.c_str(),
               "--camera-planes", cp.c_str(),
               "--camera-temporal-planes", ctp.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }
    std::cout << "[main] launched panel_driver.py pid=" << pid
              << " (" << drv
              << ", canvas " << cw << "x" << chh
              << ", panel " << pw << "x" << ph
              << ", chain " << ch << ", parallel " << par
              << ", camera_mode " << cm << ")\n";
}

// Build the PanelOutput that NativeFaceController writes into. Reads
// cfg["protoface"]["backend"]:
//   "hub75"   (default) → ShmPusherOutput; panel_driver.py shuttles to LEDs.
//   "max7219"          → Max7219PanelOutput direct-to-spidev, multi-chain.
//   "rgb_matrix"       → NeoPixelMatrixOutput — WS2812-based 8x8 RGB matrix
//                        drop-ins replacing the MAX7219 modules. Same chain
//                        geometry; full RGB per pixel.
// Pulled out as a free helper so both the startup path and the menu's
// Per-side daisy-chain assignment lives below pf_eye_w / pf_mirror_x (those
// are defined later in the file). Forward-declare what pf_build_panel_output
// needs so it can call into the helper without reordering everything else.
// Map the WeatherMonitor's WMO weather code onto a face effect spec for the
// Weather Sync feature ("" ≙ null json = no override, show the user's effect).
// Presets referenced here live in src/face/particles.cpp presets().
static nlohmann::json weather_effect_spec(int code, bool is_day) {
    auto preset = [](const char* p) { nlohmann::json j; j["preset"] = p; return j; };
    if (code == 0 || code == 1)                       // clear / mostly clear
        return is_day ? nlohmann::json() : preset("night_sky");
    if (code == 3 || code == 45 || code == 48)        // overcast / fog
        return nlohmann::json("clouds");
    if (code == 71 || code == 85)                     // light snow
        return preset("gentle_snow");
    if ((code >= 73 && code <= 77) || code == 86)     // moderate-heavy snow
        return preset("heavy_snow");
    if (code >= 95 && code <= 99)                     // thunderstorm
        return preset("thunderstorm");
    if ((code >= 51 && code <= 67) ||                 // drizzle / rain
        (code >= 80 && code <= 82))                   // showers
        return preset("rain");
    return nlohmann::json();                          // partly cloudy etc. → none
}

struct PfSideChains {
    std::vector<std::array<int, 2>> left;
    std::vector<std::array<int, 2>> right;
};
static PfSideChains pf_auto_side_chains(
        const std::string& eye_layout,
        const std::string& mouth_layout,
        const std::string& nose_layout,
        int canvas_h);

static void pf_hub75_driver_geometry(const PfHub75Layout& L,
                                     int& panel_w, int& panel_h,
                                     int& chain, int& parallel);

// hot-swap action use the exact same construction logic.
static std::unique_ptr<face::PanelOutput>
pf_build_panel_output(const json& cfg, const face::RenderConfig& rc,
                      const std::string& pf_eye_layout   = "1x2",
                      const std::string& pf_mouth_layout = "1x3",
                      const std::string& pf_nose_layout  = "1x1",
                      const PfHub75Layout* hub75         = nullptr) {
    const json* jpf = cfg.contains("protoface") ? &cfg["protoface"] : nullptr;
    const std::string backend = jpf ? jpf->value("backend", std::string("hub75"))
                                    : std::string("hub75");
    if (backend == "max7219") {
        face::Max7219PanelOutput::Config mc;
        if (jpf && jpf->contains("max7219")) {
            const auto& jm = (*jpf)["max7219"];
            // Optional shared GPIO bus for chains whose transport is "gpio".
            mc.gpio_chip    = jm.value("gpio_chip",    std::string("/dev/gpiochip0"));
            mc.gpio_din_pin = jm.value("gpio_din_pin", -1);
            mc.gpio_clk_pin = jm.value("gpio_clk_pin", -1);
            if (jm.contains("chains") && jm["chains"].is_array()) {
                for (const auto& jc : jm["chains"]) {
                    face::Max7219Chain::Config cc;
                    cc.name        = jc.value("name",        std::string("chain"));
                    const std::string tr = jc.value("transport", std::string("spidev"));
                    cc.transport   = (tr == "gpio")
                        ? face::Max7219Chain::Transport::Gpio
                        : face::Max7219Chain::Transport::Spidev;
                    cc.spi_device  = jc.value("spi_device",  std::string("/dev/spidev0.1"));
                    cc.speed_hz    = jc.value("speed_hz",    1'000'000);
                    cc.gpio_chip   = jc.value("gpio_chip",   mc.gpio_chip);
                    cc.gpio_cs_pin = jc.value("gpio_cs_pin", -1);
                    cc.cols_chips  = jc.value("cols_chips",  1);
                    cc.rows_chips  = jc.value("rows_chips",  1);
                    cc.canvas_x    = jc.value("canvas_x",    0);
                    cc.canvas_y    = jc.value("canvas_y",    0);
                    cc.intensity   = static_cast<uint8_t>(
                        std::clamp(jc.value("intensity", 4),   0,   15));
                    cc.threshold   = static_cast<uint8_t>(
                        std::clamp(jc.value("threshold", 80),  0,  255));
                    const std::string mt = jc.value("module_type", std::string("fc16"));
                    cc.module_type = (mt == "generic1088" || mt == "generic")
                        ? face::Max7219Chain::ModuleType::Generic1088
                        : face::Max7219Chain::ModuleType::FC16;
                    const std::string co = jc.value("chain_order", std::string("serpentine"));
                    cc.chain_order = (co == "row_major" || co == "row-major")
                        ? face::Max7219Chain::ChainOrder::RowMajor
                        : face::Max7219Chain::ChainOrder::Serpentine;
                    // Optional per-module positions for non-rectangular chains.
                    // Each entry: [canvas_x, canvas_y] of a module's top-left,
                    // in DIN→DOUT order along the chain.
                    if (jc.contains("module_positions") && jc["module_positions"].is_array()) {
                        for (const auto& jp : jc["module_positions"]) {
                            if (jp.is_array() && jp.size() == 2)
                                cc.module_positions.push_back(
                                    {jp[0].get<int>(), jp[1].get<int>()});
                        }
                    }
                    mc.chains.push_back(std::move(cc));
                }
            }
        }
        // Auto-fill chains from the layout pickers when the user hasn't
        // authored their own — two daisy chains, one per side of the face,
        // each carrying eye + share-of-nose + mouth-half modules. Defaults
        // wire left side to spidev0.0 (CE0) and right side to spidev1.0.
        if (mc.chains.empty()) {
            PfSideChains auto_s = pf_auto_side_chains(pf_eye_layout,
                                                      pf_mouth_layout,
                                                      pf_nose_layout,
                                                      rc.canvas_h);
            if (!auto_s.left.empty()) {
                face::Max7219Chain::Config cl;
                cl.name = "left";
                cl.spi_device = "/dev/spidev0.0";
                cl.module_positions = std::move(auto_s.left);
                mc.chains.push_back(std::move(cl));
            }
            if (!auto_s.right.empty()) {
                face::Max7219Chain::Config cr;
                cr.name = "right";
                cr.spi_device = "/dev/spidev1.0";
                cr.module_positions = std::move(auto_s.right);
                mc.chains.push_back(std::move(cr));
            }
        }
        return std::make_unique<face::Max7219PanelOutput>(std::move(mc));
    }
    if (backend == "rgb_matrix") {
        face::NeoPixelMatrixOutput::Config nc;
        if (jpf && jpf->contains("rgb_matrix")) {
            const auto& jm = (*jpf)["rgb_matrix"];
            if (jm.contains("chains") && jm["chains"].is_array()) {
                for (const auto& jc : jm["chains"]) {
                    face::NeoPixelMatrixChain::Config cc;
                    cc.name       = jc.value("name",       std::string("chain"));
                    cc.spi_device = jc.value("spi_device", std::string("/dev/spidev0.0"));
                    cc.speed_hz   = jc.value("speed_hz",   2'400'000);
                    cc.cols_chips = jc.value("cols_chips", 1);
                    cc.rows_chips = jc.value("rows_chips", 1);
                    cc.canvas_x   = jc.value("canvas_x",   0);
                    cc.canvas_y   = jc.value("canvas_y",   0);
                    cc.brightness = static_cast<uint8_t>(
                        std::clamp(jc.value("brightness", 64), 0, 255));
                    const std::string pl = jc.value("pixel_layout",
                                                    std::string("adafruit_serpentine"));
                    cc.pixel_layout = (pl == "row_major" || pl == "row-major")
                        ? face::NeoPixelMatrixChain::PixelLayout::RowMajor
                        : face::NeoPixelMatrixChain::PixelLayout::AdafruitSerpentine;
                    const std::string co = jc.value("chain_order", std::string("serpentine"));
                    cc.chain_order = (co == "row_major" || co == "row-major")
                        ? face::NeoPixelMatrixChain::ChainOrder::RowMajor
                        : face::NeoPixelMatrixChain::ChainOrder::Serpentine;
                    const std::string ord = jc.value("color_order", std::string("GRB"));
                    cc.color_order = (ord == "RGB" || ord == "rgb")
                        ? face::NeoPixelMatrixChain::ColorOrder::RGB
                        : face::NeoPixelMatrixChain::ColorOrder::GRB;
                    if (jc.contains("module_positions") && jc["module_positions"].is_array()) {
                        for (const auto& jp : jc["module_positions"]) {
                            if (jp.is_array() && jp.size() == 2)
                                cc.module_positions.push_back(
                                    {jp[0].get<int>(), jp[1].get<int>()});
                        }
                    }
                    nc.chains.push_back(std::move(cc));
                }
            }
        }
        // Auto-fill chains (per-side daisy) — same logic as the MAX7219
        // path. RGB matrix shares MOSI semantics with WS2812 strips, so
        // each side ideally lives on its own SPI bus.
        if (nc.chains.empty()) {
            PfSideChains auto_s = pf_auto_side_chains(pf_eye_layout,
                                                      pf_mouth_layout,
                                                      pf_nose_layout,
                                                      rc.canvas_h);
            if (!auto_s.left.empty()) {
                face::NeoPixelMatrixChain::Config cl;
                cl.name = "left";
                cl.spi_device = "/dev/spidev0.0";
                cl.module_positions = std::move(auto_s.left);
                nc.chains.push_back(std::move(cl));
            }
            if (!auto_s.right.empty()) {
                face::NeoPixelMatrixChain::Config cr;
                cr.name = "right";
                cr.spi_device = "/dev/spidev1.0";
                cr.module_positions = std::move(auto_s.right);
                nc.chains.push_back(std::move(cr));
            }
        }
        return std::make_unique<face::NeoPixelMatrixOutput>(std::move(nc));
    }
    // HUB75 / daemon path — pass the layout-derived panel inventory through
    // so the in-HUD face editor knows which regions to outline. When the
    // user hasn't picked a HUB75 layout, panels stays empty and the editor
    // stays hidden (legacy daemon-mode behaviour preserved).
    std::vector<face::ShmPusherOutput::Panel> hub75_panels;
    if (hub75) hub75_panels = pf_hub75_panels(*hub75);
    return std::make_unique<face::ShmPusherOutput>(
        rc.canvas_w, rc.canvas_h, std::move(hub75_panels));
}

// ── HUB75 panel layout (presets + per-panel nudge) ───────────────────────────
// PfHub75Layout is forward-declared above so pf_build_panel_output /
// pf_build_render_config can see it; the helpers below are the actual
// implementations.

static void pf_hub75_panel_dims(const std::string& sz, int& w, int& h) {
    if      (sz == "32x16")  { w = 32;  h = 16; }
    else if (sz == "64x64")  { w = 64;  h = 64; }
    else if (sz == "96x48")  { w = 96;  h = 48; }
    else if (sz == "128x32") { w = 128; h = 32; }
    else if (sz == "128x64") { w = 128; h = 64; }
    else                     { w = 64;  h = 32; }   // "64x32" default
}

// Returns each panel's (x, y, panel_w, panel_h) with the user's nudge applied
// and a canonical name ("panel_0" .. "panel_N"). The first panel always
// anchors at (0, 0) + nudge[0]; subsequent panels lay out per arrangement.
//
// No background margin: the canvas is the *tight* bounding box of the panel
// set so it maps 1:1 onto the physical HUB75 framebuffer that panel_driver.py
// builds (a chain/parallel arrangement of panels has no border). A margin
// would blit as a dark band around the face and shift it off the panels — and
// since the renderer and the in-HUD editor share this layout, both stay WYSIWYG
// with what actually lights up. Per-panel nudges still offset a panel within
// the canvas; an extreme nudge that pushes a panel past the bounding box simply
// clips, which is unavoidable on fixed-position hardware anyway.
static constexpr int kHub75Margin = 0;

// Centre = 0 model. nudge_dx[i] / nudge_dy[i] are stored as the offset of
// panel i's CENTRE from the canvas centre (px). Default values are the
// "auto-placed" positions — see pf_hub75_apply_defaults below. To convert
// stored nudges into a panel rect: rect.x = canvas_w/2 + nudge_dx - pw/2.
std::vector<face::ShmPusherOutput::Panel>
pf_hub75_panels(const PfHub75Layout& L) {
    const int n = std::clamp(L.panel_count, 1, 4);
    std::vector<face::ShmPusherOutput::Panel> out;
    out.reserve(static_cast<size_t>(n));
    int pw[4] = {0,0,0,0}, ph[4] = {0,0,0,0};
    for (int i = 0; i < 4; ++i) {
        const std::string& s = L.panel_size_per[i].empty()
                               ? L.panel_size : L.panel_size_per[i];
        pf_hub75_panel_dims(s, pw[i], ph[i]);
    }
    int cw = 0, ch = 0;
    pf_hub75_canvas(L, cw, ch);
    const int cx = cw / 2;
    const int cy = ch / 2;
    for (int i = 0; i < n; ++i) {
        face::ShmPusherOutput::Panel p;
        p.name = "panel_" + std::to_string(i);
        p.rect = cv::Rect(cx + L.nudge_dx[i] - pw[i] / 2,
                          cy + L.nudge_dy[i] - ph[i] / 2,
                          pw[i], ph[i]);
        out.push_back(std::move(p));
    }
    return out;
}

// Compute and store the "auto-placed" centre offsets for the current
// arrangement / count / sizes. Called when the user picks a new size,
// count or arrangement (and once at startup if cfg didn't load any).
void pf_hub75_apply_defaults(PfHub75Layout& L) {
    const int n = std::clamp(L.panel_count, 1, 4);
    int pw[4] = {0,0,0,0}, ph[4] = {0,0,0,0};
    for (int i = 0; i < 4; ++i) {
        const std::string& s = L.panel_size_per[i].empty()
                               ? L.panel_size : L.panel_size_per[i];
        pf_hub75_panel_dims(s, pw[i], ph[i]);
    }
    // First compute centred-set absolute positions (with margin), then
    // convert to centre-offsets from the canvas centre.
    int x[4] = {0,0,0,0}, y[4] = {0,0,0,0};
    if (L.arrangement == "vertical") {
        int yy = kHub75Margin;
        int max_w = 0;
        for (int i = 0; i < n; ++i) max_w = std::max(max_w, pw[i]);
        for (int i = 0; i < n; ++i) {
            x[i] = kHub75Margin + (max_w - pw[i]) / 2;
            y[i] = yy; yy += ph[i];
        }
    } else if (L.arrangement == "grid2x2") {
        const int row0_h = std::max(ph[0], n >= 2 ? ph[1] : 0);
        for (int i = 0; i < n; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            int xx = kHub75Margin;
            if (col == 1) xx += pw[i - 1];
            int yy = kHub75Margin;
            if (row == 1) yy += row0_h;
            x[i] = xx; y[i] = yy;
        }
    } else { // horizontal
        int xx = kHub75Margin;
        int max_h = 0;
        for (int i = 0; i < n; ++i) max_h = std::max(max_h, ph[i]);
        for (int i = 0; i < n; ++i) {
            x[i] = xx;
            y[i] = kHub75Margin + (max_h - ph[i]) / 2;
            xx += pw[i];
        }
    }
    int cw = 0, ch = 0;
    pf_hub75_canvas(L, cw, ch);
    const int cx = cw / 2, cy = ch / 2;
    for (int i = 0; i < n; ++i) {
        L.nudge_dx[i] = (x[i] + pw[i] / 2) - cx;
        L.nudge_dy[i] = (y[i] + ph[i] / 2) - cy;
    }
    for (int i = n; i < 4; ++i) {
        L.nudge_dx[i] = 0;
        L.nudge_dy[i] = 0;
    }
    L.defaults_applied = true;
}

// Total canvas size = bounding box of the *un-nudged* panel set (kHub75Margin
// is 0 — see the note above). Canvas size is therefore stable as the user
// tweaks nudges (so the renderer canvas doesn't resize every slider step) and
// equals the physical HUB75 framebuffer the driver pushes into.
void pf_hub75_canvas(const PfHub75Layout& L, int& cw, int& ch) {
    const int n = std::clamp(L.panel_count, 1, 4);
    int pw[4] = {0,0,0,0}, ph[4] = {0,0,0,0};
    for (int i = 0; i < 4; ++i) {
        const std::string& s = L.panel_size_per[i].empty()
                               ? L.panel_size : L.panel_size_per[i];
        pf_hub75_panel_dims(s, pw[i], ph[i]);
    }
    int bb_w = 0, bb_h = 0;
    if (L.arrangement == "vertical") {
        for (int i = 0; i < n; ++i) {
            bb_w = std::max(bb_w, pw[i]);
            bb_h += ph[i];
        }
    } else if (L.arrangement == "grid2x2") {
        const int row0_w = pw[0] + (n >= 2 ? pw[1] : 0);
        const int row1_w = (n >= 3 ? pw[2] : 0) + (n >= 4 ? pw[3] : 0);
        bb_w = std::max(row0_w, row1_w);
        const int row0_h = std::max(ph[0], n >= 2 ? ph[1] : 0);
        const int row1_h = std::max(n >= 3 ? ph[2] : 0, n >= 4 ? ph[3] : 0);
        bb_h = row0_h + row1_h;
    } else { // horizontal
        for (int i = 0; i < n; ++i) {
            bb_w += pw[i];
            bb_h = std::max(bb_h, ph[i]);
        }
    }
    cw = bb_w + 2 * kHub75Margin;
    ch = bb_h + 2 * kHub75Margin;
    if (cw <= 0) cw = 64;
    if (ch <= 0) ch = 32;
}

// Translate the panel layout into the piomatter geometry panel_driver.py needs
// so its framebuffer matches the (tight, margin-free) canvas the renderer
// pushes. The single physical panel size feeds --panel-w/--panel-h; the
// arrangement maps onto piomatter's chain (panels daisied along the data line)
// and parallel (independent lanes) counts:
//   horizontal → chain = N, parallel = 1   (canvas = pw*N × ph)
//   vertical   → chain = 1, parallel = N   (canvas = pw   × ph*N)
//   grid2x2    → chain = 2, parallel = 2   (canvas = pw*2 × ph*2)
// Mixed per-panel sizes can't be expressed as a uniform chain (piomatter
// assumes identical panels), so we fall back to panel 0's size; the canvas
// still matches because pf_hub75_canvas sums the real per-panel dims, and the
// driver derives missing geometry from the canvas as a backstop.
static void pf_hub75_driver_geometry(const PfHub75Layout& L,
                                     int& panel_w, int& panel_h,
                                     int& chain, int& parallel) {
    const int n = std::clamp(L.panel_count, 1, 4);
    const std::string& s0 = L.panel_size_per[0].empty()
                            ? L.panel_size : L.panel_size_per[0];
    pf_hub75_panel_dims(s0, panel_w, panel_h);
    if (L.arrangement == "vertical") {
        chain = 1; parallel = n;
    } else if (L.arrangement == "grid2x2") {
        chain = (n >= 2) ? 2 : 1;
        parallel = (n >= 3) ? 2 : 1;
    } else { // horizontal
        chain = n; parallel = 1;
    }
}

// ── Chain layout → named zone rects ───────────────────────────────────────────
// Translates the eye/mouth/nose layout pickers into the rectangles the face
// editor highlights. Coordinates per the project spec; the mirror axis is the
// nose's horizontal centre (fence between cols), so the right eye and right
// mouth land symmetrically around it. Nose can be omitted ("none"), in which
// case the mirror axis falls back to canvas_w/2. Mouth is anchored by its
// bottom-left corner (flush with the canvas bottom) so taller layouts don't
// run off the canvas.
struct PfFaceZones {
    std::vector<face::NamedRegion> regions;
    int                            mirror_x = 0;   // canvas col index used as the mirror "fence"
};

// Per-layout dimensions (pixels). Each pick string is "RxC" where R = rows
// of 8x8 modules and C = cols. Pixel size = (C*8) x (R*8).
static int pf_eye_w  (const std::string& l) { return (l == "1x3" || l == "2x3") ? 24 : 16; }
static int pf_eye_h  (const std::string& l) { return (l == "2x2" || l == "2x3") ? 16 :  8; }
static int pf_mouth_w(const std::string& l) { return (l == "1x4" || l == "2x4") ? 32 : 24; }
static int pf_mouth_h(const std::string& l) { return (l == "2x3" || l == "2x4") ? 16 :  8; }
static int pf_nose_w (const std::string& l) {
    if (l == "1x1") return  8;
    if (l == "1x2") return 16;
    if (l == "1x3") return 24;
    return 0;
}

// Spacing rules — keep zones from overlapping and centre everything around
// the nose, regardless of which picker the user changes:
//
//   eye_l   sits flush with col 0
//   nose    is centred on mirror_x (the canvas's symmetry axis)
//   eye_r   is the left eye mirrored around mirror_x
//   mouth_l has its inner (right) edge 8 px in from the centre
//   mouth_r is mouth_l mirrored
//
// mirror_x is sized to satisfy two constraints at once:
//   eye + gap + half-nose ≤ mirror_x    (8 px gap between eye and nose,
//                                        or between the two eyes if no nose)
//   mouth_w + 8           ≤ mirror_x    (8 px clearance from centre)
//
// Take the max so growing any chain just stretches the canvas; nothing
// ever overlaps.
static int pf_mirror_x(const std::string& eye_layout,
                       const std::string& mouth_layout,
                       const std::string& nose_layout)
{
    constexpr int GAP_EYE_NOSE   = 8;   // min gap between eye and nose (or between two eyes when nose=none)
    constexpr int MOUTH_INSET    = 8;   // mouth inner edge offset from canvas centre
    const int eye_w   = pf_eye_w(eye_layout);
    const int nose_w  = pf_nose_w(nose_layout);
    const int mouth_w = pf_mouth_w(mouth_layout);
    const int eyes_side  = eye_w + GAP_EYE_NOSE + nose_w / 2;
    const int mouth_side = mouth_w + MOUTH_INSET;
    return std::max(eyes_side, mouth_side);
}

// Canvas width for a given chain-layout set: just twice the mirror axis
// because every zone mirrors around it.
static int pf_canvas_w_for_layout(const std::string& eye_layout,
                                  const std::string& mouth_layout,
                                  const std::string& nose_layout)
{
    return 2 * pf_mirror_x(eye_layout, mouth_layout, nose_layout);
}

// Per-side module assignment used to auto-populate chains[] when the user
// hasn't authored their own. Each side becomes one physical daisy chain:
//   left  = eye_l + left-share-of-nose + mouth_l
//   right = eye_r + right-share-of-nose + mouth_r
// Nose split per user spec: 1→L, 2→L+R, 3→L+L+R (extra goes to the left).
// Modules listed in DIN→DOUT order: eye rows row-major, then nose, then mouth.
static PfSideChains pf_auto_side_chains(
        const std::string& eye_layout,
        const std::string& mouth_layout,
        const std::string& nose_layout,
        int canvas_h)
{
    PfSideChains s;
    const int eye_w   = pf_eye_w(eye_layout);
    const int eye_h   = pf_eye_h(eye_layout);
    const int nose_w  = pf_nose_w(nose_layout);
    const int mouth_w = pf_mouth_w(mouth_layout);
    const int mouth_h = pf_mouth_h(mouth_layout);
    const int mx      = pf_mirror_x(eye_layout, mouth_layout, nose_layout);
    const int eye_cols   = eye_w   / 8;
    const int eye_rows   = eye_h   / 8;
    const int mouth_cols = mouth_w / 8;
    const int mouth_rows = mouth_h / 8;
    const int mouth_y    = std::max(0, canvas_h - mouth_h);

    // Eyes: left at col 0, right mirrored. Row-major walk = top-row first.
    for (int r = 0; r < eye_rows; ++r)
        for (int c = 0; c < eye_cols; ++c)
            s.left.push_back({c * 8, r * 8});
    const int rey_x = 2 * mx - eye_w;
    for (int r = 0; r < eye_rows; ++r)
        for (int c = 0; c < eye_cols; ++c)
            s.right.push_back({rey_x + c * 8, r * 8});

    // Nose split — first ceil(N/2) modules go to the left chain.
    const int nose_cols   = nose_w / 8;
    const int nose_left_n = (nose_cols + 1) / 2;   // 1→1, 2→1, 3→2
    const int nose_x0     = mx - nose_w / 2;
    for (int c = 0; c < nose_cols; ++c) {
        const std::array<int, 2> pos = {nose_x0 + c * 8, 0};
        if (c < nose_left_n) s.left.push_back(pos);
        else                 s.right.push_back(pos);
    }

    // Mouth halves: mouth_l on left, mouth_r on right; row-major.
    constexpr int MOUTH_INSET = 8;
    const int ml_x = mx - MOUTH_INSET - mouth_w;
    const int mr_x = mx + MOUTH_INSET;
    for (int r = 0; r < mouth_rows; ++r) {
        for (int c = 0; c < mouth_cols; ++c)
            s.left.push_back({ml_x + c * 8, mouth_y + r * 8});
        for (int c = 0; c < mouth_cols; ++c)
            s.right.push_back({mr_x + c * 8, mouth_y + r * 8});
    }
    return s;
}

static PfFaceZones pf_compute_face_zones(
        const std::string& eye_layout,
        const std::string& mouth_layout,
        const std::string& nose_layout,
        int /*canvas_w*/,            // legacy; layouts now own the geometry
        int canvas_h)
{
    PfFaceZones out;
    auto& zones = out.regions;

    const int eye_w   = pf_eye_w(eye_layout);
    const int eye_h   = pf_eye_h(eye_layout);
    const int nose_w  = pf_nose_w(nose_layout);
    const int nose_h  = 8;
    const int mouth_w = pf_mouth_w(mouth_layout);
    const int mouth_h = pf_mouth_h(mouth_layout);

    out.mirror_x = pf_mirror_x(eye_layout, mouth_layout, nose_layout);
    const int mx = out.mirror_x;

    // Eyes — left at col 0, right mirrored around mx.
    zones.push_back({"eye_l", cv::Rect(0,            0, eye_w, eye_h)});
    zones.push_back({"eye_r", cv::Rect(2 * mx - eye_w, 0, eye_w, eye_h)});

    // Nose — centred on mx. Omitted when picker is "none".
    if (nose_w > 0) {
        zones.push_back({"nose", cv::Rect(mx - nose_w / 2, 0, nose_w, nose_h)});
    }

    // Mouth — split into two halves with their inner edges 8 px from the
    // centre, bottom-aligned to the canvas so taller layouts don't run off.
    const int mouth_y = std::max(0, canvas_h - mouth_h);
    constexpr int MOUTH_INSET = 8;
    const int ml_x = mx - MOUTH_INSET - mouth_w;
    const int mr_x = mx + MOUTH_INSET;
    zones.push_back({"mouth_l", cv::Rect(ml_x, mouth_y, mouth_w, mouth_h)});
    zones.push_back({"mouth_r", cv::Rect(mr_x, mouth_y, mouth_w, mouth_h)});

    return out;
}

static face::RenderConfig pf_build_render_config(const json& cfg,
                                                 const PfHub75Layout* hub75 = nullptr) {
    face::RenderConfig rc;
    const json* jpf = cfg.contains("protoface") ? &cfg["protoface"] : nullptr;

    std::string assets = "Protoface";
    if (const char* home = std::getenv("HOME"))
        assets = std::string(home) + "/protohud/Protoface";
    if (jpf && jpf->contains("assets_dir") &&
        !(*jpf)["assets_dir"].get<std::string>().empty())
        assets = (*jpf)["assets_dir"].get<std::string>();
    rc.faces_dir     = assets + "/faces";
    rc.materials_dir = assets + "/materials";
    rc.gifs_dir      = assets + "/gifs";

    if (jpf) {
        rc.canvas_w = jval(*jpf, "canvas_w", rc.canvas_w);
        rc.canvas_h = jval(*jpf, "canvas_h", rc.canvas_h);
        rc.fps      = jval(*jpf, "fps",      rc.fps);
        rc.continuous_effects = jval(*jpf, "continuous_effects", false);
        if (jpf->contains("faces_dir"))     rc.faces_dir     = (*jpf)["faces_dir"].get<std::string>();
        if (jpf->contains("materials_dir")) rc.materials_dir = (*jpf)["materials_dir"].get<std::string>();
        if (jpf->contains("gifs_dir"))      rc.gifs_dir      = (*jpf)["gifs_dir"].get<std::string>();
        if (jpf->contains("gif") && (*jpf)["gif"].is_object())
            rc.gif_auto_release = jval((*jpf)["gif"], "auto_release", rc.gif_auto_release);
    }

    auto parse_panel = [](const json& jp) {
        face::PanelCfg p;
        p.name = jp.value("name", std::string());
        if (jp.contains("region") && jp["region"].is_array() && jp["region"].size() == 4) {
            p.x = jp["region"][0].get<int>(); p.y = jp["region"][1].get<int>();
            p.w = jp["region"][2].get<int>(); p.h = jp["region"][3].get<int>();
        }
        p.mirror_of = jp.value("mirror_of", std::string());
        if (jp.contains("face")) {
            const auto& jf = jp["face"];
            p.face.active            = jf.value("active", p.face.active);
            p.face.expression_fade   = jval(jf, "expression_fade",   p.face.expression_fade);
            p.face.mouth_sensitivity = jval(jf, "mouth_sensitivity", p.face.mouth_sensitivity);
            if (jf.contains("wiggle")) {
                const auto& jw = jf["wiggle"];
                p.face.wiggle.speed       = jval(jw, "speed",       p.face.wiggle.speed);
                p.face.wiggle.amplitude_x = jval(jw, "amplitude_x", p.face.wiggle.amplitude_x);
                p.face.wiggle.amplitude_y = jval(jw, "amplitude_y", p.face.wiggle.amplitude_y);
            }
            if (jf.contains("blink")) {
                const auto& jb = jf["blink"];
                p.face.blink.interval_min = jval(jb, "interval_min", p.face.blink.interval_min);
                p.face.blink.interval_max = jval(jb, "interval_max", p.face.blink.interval_max);
                p.face.blink.duration     = jval(jb, "duration",     p.face.blink.duration);
            }
        }
        if (jp.contains("material")) {
            const auto& jm = jp["material"];
            p.material.active   = jm.value("active", p.material.active);
            p.material.scroll_x = jval(jm, "scroll_x", p.material.scroll_x);
            p.material.scroll_y = jval(jm, "scroll_y", p.material.scroll_y);
        }
        if (jp.contains("particles")) p.particles = jp["particles"];
        return p;
    };

    if (jpf && jpf->contains("panels") && (*jpf)["panels"].is_array() &&
        !(*jpf)["panels"].empty()) {
        for (const auto& jp : (*jpf)["panels"]) rc.panels.push_back(parse_panel(jp));
    } else if (hub75 && hub75->panel_count > 0) {
        // HUB75 picker mode — the face is drawn across all panels as ONE image.
        // Render it as a single logical panel covering the whole canvas (so the
        // material, particle effects and blink are continuous across the seam),
        // then split + flip per physical panel at output time.
        const auto plist = pf_hub75_panels(*hub75);
        int cw = 0, ch = 0;
        pf_hub75_canvas(*hub75, cw, ch);
        rc.canvas_w = cw;
        rc.canvas_h = ch;
        face::PanelCfg face;
        face.name = "face"; face.x = 0; face.y = 0; face.w = cw; face.h = ch;
        rc.panels.push_back(std::move(face));
        for (size_t i = 0; i < plist.size(); ++i) {
            const auto& p = plist[i];
            face::RenderConfig::OutputPanel op;
            op.x = p.rect.x; op.y = p.rect.y;
            op.w = p.rect.width; op.h = p.rect.height;
            if (i < 4) { op.flip_x = hub75->flip_x[i]; op.flip_y = hub75->flip_y[i]; }
            rc.output_panels.push_back(op);
        }
    } else {
        face::PanelCfg left;  left.name  = "face_left";  left.x = 0;  left.w = 64; left.h = 32;
        face::PanelCfg right; right.name = "face_right"; right.x = 64; right.w = 64; right.h = 32;
        right.mirror_of = "face_left";
        rc.panels.push_back(left);
        rc.panels.push_back(right);
    }
    return rc;
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

// Serialize the captured-QR running list to <qr_dir>/index.json. Caller must
// hold state.mtx (it reads the live log). The folders themselves hold the link
// + raw image; this index is the de-dupe list and the menu's source of truth.
void qr_write_index(const std::string& qr_dir, const QrCaptureLog& log) {
    if (qr_dir.empty()) return;
    json arr = json::array();
    for (const auto& c : log.items)
        arr.push_back({{"text", c.text}, {"type", c.type}, {"ts", c.timestamp},
                       {"folder", c.folder}, {"image", c.image}, {"decode", c.decode}});
    std::error_code ec;
    std::filesystem::create_directories(qr_dir, ec);
    std::ofstream f(std::filesystem::path(qr_dir) / "index.json");
    if (f) f << arr.dump();
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
    // Writing to a socket/pipe whose peer has gone away (Protoface restarted, a
    // serial device unplugged, etc.) must not kill the whole HUD — ignore SIGPIPE
    // and handle the -1/EPIPE return at each call site.
    std::signal(SIGPIPE, SIG_IGN);

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
    bool cfg_parse_failed = false;
    json cfg = load_config(cfg_load, &cfg_parse_failed);

    // ── Profiles ──────────────────────────────────────────────────────────────
    // A profile is a full config snapshot under <config>/profiles/<name>.json.
    // Applying one = relaunch ProtoHUD with that file as its config (see the
    // re-exec at shutdown). When we're launched WITH an explicit config path that
    // lives in the profiles dir, we're "running a profile" — skip the landing page.
    std::string exe_path;
    try { exe_path = fs::canonical(argv[0]).string(); }
    catch (...) { exe_path = argv[0]; }

    ProfileManager profiles;
    profiles.init(bin_dir + "/../config/profiles");

    // HUD/menu visual presets (separate, smaller files; applied live, not via restart).
    ProfileManager hud_presets;
    hud_presets.init(bin_dir + "/../config/presets");

    std::string active_profile_name;   // "" unless launched from a profile file
    if (argc > 1) {
        try {
            fs::path argp  = fs::weakly_canonical(fs::path(argv[1]));
            fs::path pdir  = fs::weakly_canonical(fs::path(profiles.dir()));
            if (argp.parent_path() == pdir && argp.extension() == ".json")
                active_profile_name = argp.stem().string();
        } catch (...) {}
    }

    // Where to send the process when the user picks/loads a profile (re-exec).
    std::string pending_reexec;

    // Continue-countdown on the landing page (auto-loads the last profile after
    // this many seconds of no input). 0 disables the countdown.
    double landing_continue_timeout_s =
        jval(cfg.contains("landing") ? cfg["landing"] : json::object(),
             "continue_timeout_s", 10.0);

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

    // ── Shared state + forward-declared backends ──────────────────────────
    // AppState lives here so the config-parse blocks below can write into
    // it. The four pointer-typed slots below get forward-declared as the
    // canonical sources of truth so callbacks/lambdas constructed during
    // module wire-up can capture them by reference even though their
    // final values aren't known yet — the assignment lines further down
    // (look for `active_face = …`, `menu_ptr = &menu`, etc.) populate them
    // once their owning objects exist.
    AppState state;

    // ── Notification log persistence ──────────────────────────────────────────
    // Save the notification queue next to config.json so a sudden reboot doesn't
    // lose it. Loaded once here; saved (debounced) from the render loop + on exit.
    const std::string notif_path =
        (fs::path(cfg_path).parent_path() / "protohud_notifications.json").string();
    if (cfg.contains("notifications") && cfg["notifications"].is_object())
        state.notif_persist = cfg["notifications"].value("persist", true);
    auto save_notifs = [&state, notif_path](bool async = true){
        json arr = json::array();
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            for (const auto& n : state.notifs.items)
                arr.push_back({{"type", static_cast<int>(n.type)}, {"title", n.title},
                               {"body", n.body}, {"ts", n.timestamp},
                               {"read", n.read}, {"dismissed", n.dismissed},
                               {"saved", n.saved}});
        }
        // The debounced caller is the render loop, and a busy SD card can
        // stall a write for tens of ms — push the file I/O off-thread. The
        // shutdown call passes async=false so the final save can't be lost.
        auto write = [notif_path, payload = arr.dump()]{
            std::ofstream f(notif_path);
            if (f) f << payload;
        };
        if (async) std::thread(std::move(write)).detach();
        else       write();
    };
    if (state.notif_persist) {
        std::ifstream f(notif_path);
        json arr;
        if (f) { try { f >> arr; } catch (...) { arr = json::array(); } }
        if (arr.is_array()) {
            std::lock_guard<std::mutex> lk(state.mtx);
            for (auto it = arr.rbegin(); it != arr.rend(); ++it) {   // file is newest-first
                if (!it->is_object()) continue;
                Notification n;
                n.type      = static_cast<NotifType>(it->value("type", static_cast<int>(NotifType::App)));
                n.title     = it->value("title", std::string());
                n.body      = it->value("body",  std::string());
                n.timestamp = it->value("ts", static_cast<int64_t>(0));
                n.saved     = it->value("saved", false);
                n.read      = true;    // loaded entries are history — never re-toast on boot
                n.dismissed = true;
                state.notifs.push(std::move(n));
            }
        }
    }

    IFaceController* active_face      = nullptr;   // set after native_ctrl/teensy/protoface_ctrl exist
    FaceProxy        face_proxy(&active_face);     // proxy reads *active_face each call
    MenuSystem*      menu_ptr         = nullptr;   // set after MenuSystem is constructed
    sensor::BoopSensor* boop_sensor_ptr = nullptr; // set after boop_sensor is constructed

    // Voice → mouth_open analysis (FFT band RMS, envelope follower).
    if (cfg.contains("voice_mouth")) {
        auto& jv = cfg["voice_mouth"];
        state.voice_mouth.enabled     = jval(jv, "enabled",     state.voice_mouth.enabled);
        state.voice_mouth.sensitivity = jval(jv, "sensitivity", state.voice_mouth.sensitivity);
        state.voice_mouth.noise_gate  = jval(jv, "noise_gate",  state.voice_mouth.noise_gate);
        state.voice_mouth.attack_ms   = jval(jv, "attack_ms",   state.voice_mouth.attack_ms);
        state.voice_mouth.release_ms  = jval(jv, "release_ms",  state.voice_mouth.release_ms);
        state.voice_mouth.band_lo_hz  = jval(jv, "band_lo_hz",  state.voice_mouth.band_lo_hz);
        state.voice_mouth.band_hi_hz  = jval(jv, "band_hi_hz",  state.voice_mouth.band_hi_hz);
        state.voice_mouth.visemes_enabled     = jval(jv, "visemes_enabled",     state.voice_mouth.visemes_enabled);
        state.voice_mouth.viseme_round_max_hz = jval(jv, "viseme_round_max_hz", state.voice_mouth.viseme_round_max_hz);
        state.voice_mouth.viseme_open_max_hz  = jval(jv, "viseme_open_max_hz",  state.voice_mouth.viseme_open_max_hz);
        state.voice_mouth.viseme_small_max_hz = jval(jv, "viseme_small_max_hz", state.voice_mouth.viseme_small_max_hz);
    }

    XRConfig xr_cfg;
    xr_cfg.product_id       = jval(jvtr,  "product_id",       0);
    xr_cfg.monitor_index    = jval(jvtr,  "monitor_index",    -1);
    xr_cfg.target_fps       = jval(jdisp, "target_fps",       90);
    xr_cfg.sbs_height       = jval(jdisp, "sbs_height",       1200);
    xr_cfg.use_beast_camera = jval(jvtr,  "use_beast_camera", true);
    xr_cfg.enable_imu       = jval(jvtr,  "enable_imu",       true);
    xr_cfg.frameless        = jval(jdisp, "frameless",         false);
    xr_cfg.fullscreen       = jval(jdisp, "fullscreen",        false);

    CamConfig owl_left, owl_right;

    if (jcam.contains("owlsight_left")) {
        auto& jl              = jcam["owlsight_left"];
        owl_left.libcamera_id = jl.value("libcamera_id", 0);
        owl_left.model_name   = jl.value("model_name",   std::string(""));
        owl_left.width        = jl.value("width",  1280);
        owl_left.height       = jl.value("height",  800);
        owl_left.fps          = jl.value("fps",      60);
        owl_left.rotation_deg = jl.value("rotation_deg", 0);
        parse_cam_controls(jl, state.camera_controls_left);
    }
    if (jcam.contains("owlsight_right")) {
        auto& jr               = jcam["owlsight_right"];
        owl_right.libcamera_id = jr.value("libcamera_id", 1);
        owl_right.model_name   = jr.value("model_name",   std::string(""));
        owl_right.width        = jr.value("width",  1280);
        owl_right.height       = jr.value("height",  800);
        owl_right.fps          = jr.value("fps",      60);
        owl_right.rotation_deg = jr.value("rotation_deg", 0);
        parse_cam_controls(jr, state.camera_controls_right);
    }
    // Back-compat: older builds persisted a single top-level "resolution" block
    // that forced BOTH eyes to one value. Newer builds save per-eye under
    // owlsight_left/right and drop this block on save, so this only fires for
    // configs written before per-camera resolution existed.
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
        usb1_cfg.width              = j1.value("width",                640);
        usb1_cfg.height             = j1.value("height",               360);
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
        usb2_cfg.width              = j2.value("width",                640);
        usb2_cfg.height             = j2.value("height",               360);
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
        usb3_cfg.width              = j3.value("width",                640);
        usb3_cfg.height             = j3.value("height",               360);
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

    // Eye source: which camera feeds each eye (CSI or one of the USB slots).
    auto parse_eye_src = [](const std::string& s) -> EyeSource {
        if (s == "usb1") return EyeSource::USB1;
        if (s == "usb2") return EyeSource::USB2;
        if (s == "usb3") return EyeSource::USB3;
        if (s == "csi_left")  return EyeSource::CSI_LEFT;
        if (s == "csi_right") return EyeSource::CSI_RIGHT;
        return EyeSource::CSI;
    };
    EyeSource left_eye_src  = parse_eye_src(jcam.value("left_eye_source",  std::string("csi")));
    EyeSource right_eye_src = parse_eye_src(jcam.value("right_eye_source", std::string("csi")));

    // Multi-cam quad layout: CSI L/R on top, two USB on the bottom, same
    // composite in both eyes. multicam_usb_{a,b} pick which two USB slots fill
    // the bottom-left / bottom-right quadrants (kept distinct by the menu).
    bool      multicam_layout = jcam.value("multicam_layout", false);
    EyeSource multicam_usb_a  = parse_eye_src(jcam.value("multicam_usb_a", std::string("usb1")));
    EyeSource multicam_usb_b  = parse_eye_src(jcam.value("multicam_usb_b", std::string("usb2")));
    // Top-left / top-right quadrant sources — now selectable (default the two CSI
    // cameras, one on each side); may also be set to a USB cam.
    EyeSource multicam_top_a  = parse_eye_src(jcam.value("multicam_top_a", std::string("csi_left")));
    EyeSource multicam_top_b  = parse_eye_src(jcam.value("multicam_top_b", std::string("csi_right")));

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

    Bno055::Config bno_cfg;
    if (cfg.contains("bno055")) {
        auto& jb = cfg["bno055"];
        bno_cfg.enabled         = jval(jb, "enabled",         false);
        bno_cfg.i2c_bus         = jb.value("i2c_bus",         std::string("/dev/i2c-1"));
        bno_cfg.i2c_addr        = jval(jb, "i2c_addr",        0x28);
        bno_cfg.declination_deg = jval(jb, "declination_deg", 0.0f);
        bno_cfg.heading_offset  = jval(jb, "heading_offset",  0.0f);
        bno_cfg.heading_axes    = jval(jb, "heading_axes",    0);
        bno_cfg.poll_hz         = jval(jb, "poll_hz",         50.0f);
        bno_cfg.auto_save_calibration = jval(jb, "save_calibration", true);
        // Transport: "i2c" (default) or "uart" (PS1→3.3V; sidesteps the Pi's
        // I²C clock-stretch trouble). uart_device + uart_baud apply in UART mode.
        bno_cfg.transport       = jb.value("transport",   std::string("i2c"));
        bno_cfg.uart_device     = jb.value("uart_device", std::string("/dev/ttyAMA0"));
        bno_cfg.uart_baud       = jval(jb, "uart_baud",   115200);
    }
    // Persist the BNO055 calibration profile next to config.json so the sensor
    // keeps its calibration across reboots (restored at start()).
    bno_cfg.calib_path =
        (fs::path(cfg_path).parent_path() / "bno055_calib.bin").string();

    // BNO086 (SH-2, over I2C). Magnetometer-referenced heading (no yaw drift)
    // plus, when head_tracking is on, roll/pitch/yaw for the imu_pose head
    // tracking path. INT/RST GPIO offsets are optional (INT-less falls back to
    // bus polling; RST-less skips the hardware reset).
    Bno08x::Config bno08x_cfg;
    if (cfg.contains("bno086")) {
        auto& jb = cfg["bno086"];
        bno08x_cfg.enabled            = jval(jb, "enabled",            false);
        bno08x_cfg.i2c_bus            = jb.value("i2c_bus", std::string("/dev/i2c-1"));
        bno08x_cfg.i2c_addr           = jval(jb, "i2c_addr",           0x4A);
        bno08x_cfg.gpiochip           = jb.value("gpiochip", std::string("/dev/gpiochip0"));
        bno08x_cfg.int_line           = jval(jb, "int_line",           -1);
        bno08x_cfg.rst_line           = jval(jb, "rst_line",           -1);
        bno08x_cfg.report_interval_us = jval(jb, "report_interval_us", 10000);
        bno08x_cfg.declination_deg    = jval(jb, "declination_deg",    0.0f);
        bno08x_cfg.heading_offset     = jval(jb, "heading_offset",     0.0f);
        bno08x_cfg.heading_invert     = jval(jb, "heading_invert",     false);
        bno08x_cfg.head_tracking      = jval(jb, "head_tracking",      false);
    }

    // IMU source selector — replaces the old "Viture always wins, MPU is
    // backup" hardcoded priority. "auto" picks the best fresh source per
    // frame (BNO055 > MPU9250 > Viture); explicit choices force that
    // source even if others are connected.
    if (cfg.contains("imu_source")) {
        const std::string s = cfg.value("imu_source", std::string("auto"));
        if      (s == "bno086" || s == "bno08x")  state.imu_source = AppState::ImuSource::Bno08x;
        else if (s == "bno055" || s == "bno")     state.imu_source = AppState::ImuSource::Bno055;
        else if (s == "mpu9250" || s == "mpu")    state.imu_source = AppState::ImuSource::Mpu9250;
        else if (s == "viture"  || s == "xr")     state.imu_source = AppState::ImuSource::Viture;
        else if (s == "none"    || s == "off")    state.imu_source = AppState::ImuSource::None;
        else                                      state.imu_source = AppState::ImuSource::Auto;
    }

    // ── Accessory LEDs (cheekhubs + fins on WS2812 daisy-chain) ──────────────
    // Single chain driven through Pi 5 SPI MOSI (GPIO 10). Zone slicing is
    // declarative — config picks {start, count} per zone (LeftCheekhub,
    // RightCheekhub, LeftFin, RightFin); patterns are per-zone (Off / Solid /
    // Breathe in v1, audio + event hooks later).
    accessory::AccessoryLeds::Config led_cfg;
    led_cfg.zones[0].name = "left_cheekhub";
    led_cfg.zones[1].name = "right_cheekhub";
    led_cfg.zones[2].name = "left_fin";
    led_cfg.zones[3].name = "right_fin";
    if (cfg.contains("accessory_leds")) {
        auto& jl = cfg["accessory_leds"];
        led_cfg.enabled            = jval(jl, "enabled",           false);
        led_cfg.global_brightness  = static_cast<uint8_t>(
            jval(jl, "global_brightness", static_cast<int>(led_cfg.global_brightness)));
        led_cfg.frame_hz           = jval(jl, "frame_hz",          60.0);
        led_cfg.strip.spi_device   = jl.value("spi_device",        std::string("/dev/spidev0.0"));
        led_cfg.strip.speed_hz     = jval(jl, "speed_hz",          2'400'000);
        if (auto co = jl.value("color_order", std::string("GRB")); co == "RGB")
            led_cfg.strip.color_order = accessory::LedStrip::ColorOrder::RGB;
        else if (co == "BGR")
            led_cfg.strip.color_order = accessory::LedStrip::ColorOrder::BGR;
        else
            led_cfg.strip.color_order = accessory::LedStrip::ColorOrder::GRB;
        if (jl.contains("zones") && jl["zones"].is_array()) {
            for (size_t i = 0; i < jl["zones"].size() && i < accessory::ZoneCount; ++i) {
                auto& jz = jl["zones"][i];
                led_cfg.zones[i].start = jval(jz, "start", led_cfg.zones[i].start);
                led_cfg.zones[i].count = jval(jz, "count", led_cfg.zones[i].count);
                if (jz.contains("color") && jz["color"].is_array() && jz["color"].size() >= 3) {
                    led_cfg.zones[i].r = static_cast<uint8_t>(std::clamp(jz["color"][0].get<int>(), 0, 255));
                    led_cfg.zones[i].g = static_cast<uint8_t>(std::clamp(jz["color"][1].get<int>(), 0, 255));
                    led_cfg.zones[i].b = static_cast<uint8_t>(std::clamp(jz["color"][2].get<int>(), 0, 255));
                }
                const std::string pat = jz.value("pattern", std::string("solid"));
                if      (pat == "off")     led_cfg.zones[i].pattern = accessory::Pattern::Off;
                else if (pat == "breathe") led_cfg.zones[i].pattern = accessory::Pattern::Breathe;
                else                       led_cfg.zones[i].pattern = accessory::Pattern::Solid;
                led_cfg.zones[i].breathe_hz =
                    jval(jz, "breathe_hz", led_cfg.zones[i].breathe_hz);
            }
        }
        // Strip length = highest (start + count) across configured zones; lets
        // users add or trim zones without separately bookkeeping a total.
        int max_end = 0;
        for (const auto& z : led_cfg.zones) max_end = std::max(max_end, z.start + z.count);
        led_cfg.strip.count = max_end;
    }
    accessory::AccessoryLeds accessory_leds(led_cfg);

    // ── Cooling fans (Pi-driven software-PWM on GPIO) ─────────────────────────
    sys::FanController::Config fan_cfg;
    if (cfg.contains("fans") && cfg["fans"].is_object()) {
        const auto& jf = cfg["fans"];
        fan_cfg.enabled    = jval(jf, "enabled", false);
        fan_cfg.chip       = jf.value("chip", fan_cfg.chip);
        fan_cfg.pwm_hz     = jval(jf, "pwm_hz",   fan_cfg.pwm_hz);
        fan_cfg.min_duty   = jval(jf, "min_duty", fan_cfg.min_duty);
        fan_cfg.invert     = jval(jf, "invert",   fan_cfg.invert);
        fan_cfg.temp_path  = jf.value("temp_path", fan_cfg.temp_path);
        auto parse_zone = [&](const json& jz, const char* defname) {
            sys::FanController::ZoneCfg z;
            z.name = jz.value("name", std::string(defname));
            if (jz.contains("gpios") && jz["gpios"].is_array())
                for (const auto& g : jz["gpios"]) if (g.is_number()) z.gpios.push_back(g.get<int>());
            else if (jz.contains("gpio") && jz["gpio"].is_number())
                z.gpios.push_back(jz["gpio"].get<int>());
            z.auto_mode  = (jz.value("mode", std::string("manual")) == "auto");
            z.speed      = jval(jz, "speed",      z.speed);
            z.auto_min_c = jval(jz, "auto_min_c", z.auto_min_c);
            z.auto_max_c = jval(jz, "auto_max_c", z.auto_max_c);
            return z;
        };
        if (jf.contains("zones") && jf["zones"].is_array()) {
            int zi = 0;
            for (const auto& jz : jf["zones"]) {
                if (zi >= sys::FanController::kMaxZones) break;
                if (!jz.is_object()) continue;
                char dn[16]; std::snprintf(dn, sizeof(dn), "Zone %d", zi + 1);
                fan_cfg.zones.push_back(parse_zone(jz, dn));
                ++zi;
            }
        } else if (jf.contains("gpios") || jf.contains("gpio")) {
            fan_cfg.zones.push_back(parse_zone(jf, "Zone 1"));   // legacy single-zone
        }
    }
    sys::FanController cooling_fans(fan_cfg);
    if (fan_cfg.enabled && !cooling_fans.start())
        std::cerr << "[main] cooling fans unavailable (check fans.gpios / pin conflicts)\n";

    // ── Boop sensor (MPR121 capacitive over I²C) ─────────────────────────────
    // Per-zone user-facing config (expression, duration, sensitivity) lives in
    // state.boop_zones — that's what the menu mutates. Hardware-level config
    // (bus, address, electrode-to-zone mapping) lives in the sensor's Config
    // and is loaded once at startup from config.json's "boop" object.
    sensor::Mpr121BoopSensor::Config boop_cfg;
    if (cfg.contains("boop")) {
        auto& jb = cfg["boop"];
        boop_cfg.enabled          = jval(jb, "enabled",          false);
        boop_cfg.i2c_bus          = jb.value("i2c_bus",          std::string("/dev/i2c-1"));
        boop_cfg.i2c_addr         = jval(jb, "i2c_addr",         0x5A);
        boop_cfg.coalesce_window_s = jval(jb, "coalesce_window_s", boop_cfg.coalesce_window_s);
        state.boop_coalesce_window_s = static_cast<float>(boop_cfg.coalesce_window_s);
        if (jb.contains("zones") && jb["zones"].is_array()) {
            // Up to 4 zones now: [0]=Snout, [1]=LeftCheek, [2]=RightCheek,
            // [3]=BothCheeks. BothCheeks is derived (no electrode probe),
            // so its electrode field stays at -1 even if config sets it.
            for (size_t i = 0; i < jb["zones"].size() && i < 4; ++i) {
                const auto& jz = jb["zones"][i];
                // Electrode mapping is editable in the menu (state.boop_zones)
                // and mirrored into the sensor config consumed at start().
                state.boop_zones[i].electrode =
                    jval(jz, "electrode", state.boop_zones[i].electrode);
                boop_cfg.electrode[i] = static_cast<int8_t>(state.boop_zones[i].electrode);
                state.boop_zones[i].expression =
                    jz.value("expression", state.boop_zones[i].expression);
                state.boop_zones[i].duration_s =
                    jval(jz, "duration_s", state.boop_zones[i].duration_s);
                state.boop_zones[i].threshold =
                    static_cast<uint8_t>(jval(jz, "threshold", static_cast<int>(state.boop_zones[i].threshold)));
                state.boop_zones[i].enabled =
                    jval(jz, "enabled", state.boop_zones[i].enabled);
                if (jz.contains("eye_trigger") && jz["eye_trigger"].is_object()) {
                    const auto& je = jz["eye_trigger"];
                    auto& et = state.boop_zones[i].eye_trigger;
                    et.enabled    = jval(je, "enabled",    et.enabled);
                    et.count      = std::clamp(jval(je, "count", et.count), 2, 10);
                    et.window_s   = jval(je, "window_s",   et.window_s);
                    et.anim       = std::clamp(jval(je, "anim", et.anim),
                                               0, face::eye_anim_count() - 1);
                    et.speed      = jval(je, "speed",      et.speed);
                    et.size       = jval(je, "size",       et.size);
                    et.duration_s = jval(je, "duration_s", et.duration_s);
                    if (je.contains("color") && je["color"].is_array() &&
                        je["color"].size() >= 3) {
                        et.r = static_cast<uint8_t>(std::clamp(je["color"][0].get<int>(), 0, 255));
                        et.g = static_cast<uint8_t>(std::clamp(je["color"][1].get<int>(), 0, 255));
                        et.b = static_cast<uint8_t>(std::clamp(je["color"][2].get<int>(), 0, 255));
                    }
                }
                boop_cfg.touch_threshold[i] = state.boop_zones[i].threshold;
                boop_cfg.zone_enabled[i]    = state.boop_zones[i].enabled;
            }
            // Lock the BothCheeks electrode at -1 regardless of what the
            // config tried to set — it's derived, never a direct probe.
            boop_cfg.electrode[static_cast<size_t>(sensor::BoopSensor::Zone::BothCheeks)] = -1;
        }
    }

    AndroidMirrorConfig and_cfg;
    OverlayConfig       pip_overlay_cfg1, pip_overlay_cfg2, pip_overlay_cfg3;
    OverlayConfig       android_overlay_cfg;
    OverlayConfig       protoface_preview_cfg;          // LED preview anchor/nudge/size
    protoface_preview_cfg.anchor_x = 1.0f;              // default: top-right (prior behaviour)
    protoface_preview_cfg.anchor_y = 0.0f;
    protoface_preview_cfg.size     = 0.15f;
    int protoface_preview_view = 0;                     // 0=whole face, 1=left, 2=right

    // Protoface options: auto-start the daemon on boot and a persisted preview
    // window placement (anchor/nudge/size/view) so it survives a restart.
    bool pf_autostart = true;
    // Face backend: "daemon" = the Protoface Python daemon over socket+shm
    // (default); "native" = render the face in-process via NativeFaceController.
    std::string pf_mode = "daemon";
    // In native mode, auto-launch scripts/panel_driver.py to push frames to the
    // HUB75 panels (set false if you run the driver yourself or only want preview).
    bool pf_launch_driver = true;
    // Backend selects which LED hardware NativeFaceController writes into:
    //   "hub75"   — HUB75 panels via the Python piomatter shim (default)
    //   "max7219" — direct SPI to one or more MAX7219 daisy-chains; the
    //               Python driver isn't needed and we don't launch it.
    std::string pf_backend = "hub75";
    // Chain layout pickers — drive both the editor's per-zone bounding
    // boxes (Left/Right Eye, Nose, Mouth) and the chain rects passed to
    // the panel output. Strings (not enums) for cfg round-trip simplicity:
    //   eye:   "1x2" | "1x3" | "2x2" | "2x3"
    //   mouth: "1x3" | "1x4" | "2x3" | "2x4"
    //   nose:  "none" | "1x1" | "1x2" | "1x3"
    std::string pf_eye_layout   = "1x2";
    std::string pf_mouth_layout = "1x3";
    std::string pf_nose_layout  = "1x1";
    // HUB75 panel layout — picker state for users on the HUB75 backend.
    // Empty (count == 0) keeps the legacy face_left/face_right pair so old
    // configs are unchanged. Once the user picks a layout it persists to
    // cfg["protoface"]["hub75"].
    PfHub75Layout pf_hub75;
    // Named HUB75 layouts — users can save multiple panel-arrangement profiles
    // ("Default", "ConTour", "CM5 Helmet", …) and switch between them. The
    // working `pf_hub75` is the live edit copy of pf_hub75_layouts[
    // pf_hub75_active]; save_cfg + every layout-management action sync the
    // two. Faces created while a given layout is active are stamped with its
    // name (NativeFaceController::set_active_layout_name + import_face_image).
    std::map<std::string, PfHub75Layout> pf_hub75_layouts;
    std::string pf_hub75_active = "Default";
    // Custom Gradient material editor state (Protoface > Material Color).
    PfGradient pf_gradient;
    // "Alive" reactive pack (Face > Effects): gravity/slosh-coupled particles
    // and the ambient weather-driven effect override. weather_fx_resync forces
    // an immediate re-evaluation when the toggle flips.
    bool pf_motion_particles = true;
    bool pf_weather_effects  = false;
    bool weather_fx_resync   = true;
    // Glitch post-effect config — forwarded to the native controller live and
    // persisted to cfg["protoface"]["glitch"]. Tunable via the settings JSON;
    // every option (chromatic, tearing, blocks, bitcrush, dropout, datamosh,
    // region_desync, expr_flicker) is an independent variable.
    face::GlitchConfig pf_glitch;
    // Face animation tunables — forwarded to every panel's FaceState live
    // and persisted to cfg["protoface"]["animation"] on save.
    bool   pf_blink_enabled   = true;
    double pf_blink_min       = 3.0;
    double pf_blink_max       = 7.0;
    double pf_blink_duration  = 0.15;
    double pf_expr_fade       = 0.3;
    // V (Preview) hold time in the face editor — pushes the in-progress canvas
    // onto the physical panels for this many seconds, then auto-restores.
    double pf_preview_duration_s = 10.0;
    if (cfg.contains("protoface")) {
        auto& jpf = cfg["protoface"];
        pf_autostart     = jval(jpf, "autostart", true);
        pf_mode          = jpf.value("mode", std::string("daemon"));
        pf_launch_driver = jval(jpf, "panel_driver", true);
        pf_backend       = jpf.value("backend", std::string("hub75"));
        state.face.face_colors = jval(jpf, "face_colors", false);
        state.face.pride_sharp = jval(jpf, "pride_sharp", true);
        pf_motion_particles    = jval(jpf, "motion_particles", pf_motion_particles);
        pf_weather_effects     = jval(jpf, "weather_effects",  pf_weather_effects);
        state.face.pride_angle = jval(jpf, "pride_angle", 90);
        if (jpf.contains("layout") && jpf["layout"].is_object()) {
            auto& jl = jpf["layout"];
            pf_eye_layout   = jl.value("eye",   pf_eye_layout);
            pf_mouth_layout = jl.value("mouth", pf_mouth_layout);
            pf_nose_layout  = jl.value("nose",  pf_nose_layout);
        }
        auto parse_hub75_layout = [](const json& jh, PfHub75Layout L) -> PfHub75Layout {
            if (!jh.is_object()) return L;
            L.panel_size  = jh.value("panel_size",  L.panel_size);
            L.arrangement = jh.value("arrangement", L.arrangement);
            L.panel_count = jval(jh, "panel_count", L.panel_count);
            L.pinout      = jh.value("pinout",      L.pinout);
            L.color_order = jh.value("color_order", L.color_order);
            L.camera_mode            = jval(jh, "camera_mode",            L.camera_mode);
            L.camera_planes          = jval(jh, "camera_planes",          L.camera_planes);
            L.camera_temporal_planes = jval(jh, "camera_temporal_planes", L.camera_temporal_planes);
            if (jh.contains("panel_size_per") && jh["panel_size_per"].is_array())
                for (size_t i = 0; i < jh["panel_size_per"].size() && i < 4; ++i)
                    if (jh["panel_size_per"][i].is_string())
                        L.panel_size_per[i] = jh["panel_size_per"][i].get<std::string>();
            if (jh.contains("nudge_dx") && jh["nudge_dx"].is_array())
                for (size_t i = 0; i < jh["nudge_dx"].size() && i < 4; ++i)
                    L.nudge_dx[i] = jh["nudge_dx"][i].get<int>();
            if (jh.contains("nudge_dy") && jh["nudge_dy"].is_array())
                for (size_t i = 0; i < jh["nudge_dy"].size() && i < 4; ++i)
                    L.nudge_dy[i] = jh["nudge_dy"][i].get<int>();
            if (jh.contains("flip_x") && jh["flip_x"].is_array())
                for (size_t i = 0; i < jh["flip_x"].size() && i < 4; ++i)
                    L.flip_x[i] = jh["flip_x"][i].get<bool>();
            if (jh.contains("flip_y") && jh["flip_y"].is_array())
                for (size_t i = 0; i < jh["flip_y"].size() && i < 4; ++i)
                    L.flip_y[i] = jh["flip_y"][i].get<bool>();
            L.defaults_applied = jval(jh, "defaults_applied", false);
            return L;
        };
        // Multi-layout schema (preferred): hub75_layouts is a name→layout map,
        // hub75_active picks the working one. Falls back to the legacy single
        // hub75 block when those aren't present (one-time migration to
        // hub75_layouts["Default"] on the next save).
        if (jpf.contains("hub75_layouts") && jpf["hub75_layouts"].is_object()) {
            for (auto& [name, jh] : jpf["hub75_layouts"].items())
                pf_hub75_layouts[name] = parse_hub75_layout(jh, PfHub75Layout{});
            pf_hub75_active = jpf.value("hub75_active", pf_hub75_active);
            if (!pf_hub75_layouts.count(pf_hub75_active))
                pf_hub75_active = pf_hub75_layouts.begin()->first;
            pf_hub75 = pf_hub75_layouts[pf_hub75_active];
        } else if (jpf.contains("hub75") && jpf["hub75"].is_object()) {
            pf_hub75 = parse_hub75_layout(jpf["hub75"], pf_hub75);
        }
        if (jpf.contains("animation") && jpf["animation"].is_object()) {
            auto& ja = jpf["animation"];
            pf_blink_enabled  = jval(ja, "blink_enabled",  pf_blink_enabled);
            pf_blink_min      = jval(ja, "blink_min",      pf_blink_min);
            pf_blink_max      = jval(ja, "blink_max",      pf_blink_max);
            pf_blink_duration = jval(ja, "blink_duration", pf_blink_duration);
            pf_expr_fade      = jval(ja, "expression_fade", pf_expr_fade);
            pf_preview_duration_s =
                jval(ja, "preview_duration_s", pf_preview_duration_s);
        }
        if (jpf.contains("glitch") && jpf["glitch"].is_object())
            pf_glitch = face::GlitchConfig::from_json(jpf["glitch"]);
        if (jpf.contains("gradient") && jpf["gradient"].is_object()) {
            auto& jg = jpf["gradient"];
            pf_gradient.count     = std::clamp(jval(jg, "count", pf_gradient.count), 2, 6);
            pf_gradient.smooth    = jval(jg, "smooth", pf_gradient.smooth);
            if (jg.contains("angle"))
                pf_gradient.angle = jval(jg, "angle", pf_gradient.angle);
            else if (jg.contains("direction"))   // migrate the old horizontal/vertical key
                pf_gradient.angle = (jg.value("direction", std::string("horizontal")) == "vertical")
                                    ? 90 : 0;
            pf_gradient.speed     = jval(jg, "speed", pf_gradient.speed);
            pf_gradient.mirror    = jval(jg, "mirror", pf_gradient.mirror);
            if (jg.contains("colors") && jg["colors"].is_array()) {
                for (size_t i = 0; i < jg["colors"].size() && i < 6; ++i) {
                    const auto& jc = jg["colors"][i];
                    if (jc.is_array() && jc.size() == 3)
                        for (int k = 0; k < 3; ++k)
                            pf_gradient.colors[i][k] =
                                std::clamp(jc[k].get<int>(), 0, 255);
                }
            }
        }
        if (jpf.contains("preview")) {
            auto& jpv = jpf["preview"];
            protoface_preview_cfg.anchor_x = jval(jpv, "anchor_x", protoface_preview_cfg.anchor_x);
            protoface_preview_cfg.anchor_y = jval(jpv, "anchor_y", protoface_preview_cfg.anchor_y);
            protoface_preview_cfg.pan_x    = jval(jpv, "pan_x",    protoface_preview_cfg.pan_x);
            protoface_preview_cfg.pan_y    = jval(jpv, "pan_y",    protoface_preview_cfg.pan_y);
            protoface_preview_cfg.size     = jval(jpv, "size",     protoface_preview_cfg.size);
            protoface_preview_view         = jval(jpv, "view",     protoface_preview_view);
        }
    }
    // Centre-offset model needs an initial seed when cfg didn't carry one
    // (first run, or upgrading from the old "delta from base" schema
    // where all-zero nudges would now stack panels at the centre).
    if (!pf_hub75.defaults_applied) pf_hub75_apply_defaults(pf_hub75);
    // Seed the named-layouts map on first launch (or after migrating from
    // the legacy single-block schema) so the menu always has something
    // selectable. Whichever was loaded into pf_hub75 above becomes the
    // initial Default.
    if (pf_hub75_layouts.empty()) {
        pf_hub75_layouts[pf_hub75_active] = pf_hub75;
    } else {
        pf_hub75_layouts[pf_hub75_active] = pf_hub75;   // keep the map in sync
    }

    // Scheduler / reminders. Networking lives in the scheduler_daemon companion;
    // ProtoHUD only reads the merged events.json + scheduler_status.json it writes.
    bool        sched_enabled    = true;
    bool        sched_autostart  = true;   // default: launch the daemon with the HUD
    int         sched_poll_s     = 20;
    int         sched_lead_min   = 10;   // applied to state after it's constructed
    // Default the data dir under the real $HOME so it matches the daemon (which
    // uses ~/.local/share). A hardcoded /home/user broke this on other accounts.
    const std::string sched_home = []{
        const char* h = std::getenv("HOME");
        return std::string(h && *h ? h : "/home/user");
    }();
    std::string sched_events_path = sched_home + "/.local/share/protohud-scheduler/events.json";
    std::string sched_status_path = sched_home + "/.local/share/protohud-scheduler/scheduler_status.json";
    if (cfg.contains("scheduler")) {
        auto& jsc = cfg["scheduler"];
        sched_enabled     = jval(jsc, "enabled",         sched_enabled);
        sched_autostart   = jval(jsc, "autostart",       sched_autostart);
        sched_poll_s      = jval(jsc, "poll_interval_s", sched_poll_s);
        sched_events_path = jval(jsc, "events_path",     sched_events_path);
        sched_status_path = jval(jsc, "status_path",     sched_status_path);
        sched_lead_min    = jval(jsc, "lead_minutes",    sched_lead_min);
        state.sched_send_link_startup = jval(jsc, "send_link_on_startup", false);
    }

    // KDE Connect bridge — RX phone notifications via DBus session bus.
    // The bridge owns its own worker thread and no-ops cleanly when
    // kdeconnectd isn't running, so non-Pi/dev hosts don't fail to start.
    integrations::KdeConnectConfig kdc_cfg;
    if (cfg.contains("kdeconnect")) {
        auto& jk = cfg["kdeconnect"];
        kdc_cfg.enabled        = jval(jk, "enabled",        kdc_cfg.enabled);
        kdc_cfg.device_id      = jk.value("device_id",      kdc_cfg.device_id);
        kdc_cfg.auto_dismiss_s = jval(jk, "auto_dismiss_s", kdc_cfg.auto_dismiss_s);
        kdc_cfg.app_blocklist  = jk.value("app_blocklist",  kdc_cfg.app_blocklist);
        kdc_cfg.message_apps   = jk.value("message_apps",   kdc_cfg.message_apps);
        kdc_cfg.ignore_list    = jk.value("ignore_list",    kdc_cfg.ignore_list);
        kdc_cfg.low_battery_pct= jk.value("low_battery_pct", kdc_cfg.low_battery_pct);
    }

    // Phone Inbox — watches a directory (default ~/Downloads) for files
    // shared from the phone (via KDE Connect Share or anything else) and
    // surfaces import-prompt toasts. Doesn't depend on DBus; works even
    // when the user shares files some other way (sftp, scp, etc.).
    integrations::PhoneInboxConfig pi_cfg;
    if (cfg.contains("phone_inbox")) {
        auto& jp = cfg["phone_inbox"];
        pi_cfg.enabled        = jval(jp, "enabled",        pi_cfg.enabled);
        pi_cfg.watch_dir      = jp.value("watch_dir",      pi_cfg.watch_dir);
        pi_cfg.settle_seconds = jval(jp, "settle_seconds", pi_cfg.settle_seconds);
        pi_cfg.auto_dismiss_s = jval(jp, "auto_dismiss_s", pi_cfg.auto_dismiss_s);
    }

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
        and_cfg.turn_screen_off = jval(jand, "turn_screen_off", false);
        and_cfg.new_display      = jval(jand, "new_display", false);
        and_cfg.new_display_size = jand.value("new_display_size", std::string(""));
        and_cfg.start_app        = jand.value("start_app", std::string(""));
        and_cfg.nav_uri_template = jand.value("nav_uri_template", and_cfg.nav_uri_template);
        if (jand.contains("destinations") && jand["destinations"].is_array()) {
            for (auto& d : jand["destinations"]) {
                AndroidMirrorConfig::NavDestination nd;
                nd.name  = d.value("name",  std::string(""));
                nd.query = d.value("query", std::string(""));
                if (!nd.name.empty() && !nd.query.empty())
                    and_cfg.destinations.push_back(std::move(nd));
            }
        }
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

    // ── Shared state (AppState declared earlier; here we populate it from cfg) ─

    state.max_messages        = jval(jhud, "lora_message_history", 50);
    state.compass_bg_enabled  = jhud.value("compass_bg", true);
    state.compass_tape        = jhud.value("compass_tape", true);
    state.legacy_hud          = jhud.value("legacy_hud", true);
    state.skip_landing        = jval(cfg.contains("landing") ? cfg["landing"] : json::object(),
                                     "skip", false);
    state.expanded_show_debug = jhud.value("expanded_show_debug", false);
    state.expanded_hide_info  = jhud.value("expanded_hide_info", false);
    state.scheduler_lead_min  = sched_lead_min;   // loaded above, applied now that state exists
    state.win_fullscreen.store(xr_cfg.fullscreen);  // mirror display cfg for the Settings toggles
    state.win_frameless.store(xr_cfg.frameless);

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

    // Video recording config (see "video" section of config.json).
    VideoConfig cfg_video;
    cfg_video.dir = home_dir + "/Videos/protohud";
    if (cfg.contains("video")) {
        auto& jv      = cfg["video"];
        { auto v = jv.value("dir", std::string{}); if (!v.empty()) cfg_video.dir = v; }
        cfg_video.fps    = jv.value("fps",    cfg_video.fps);
        cfg_video.fourcc = jv.value("fourcc", cfg_video.fourcc);
        std::string camsel = jv.value("camera", std::string("left"));
        state.video_camera = (camsel == "right") ? VideoCamera::Right
                           : (camsel == "both")  ? VideoCamera::Both
                                                 : VideoCamera::Left;
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
        mo.compass_ring        = jm.value("compass_ring",        mo.compass_ring);
        mo.battery_arc         = jm.value("battery_arc",         mo.battery_arc);
        mo.system_debug        = jm.value("system_debug",        mo.system_debug);
        mo.clock               = jm.value("clock",               mo.clock);
        mo.clock_date          = jm.value("clock_date",          mo.clock_date);
        mo.portrait            = jm.value("portrait",            mo.portrait);
        mo.portrait_right_half = jm.value("portrait_right_half", mo.portrait_right_half);
        mo.portrait_scale      = jm.value("portrait_scale",      mo.portrait_scale);
        mo.zoom                = jm.value("zoom",                mo.zoom);
        { auto v = jm.value("map_path", std::string{}); if (!v.empty()) mo.map_path = v; }
    }

    // Cycling info panel (opposite the minimap).
    if (cfg.contains("info_panel")) {
        const auto& jip = cfg["info_panel"];
        auto& ip = state.info_panel;
        ip.enabled   = jip.value("enabled",   ip.enabled);
        ip.anchor_x  = jip.value("anchor_x",  ip.anchor_x);
        ip.anchor_y  = jip.value("anchor_y",  ip.anchor_y);
        ip.size_px    = jip.value("size_px",    ip.size_px);
        ip.cycle_sec  = jip.value("cycle_sec",  ip.cycle_sec);
        ip.clock_face = jip.value("clock_face", ip.clock_face);
        if (jip.contains("show") && jip["show"].is_array()) {
            const auto& sh = jip["show"];
            for (int i = 0; i < static_cast<int>(InfoWidget::Count) &&
                            i < static_cast<int>(sh.size()); ++i)
                ip.show[i] = sh[i].get<bool>();
        }
    }

    // HUD dock (top/bottom + swap) — positions the minimap & info panel twins.
    if (cfg.contains("hud_dock")) {
        const auto& jd = cfg["hud_dock"];
        state.hud_dock.bottom   = jd.value("bottom",   state.hud_dock.bottom);
        state.hud_dock.v_offset = jd.value("v_offset", state.hud_dock.v_offset);
    }
    apply_hud_dock(state);   // dock is authoritative for both anchors

    // Weather (Open-Meteo via WeatherMonitor).
    if (cfg.contains("weather")) {
        const auto& jw = cfg["weather"];
        auto& wc = state.weather_cfg;
        wc.enabled      = jw.value("enabled",      wc.enabled);
        wc.auto_locate  = jw.value("auto_locate",  wc.auto_locate);
        wc.metric       = jw.value("metric",       wc.metric);
        wc.lat          = jw.value("lat",          wc.lat);
        wc.lon          = jw.value("lon",          wc.lon);
        wc.place        = jw.value("place",        wc.place);
        wc.interval_min = jw.value("interval_min", wc.interval_min);
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
        splash_cfg.logo_size_px  = js.value("logo_size_px",  splash_cfg.logo_size_px);
        splash_cfg.show_ring     = js.value("show_ring",     splash_cfg.show_ring);
        splash_cfg.min_display_s = js.value("min_display_s", splash_cfg.min_display_s);
        splash_cfg.title         = js.value("title",         splash_cfg.title);
        splash_cfg.subtitle      = js.value("subtitle",      splash_cfg.subtitle);
    }
    // ── Splash + landing temporarily disabled (re-enable by removing the
    // two assignments below). Keeps all the SplashScreen / LandingState
    // code in place; just forces both paths to short-circuit.
    splash_cfg.enabled = false;
    // Resolve relative image path against the binary directory.
    if (!splash_cfg.image_path.empty() && splash_cfg.image_path[0] != '/')
        splash_cfg.image_path = res(splash_cfg.image_path);

    // Seed per-eye resolution state from whatever each OWLsight config says
    state.camera_resolution.width        = owl_left.width;
    state.camera_resolution.height       = owl_left.height;
    state.camera_resolution.fps          = owl_left.fps;
    state.camera_resolution_right.width  = owl_right.width;
    state.camera_resolution_right.height = owl_right.height;
    state.camera_resolution_right.fps    = owl_right.fps;

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
    // When a BNO086 is the head-tracking source it owns imu_pose; the VITURE
    // callback then only publishes its heading (below), not the pose, so the
    // two IMUs don't fight over the head orientation.
    std::atomic<bool> bno08x_owns_pose { false };

    xr.on_imu_pose([&state, &last_xr_imu_us, &bno08x_owns_pose](float roll, float pitch, float yaw) {
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_xr_imu_us = now_us;
        std::lock_guard<std::mutex> lk(state.mtx);
        if (!bno08x_owns_pose.load()) state.imu_pose = { roll, pitch, yaw };

        // Publish to the IMU bus so the heading picker can consider Viture
        // alongside the dedicated IMU sensors. Invert applied here so the
        // picker can stay backend-agnostic.
        float h = state.compass_invert ? (360.f - yaw) : yaw;
        h = std::fmod(h, 360.f);
        if (h < 0.f) h += 360.f;
        state.imu_viture.heading_deg = h;
        state.imu_viture.last_us     = now_us;
        state.imu_viture.calibrated  = true;

        // Mirror into the debug-window IMU readout + estimate the callback rate.
        auto& d = state.imu_data;
        d.xr_roll = roll; d.xr_pitch = pitch; d.xr_yaw = yaw;
        static int64_t prev_us = 0;
        if (prev_us) {
            float dms = (now_us - prev_us) / 1000.f;
            if (dms > 0.f) {
                float hz = 1000.f / dms;
                d.xr_rate_hz = (d.xr_rate_hz > 0.f) ? d.xr_rate_hz * 0.98f + hz * 0.02f : hz;
            }
        }
        prev_us = now_us;
    });

    xr.on_state_changed([](int id, int val) {
        std::cout << "[xr] state change id=" << id << " val=" << val << "\n";
    });

    // ── MPU-9250 compass ──────────────────────────────────────────────────────
    // Publishes into the IMU bus (state.imu_mpu); pick_imu_heading() chooses
    // which slot drives the HUD compass per frame based on state.imu_source.

    Mpu9250 mpu9250(mpu_cfg);
    mpu9250.set_heading_callback([&state](float heading) {
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(state.mtx);
        state.health.mpu9250_ok = true;

        // Circular low-pass filter on the slot's stored value — operate on
        // sin/cos to handle 0/360 wrap. alpha=0.1 at 50 Hz → ~0.19 s time
        // constant. Slot keeps a filtered value so the picker can read it
        // directly without re-smoothing on every frame.
        constexpr float kAlpha   = 0.1f;
        constexpr float kDeg2Rad = 3.14159265f / 180.f;
        const float prev = state.imu_mpu.heading_deg;
        const float fs = std::sin(prev * kDeg2Rad) + kAlpha * (std::sin(heading * kDeg2Rad) - std::sin(prev * kDeg2Rad));
        const float fc = std::cos(prev * kDeg2Rad) + kAlpha * (std::cos(heading * kDeg2Rad) - std::cos(prev * kDeg2Rad));
        float filtered = std::atan2(fs, fc) / kDeg2Rad;
        if (filtered < 0.f) filtered += 360.f;
        state.imu_mpu.heading_deg = filtered;
        state.imu_mpu.last_us     = now_us;
        state.imu_mpu.calibrated  = true;
    });

    // Full raw 9-axis sample → debug-window IMU readout (separate from the
    // heading filter above so compass behaviour is unchanged).
    mpu9250.set_sample_callback([&state](const Mpu9250::Sample& s) {
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(state.mtx);
        auto& d = state.imu_data;
        d.mpu_ok = true;
        for (int i = 0; i < 3; ++i) {
            d.accel_g[i]  = s.accel_g[i];
            d.gyro_dps[i] = s.gyro_dps[i];
            d.mag_ut[i]   = s.mag_ut[i];
        }
        d.temp_c      = s.temp_c;
        d.mpu_heading = s.heading_deg;
        static int64_t prev_us = 0;
        if (prev_us) {
            float dms = (now_us - prev_us) / 1000.f;
            if (dms > 0.f) {
                float hz = 1000.f / dms;
                d.mpu_rate_hz = (d.mpu_rate_hz > 0.f) ? d.mpu_rate_hz * 0.9f + hz * 0.1f : hz;
            }
        }
        prev_us = now_us;
    });

    if (!mpu9250.start() && mpu_cfg.enabled)
        std::cerr << "[main] MPU-9250 backup compass unavailable\n";

    // ── BNO055 (Adafruit 9-DOF absolute orientation) ─────────────────────────
    // On-chip sensor fusion in NDOF mode — reports a calibrated absolute
    // heading directly. Publishes into state.imu_bno alongside MPU/Viture;
    // the IMU source picker chooses whichever the user selected (or the
    // highest-priority fresh slot in Auto mode).
    Bno055 bno055(bno_cfg);
    bno055.set_heading_callback([&state](float heading) {
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(state.mtx);
        state.imu_bno.heading_deg = heading;
        state.imu_bno.last_us     = now_us;
        // calib_sys ≥ 2 is "trustworthy" per the datasheet; below that the
        // chip is still building its mag model and the heading will drift.
    });
    bno055.set_sample_callback([&state, &bno055](const Bno055::Sample& s) {
        std::lock_guard<std::mutex> lk(state.mtx);
        // Surface the calibration flag so pick_imu_heading can downgrade
        // BNO055 to a non-preferred source until the user has rotated the
        // head enough for the mag model to settle.
        state.imu_bno.calibrated = (bno055.calib_sys() >= 2);
        // Mirror raw 9-axis + calibration into the debug IMU readout. Lives
        // alongside the MPU9250 fields so the debug window can show both.
        auto& d = state.imu_data;
        d.bno_ok    = true;
        d.bno_calib_sys   = s.calib_sys;
        d.bno_calib_gyro  = s.calib_gyro;
        d.bno_calib_accel = s.calib_accel;
        d.bno_calib_mag   = s.calib_mag;
        for (int i = 0; i < 3; ++i) {
            d.bno_accel_g[i]  = s.accel_g[i];
            d.bno_gyro_dps[i] = s.gyro_dps[i];
            d.bno_mag_ut[i]   = s.mag_ut[i];
            d.bno_euler[i]    = s.euler_deg[i];
        }
    });
    bno055.set_calib_saved_callback([&state](bool ok) {
        Notification n;
        n.type           = NotifType::App;
        n.title          = ok ? "IMU calibration saved" : "IMU calibration save failed";
        n.body           = ok ? "BNO055 offsets stored; they'll load on boot."
                               : "Could not read/write the calibration profile.";
        n.auto_dismiss_s = 6.f;
        std::lock_guard<std::mutex> lk(state.mtx);
        state.notifs.push(std::move(n));
    });
    if (!bno055.start() && bno_cfg.enabled)
        std::cerr << "[main] BNO055 9-DOF IMU unavailable\n";

    // ── BNO086 (SH-2, I2C) ───────────────────────────────────────────────────
    // Mag-referenced heading → imu_bno08x (compass), and when head_tracking is
    // enabled, roll/pitch/yaw → imu_pose (the head-tracking path), taking over
    // pose ownership from the VITURE IMU.
    Bno08x bno086(bno08x_cfg);
    bno086.set_heading_callback([&state](float heading) {
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(state.mtx);
        state.imu_bno08x.heading_deg = heading;
        state.imu_bno08x.last_us     = now_us;
        state.imu_bno08x.calibrated  = true;
    });
    bno086.set_orientation_callback([&state](float roll, float pitch, float yaw) {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.imu_pose = { roll, pitch, yaw };   // owns pose while head_tracking on
    });
    bno086.set_sample_callback([&state](const Bno08x::Sample& s) {
        std::lock_guard<std::mutex> lk(state.mtx);
        auto& d = state.imu_data;
        d.xr_roll = s.euler_deg[0]; d.xr_pitch = s.euler_deg[1]; d.xr_yaw = s.euler_deg[2];
    });
    if (bno086.start()) {
        bno08x_owns_pose.store(bno08x_cfg.head_tracking);
    } else if (bno08x_cfg.enabled) {
        std::cerr << "[main] BNO086 IMU unavailable\n";
    }

    // ── Boop sensor ──────────────────────────────────────────────────────────
    // Polls on its own thread; the on_boop callback fires from there and
    // re-enters face_proxy.trigger_boop, which locks the controller's mutex
    // before touching panels. Safe to construct unconditionally — start()
    // returns false (no thread spun up) when cfg.enabled is false or the
    // chip isn't present on the bus.
    sensor::Mpr121BoopSensor boop_sensor(boop_cfg);
    // Canonical boop reaction PNG name per zone. When face_image_exists()
    // confirms a dedicated boop_<zone>.png is authored, we trigger that
    // expression by filename instead of the user's configured fallback —
    // gives users distinct "snout boop" / "left wink" / "right wink" /
    // "surprise" reactions on top of the generic expression cycle.
    auto boop_face_stem = [](sensor::BoopSensor::Zone z) -> const char* {
        switch (z) {
        case sensor::BoopSensor::Zone::Snout:      return "boop_snout";
        case sensor::BoopSensor::Zone::LeftCheek:  return "boop_left";
        case sensor::BoopSensor::Zone::RightCheek: return "boop_right";
        case sensor::BoopSensor::Zone::BothCheeks: return "boop_both";
        }
        return "";
    };

    // Per-zone rapid-boop tracking for the animated-eyes easter egg. Touched
    // only from the sensor poll thread, so no extra locking is needed here.
    auto eye_now = []{
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    auto eye_last = std::make_shared<std::array<double, 4>>();   // last boop time / zone
    auto eye_run  = std::make_shared<std::array<int, 4>>();      // consecutive run / zone
    // fire_boop is now callable from BOTH the MPR121 poll thread and the GPIO
    // input thread, so the rapid-boop counters need a lock.
    auto eye_mtx  = std::make_shared<std::mutex>();

    // Accessory LED flash feedback per boop zone — shared by the normal boop
    // reaction and the animated-eyes path.
    auto flash_zone = [&accessory_leds](sensor::BoopSensor::Zone z) {
        using LZ = accessory::Zone;
        switch (z) {
        case sensor::BoopSensor::Zone::Snout:
            accessory_leds.trigger_flash(LZ::LeftFin,       0.35);
            accessory_leds.trigger_flash(LZ::RightFin,      0.35);
            break;
        case sensor::BoopSensor::Zone::LeftCheek:
            accessory_leds.trigger_flash(LZ::LeftCheekhub,  0.35);
            break;
        case sensor::BoopSensor::Zone::RightCheek:
            accessory_leds.trigger_flash(LZ::RightCheekhub, 0.35);
            break;
        case sensor::BoopSensor::Zone::BothCheeks:
            accessory_leds.trigger_flash(LZ::LeftCheekhub,  0.45);
            accessory_leds.trigger_flash(LZ::RightCheekhub, 0.45);
            accessory_leds.trigger_flash(LZ::LeftFin,       0.45);
            accessory_leds.trigger_flash(LZ::RightFin,      0.45);
            break;
        }
    };

    auto fire_boop = [&face_proxy, &state, boop_face_stem,
                      eye_now, flash_zone, eye_last, eye_run, eye_mtx]
                     (sensor::BoopSensor::Zone z) {
        const auto zi = static_cast<size_t>(z);
        if (zi >= 4) return;
        // Snapshot under the state lock so a menu edit mid-boop can't tear
        // the std::string read.
        bool             enabled;
        std::string      fallback_expr;
        double           duration_s;
        EyeTriggerConfig eye;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            enabled       = state.boop_zones[zi].enabled;
            fallback_expr = state.boop_zones[zi].expression;
            duration_s    = state.boop_zones[zi].duration_s;
            eye           = state.boop_zones[zi].eye_trigger;
        }
        if (!enabled) return;

        // Rapid-boop animated-eyes easter egg: count boops on this zone landing
        // within window_s of each other; on the Nth, play the procedural eye
        // animation *instead* of the normal reaction and reset the counter.
        if (eye.enabled && eye.count > 0) {
            const double now = eye_now();
            bool fire = false;
            {
                std::lock_guard<std::mutex> lk(*eye_mtx);
                if (now - (*eye_last)[zi] <= eye.window_s) (*eye_run)[zi] += 1;
                else                                       (*eye_run)[zi]  = 1;
                (*eye_last)[zi] = now;
                if ((*eye_run)[zi] >= eye.count) { (*eye_run)[zi] = 0; fire = true; }
            }
            if (fire) {
                face_proxy.play_eye_animation(eye.anim, eye.speed, eye.size,
                                              eye.r, eye.g, eye.b, eye.duration_s);
                flash_zone(z);
                return;   // eyes play instead of the normal boop reaction
            }
        } else {
            std::lock_guard<std::mutex> lk(*eye_mtx);
            (*eye_run)[zi] = 0;
        }
        // Prefer the dedicated boop_<zone> face when present on disk.
        // face_image_exists() is the canonical "is this PNG in the active
        // face folder" check the editor/import path uses too.
        std::string expression;
        const std::string stem = boop_face_stem(z);
        if (!stem.empty() && face_proxy.face_image_exists(stem))
            expression = stem;
        else
            expression = fallback_expr;
        if (expression.empty()) return;
        face_proxy.trigger_boop(expression, duration_s);
        // Touch feedback on the panels: an expanding ring at the booped zone
        // (native renderer only; other backends no-op).
        face_proxy.trigger_boop_ripple(static_cast<int>(z));

        // Accessory LED flash feedback (same per-zone mapping for both paths).
        flash_zone(z);
    };
    boop_sensor.on_boop(fire_boop);
    if (boop_cfg.enabled && !boop_sensor.start())
        std::cerr << "[main] boop sensor (MPR121) unavailable\n";
    boop_sensor_ptr = &boop_sensor;   // expose for the menu's live tuning

    if (led_cfg.enabled && !accessory_leds.start())
        std::cerr << "[main] accessory LEDs unavailable — continuing without\n";

    // ── Light sensor (BH1750 ambient lux → squint reaction) ──────────────────
    // Hardware-level config (bus, address, sensor type) comes from cfg["light_sensor"];
    // the trigger thresholds + reaction live in state.light_squint so the menu can
    // mutate them at runtime and mutate_cfg persists them on save.
    sensor::LightSensor::Config light_cfg;
    if (cfg.contains("light_sensor")) {
        auto& jl = cfg["light_sensor"];
        light_cfg.enabled  = jval(jl, "enabled",  light_cfg.enabled);
        light_cfg.i2c_bus  = jl.value("i2c_bus",  light_cfg.i2c_bus);
        light_cfg.i2c_addr = jval(jl, "i2c_addr", light_cfg.i2c_addr);
        light_cfg.poll_hz  = jval(jl, "poll_hz",  light_cfg.poll_hz);
        state.light_squint.enabled              = jval(jl, "enabled",              state.light_squint.enabled);
        state.light_squint.dark_threshold_lux   = jval(jl, "dark_threshold_lux",   state.light_squint.dark_threshold_lux);
        state.light_squint.bright_threshold_lux = jval(jl, "bright_threshold_lux", state.light_squint.bright_threshold_lux);
        state.light_squint.transition_window_s  = jval(jl, "transition_window_s",  state.light_squint.transition_window_s);
        state.light_squint.expression           = jl.value("expression",            state.light_squint.expression);
        state.light_squint.duration_s           = jval(jl, "duration_s",           state.light_squint.duration_s);
        state.light_squint.cooldown_s           = jval(jl, "cooldown_s",           state.light_squint.cooldown_s);
    }
    sensor::LightSensor light_sensor(light_cfg);
    // Edge detector: remember when we last saw "dark"; if we cross into
    // "bright" within transition_window_s, fire the squint reaction. Cooldown
    // gates back-to-back squints (e.g. flickering lights).
    struct LightEdgeState {
        double last_dark_t   = -1.0;
        double last_squint_t = -1.0e9;
    };
    auto light_edge = std::make_shared<LightEdgeState>();
    light_sensor.set_lux_callback([light_edge, &state, &face_proxy](float lux) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Snapshot the config so a mid-callback menu edit can't tear strings.
        LightSquintConfig c;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            c = state.light_squint;
        }
        if (!c.enabled) return;
        if (lux < c.dark_threshold_lux) {
            light_edge->last_dark_t = now;
            return;
        }
        if (lux <= c.bright_threshold_lux) return;
        // Bright. Did we come from dark recently?
        if (light_edge->last_dark_t < 0.0) return;
        if (now - light_edge->last_dark_t > c.transition_window_s) return;
        // Cooldown gate.
        if (now - light_edge->last_squint_t < c.cooldown_s) return;
        light_edge->last_squint_t = now;
        light_edge->last_dark_t   = -1.0;   // consume the edge
        if (!c.expression.empty())
            face_proxy.trigger_boop(c.expression, c.duration_s);
    });
    if (light_cfg.enabled && !light_sensor.start())
        std::cerr << "[main] light sensor unavailable\n";

    // ── Dev/debug monitors ────────────────────────────────────────────────────

    SystemMonitor    sys_mon;
    WifiMonitor      wifi_mon;
    PingMonitor      ping_mon;
    BtMonitor        bt_mon;
    SchedulerMonitor sched_mon;
    WeatherMonitor   weather_mon;

    sys_mon.start(&state);
    wifi_mon.start(&state, cfg_wifi_iface);
    ping_mon.start(&state, cfg_ping_host);
    bt_mon.start(&state);
    weather_mon.start(&state);
    if (sched_enabled) {
        if (sched_autostart) {
            // Launch the daemon via fork()+setsid()+execlp(), the same robust path
            // panel_driver.py uses — a backgrounded std::system("... &") was failing
            // silently / not surviving on this platform. config is found via the
            // script's own __file__, so no chdir is needed.
            // NOTE: the daemon entry is scheduler.py (not run.py) on purpose — the
            // Protoface setup below runs `pkill -f run.py`, which would otherwise
            // kill this daemon too.
            const std::string drv = bin_dir + "/../scheduler_daemon/scheduler.py";
            std::system("pkill -f scheduler_daemon 2>/dev/null");   // clear any old instance
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int lf = ::open("/tmp/protohud-scheduler.log",
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); ::close(lf); }
                int nf = ::open("/dev/null", O_RDONLY);
                if (nf >= 0) { dup2(nf, 0); ::close(nf); }
                execlp("python3", "python3", "-u", drv.c_str(),
                       static_cast<char*>(nullptr));
                const char* msg = "[scheduler] execlp python3 failed — not on PATH?\n";
                ssize_t wr = ::write(2, msg, strlen(msg)); (void)wr;
                _exit(127);   // execlp failed (python3 not found)
            }
            std::cout << "[scheduler] launched daemon pid=" << pid
                      << " (" << drv << ", log: /tmp/protohud-scheduler.log)\n";
        } else {
            std::cout << "[scheduler] autostart disabled (scheduler.autostart=false)\n";
        }
        sched_mon.start(&state, sched_events_path, sched_status_path, sched_poll_s);
    } else {
        std::cout << "[scheduler] disabled (scheduler.enabled=false)\n";
    }

    // KDE Connect bridge — RX phone notifications, pushed into AppState::notifs.
    // Optional (gated by HAVE_DBUS at build time + cfg.enabled at runtime); silent
    // no-op when the daemon isn't running so it doesn't spam logs on dev hosts.
    // Visible to the menu regardless of HAVE_DBUS (the header is dbus-free);
    // stays null when the bridge isn't compiled in, so "Ring My Phone" no-ops.
    integrations::KdeConnectBridge* kdc_menu_ptr = nullptr;
#ifdef HAVE_DBUS
    const bool kdc_enabled = kdc_cfg.enabled;
    integrations::KdeConnectBridge kdc(state, std::move(kdc_cfg));
    kdc_menu_ptr = &kdc;
    if (kdc_enabled) {
        if (!kdc.start())
            std::cout << "[kdeconnect] disabled (failed to attach to session bus)\n";
        else
            std::cout << "[kdeconnect] bridge started\n";
    } else {
        std::cout << "[kdeconnect] disabled (cfg.kdeconnect.enabled=false)\n";
    }
#endif

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

    // Voice analyzer tuning is set here; the face-drive callback is wired
    // later, once face_proxy exists. The audio thread starts without a face
    // callback set — its push_stereo_s16 path is no-op while enabled is false
    // anyway, so there's no window of dangling reads.
    if (auto* va = audio.voice()) {
        va->set_sensitivity(state.voice_mouth.sensitivity);
        va->set_noise_gate (state.voice_mouth.noise_gate);
        va->set_attack_ms  (state.voice_mouth.attack_ms);
        va->set_release_ms (state.voice_mouth.release_ms);
        va->set_band       (state.voice_mouth.band_lo_hz, state.voice_mouth.band_hi_hz);
        va->set_viseme_thresholds(state.voice_mouth.viseme_round_max_hz,
                                  state.voice_mouth.viseme_open_max_hz,
                                  state.voice_mouth.viseme_small_max_hz);
        va->set_visemes_enabled(state.voice_mouth.visemes_enabled);
        va->set_enabled    (state.voice_mouth.enabled);
    }

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
    hud.set_icon_dir(res("assets/icons"));

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

    // CSI boot auto-retry: a sensor that comes up wedged at boot leaves one eye
    // dark until a reboot. Attempt reinit_owls() a few times early on (from the
    // render loop, on the render thread) to recover it. csi_expected lets a mono
    // build set 1 so it never retries a "missing" second camera it doesn't have.
    int       csi_retries_left = jval(jcam, "csi_boot_retries", 2);
    const int csi_expected     = std::clamp(jval(jcam, "csi_expected", 2), 0, 2);
    auto      csi_next_retry   = std::chrono::steady_clock::now() + std::chrono::seconds(4);

    // Startup autofocus
    bool af_on_startup = false;
    if (cfg.contains("camera"))
        af_on_startup = cfg["camera"].value("autofocus_on_startup", false);
    if (af_on_startup) {
        if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
        if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
        std::cout << "[main] autofocus on startup\n";
    }

    // Re-apply persisted per-eye AF/AE/WB/ISP/HDR controls now the cameras are
    // running (the setters queue onto the next request). Done after the startup
    // autofocus so a saved manual focus mode isn't immediately overridden.
    apply_cam_controls(cameras.owl_left(),  state.camera_controls_left);
    apply_cam_controls(cameras.owl_right(), state.camera_controls_right);

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
    // SmartKnob: the firmware uses the ESP32-S3 NATIVE USB-CDC port (build flag
    // ARDUINO_USB_MODE=1), so it enumerates as Espressif VID=0x303A, PID=0x1001
    // on a /dev/ttyACM node — NOT the CH341 UART bridge (0x1A86) the board also
    // exposes. If the PID varies by core version, set smartknob.port explicitly
    // to a /dev/serial/by-id/... path instead.
    teensy_port = resolve_serial_port(teensy_port, 0x16C0, 0x0483);
    lora_port   = resolve_serial_port(lora_port,   0x1A86, 0x7523);
    knob_port   = resolve_serial_port(knob_port,   0x303A, 0x1001);

    TeensyController     teensy  (teensy_port,   baud, state);
    ProtoFaceController  protoface_ctrl;
    LoRaRadio            lora    (lora_port,     baud, state);
    SmartKnob            knob    (knob_port,     baud, state);
    WirelessController   wireless(wireless_port, baud);

    // Native in-process face renderer (only constructed in "native" mode). It
    // renders the LED face in C++ and writes the same /dev/shm frame the daemon
    // would, so the existing preview path and panel_driver.py work unchanged.
    std::unique_ptr<face::NativeFaceController> native_ctrl;

    if (!teensy.start()) std::cerr << "[main] Teensy not available on " << teensy_port << "\n";
    if (pf_mode == "native") {
        // Ensure NO Protoface daemon co-exists — it would double-write the shm
        // ("neutral teal" frames / flicker) and fight panel_driver.py for
        // /dev/pio0. First ask any responsive daemon to exit cleanly (blanks the
        // panels), then force-kill any wedged/stale one (its IPC may be dead, so
        // shutdown_daemon can't reach it), then settle before claiming the lock.
        protoface_ctrl.shutdown_daemon();
        std::system("pkill -f run.py 2>/dev/null");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // Claim Protoface's single-instance lock and hold it for our whole
        // lifetime, so a daemon that respawns later hits the flock in run.py and
        // exits immediately instead of double-writing the shm.
        {
            static int pf_lock_fd = ::open("/tmp/protoface.lock", O_RDWR | O_CREAT, 0660);
            if (pf_lock_fd < 0 || ::flock(pf_lock_fd, LOCK_EX | LOCK_NB) != 0)
                std::cerr << "[main] warning: could not claim /tmp/protoface.lock — "
                             "a daemon may still be running and double-writing the panel\n";
        }
        face::RenderConfig rc = pf_build_render_config(cfg, &pf_hub75);
        // Per-backend face folder — HUB75 keeps the legacy "main" folder
        // for back-compat; MAX7219 / RGB-matrix get their own "main_max7219"
        // / "main_rgb_matrix" subfolders so users can author distinct art
        // for the different panel technologies without files colliding.
        if (pf_backend != "hub75") {
            const std::string suffix = "_" + pf_backend;
            for (auto& pn : rc.panels)
                if (!pn.face.active.empty()) pn.face.active += suffix;
            // MAX7219 / RGB-matrix panels are discrete 8x8 LED grids: a
            // sub-pixel wiggle that reads as gentle motion on HUB75 just
            // smears pixels on these backends and looks wrong both on the
            // panels and in the preview. Zero the wiggle on every panel.
            // Particle effects (sparkle / rain / etc.) likewise don't read
            // on a handful of 8x8 modules — disable them entirely via the
            // RenderConfig flag (also stops set_effect from re-installing
            // them after start-up).
            for (auto& pn : rc.panels) {
                pn.face.wiggle.amplitude_x = 0.0;
                pn.face.wiggle.amplitude_y = 0.0;
                pn.particles = "none";
            }
            rc.effects_enabled = false;
            // The legacy HUB75 layout uses face_left (0,0,64,32) +
            // face_right (64,0,64,32) where face_right is mirror_of
            // face_left, so the canvas ends up holding the face twice
            // (one copy + a horizontal flip). On MAX7219 / RGB matrix
            // each chain reads a specific slice of the canvas (eye_l,
            // eye_r, nose, mouth — positioned around the nose centre),
            // so the right "mirror copy" panel just doubles what the
            // preview shows and never matches the actual chain layout.
            // Shrink the renderer canvas to the face content width
            // (2*mirror_x) and replace the panel pair with one panel
            // covering it. canvas_w == panel_w means the face PNG is
            // loaded at the same dimensions the editor saves it in —
            // otherwise cv::resize squishes the user's art when the
            // panel is narrower than the editor canvas.
            if (!rc.panels.empty()) {
                // Width is fully determined by the chain layouts now —
                // every zone mirrors around the same axis, so the canvas
                // is just 2 * mirror_x. Grows when the user picks bigger
                // panels; shrinks when they pick smaller ones.
                const int face_w = pf_canvas_w_for_layout(pf_eye_layout,
                                                          pf_mouth_layout,
                                                          pf_nose_layout);
                rc.canvas_w = face_w;
                face::PanelCfg solo = rc.panels.front();
                solo.name      = "face";
                solo.x         = 0;
                solo.y         = 0;
                solo.w         = face_w;
                solo.h         = rc.canvas_h;
                solo.mirror_of.clear();
                rc.panels.clear();
                rc.panels.push_back(std::move(solo));
            }
        }
        // Ensure the per-backend face folder(s) exist on disk — otherwise the
        // FaceLoader's directory scan returns empty and the in-HUD editor has
        // nowhere to write new PNGs. create_directories is a no-op if present.
        {
            std::error_code mkec;
            for (const auto& pn : rc.panels)
                if (!pn.face.active.empty())
                    fs::create_directories(fs::path(rc.faces_dir) / pn.face.active, mkec);
        }
        // Auto-save the live look next to config.json so menu changes persist.
        rc.state_path = (fs::path(cfg_path).parent_path() / "protoface_state.json").string();
        native_ctrl = std::make_unique<face::NativeFaceController>(
            rc, pf_build_panel_output(cfg, rc,
                                      pf_eye_layout, pf_mouth_layout, pf_nose_layout,
                                      &pf_hub75));
        native_ctrl->set_face_colors(state.face.face_colors);
        native_ctrl->set_menu_item(10, state.face.pride_sharp ? 1 : 0);  // pride sharp-bands
        native_ctrl->set_motion_particles(pf_motion_particles);
        native_ctrl->set_menu_item(11, (state.face.pride_angle / 15) & 0xFF);  // pride rotation
        native_ctrl->start();
        // Push the user's saved animation tunables into every panel's
        // FaceState. The defaults in FaceState/FaceCfg apply otherwise.
        native_ctrl->set_blink_enabled(pf_blink_enabled);
        native_ctrl->set_blink_timing(pf_blink_min, pf_blink_max, pf_blink_duration);
        native_ctrl->set_expression_fade(pf_expr_fade);
        native_ctrl->set_glitch(pf_glitch);
        native_ctrl->set_active_layout_name(pf_hub75_active);
        protoface_ctrl.start();   // shm reader only — feeds the in-HUD preview
        std::cout << "[main] Protoface: native in-process renderer\n";

        // Push frames to the panels via the Piomatter shim (reads the same shm).
        // Launch it with fork()+setsid()+execlp() rather than a backgrounded
        // shell command: the `nohup … &` form via std::system was failing
        // silently (no process, no log). Here we open the log in C++ so it's
        // always written, and setsid detaches the driver into its own session.
        // panel_driver.py is the HUB75-via-piomatter shim — only relevant
        // when the renderer writes into a /dev/shm frame. MAX7219 writes
        // directly to spidev and needs no Python helper.
        if (pf_launch_driver && pf_backend == "hub75") {
            int gpw = 64, gph = 32, gchain = 2, gpar = 1;
            pf_hub75_driver_geometry(pf_hub75, gpw, gph, gchain, gpar);
            pf_launch_panel_driver(bin_dir, rc.canvas_w, rc.canvas_h,
                                   gpw, gph, gchain, gpar, pf_hub75.pinout,
                                   pf_hub75.color_order, pf_hub75.camera_mode,
                                   pf_hub75.camera_planes,
                                   pf_hub75.camera_temporal_planes);
        }
    } else {
        // Auto-start the Protoface daemon on boot (no-op if already running). The
        // reconnect loop in start() then connects once its socket comes up.
        if (pf_autostart) protoface_ctrl.launch();
        protoface_ctrl.start();   // connects async; no-op if socket absent
    }

    // QR / barcode scanner — active when either scan toggle is enabled.
    // Captured-QR store: one folder per code (link.txt + the raw frame +
    // meta.json) under <config-dir>/qr_codes, with a de-duplicated running
    // list in index.json. Load the existing list so reboots keep history and
    // already-seen codes aren't re-captured.
    const std::string qr_dir =
        (fs::path(cfg_path).parent_path() / "qr_codes").string();
    {
        std::lock_guard lk(state.mtx);
        state.qr_dir = qr_dir;
        std::ifstream f(fs::path(qr_dir) / "index.json");
        if (f) {
            try {
                json arr; f >> arr;
                if (arr.is_array())
                    for (const auto& e : arr) {
                        if (!e.is_object()) continue;
                        QrCapture c;
                        c.text      = e.value("text", std::string());
                        c.type      = e.value("type", std::string());
                        c.timestamp = e.value("ts", static_cast<int64_t>(0));
                        c.folder    = e.value("folder", std::string());
                        c.image     = e.value("image", std::string());
                        c.decode    = e.value("decode", std::string());
                        if (!c.text.empty()) state.qr_captures.items.push_back(std::move(c));
                    }
            } catch (...) {}
        }
        state.qr_captures.rebuild_seen();
    }

    QrScanner qr_scanner;
    qr_scanner.set_callback([&state, qr_dir](const std::string& text,
                                             const std::string& type,
                                             const std::vector<uint8_t>& gray,
                                             const std::vector<uint8_t>& rgba,
                                             int w, int h) {
        // Honour mute window (set by "MUTE 1m" action)
        if (static_cast<int64_t>(time(nullptr)) < state.qr_mute_until_s.load()) return;

        // De-dupe against the running list: already-captured codes are dropped
        // silently so they don't double up.
        {
            std::lock_guard lk(state.mtx);
            if (state.qr_captures.contains(text)) return;
        }

        const bool is_url = text.size() > 7 &&
                            (text.substr(0, 7) == "http://" ||
                             text.substr(0, 8) == "https://");

        // Save this capture to its own folder: <ts>_<hash>/ with the link, the
        // raw grayscale frame it was decoded from, and a small meta.json.
        const int64_t now_s = static_cast<int64_t>(time(nullptr));
        QrCapture cap;
        cap.text = text; cap.type = type; cap.timestamp = now_s;
        if (!qr_dir.empty()) {
            time_t now = static_cast<time_t>(now_s);
            struct tm tm{}; localtime_r(&now, &tm);
            char ts[20]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
            char name[40];
            std::snprintf(name, sizeof(name), "%s_%08x", ts,
                static_cast<unsigned>(std::hash<std::string>{}(text) & 0xffffffffu));
            const fs::path folder = fs::path(qr_dir) / name;
            std::error_code ec;
            fs::create_directories(folder, ec);
            cap.folder = folder.string();
            { std::ofstream lf(folder / "link.txt"); if (lf) lf << text << "\n"; }
            // Grayscale decode frame (what ZBar saw).
            if (!gray.empty() && w > 0 && h > 0 &&
                static_cast<int>(gray.size()) >= w * h) {
                cv::Mat g(h, w, CV_8UC1, const_cast<uint8_t*>(gray.data()));
                if (cv::imwrite((folder / "decode.png").string(), g))
                    cap.decode = "decode.png";
            }
            // Colour camera frame (RGBA → BGR for PNG storage).
            if (!rgba.empty() && w > 0 && h > 0 &&
                static_cast<int>(rgba.size()) >= w * h * 4) {
                cv::Mat r(h, w, CV_8UC4, const_cast<uint8_t*>(rgba.data()));
                cv::Mat bgr; cv::cvtColor(r, bgr, cv::COLOR_RGBA2BGR);
                if (cv::imwrite((folder / "camera.png").string(), bgr))
                    cap.image = "camera.png";
            }
            json meta = {{"text", text}, {"type", type}, {"ts", now_s},
                         {"camera", cap.image}, {"decode", cap.decode}};
            std::ofstream mf(folder / "meta.json");
            if (mf) mf << meta.dump(2);
        }

        // Add to the running list + persist the index, then surface a toast.
        {
            std::lock_guard lk(state.mtx);
            state.qr_captures.add(cap);
            qr_write_index(state.qr_dir, state.qr_captures);

            Notification n;
            n.type           = NotifType::App;
            n.title          = type + " Saved";
            n.body           = text;
            n.auto_dismiss_s = 20.f;   // longer so user has time to act
            if (is_url) {
                n.actions.push_back({"OPEN", [text](AppState&) {
                    std::string safe = text;
                    for (auto& c : safe) if (c == '\'') c = ' ';
                    std::string cmd = "xdg-open '" + safe + "' >/dev/null 2>&1 &";
                    system(cmd.c_str());
                }});
            }
            n.actions.push_back({"MUTE 1m", [](AppState& s) {
                s.qr_mute_until_s.store(static_cast<int64_t>(time(nullptr)) + 60);
            }});
            state.notifs.push(std::move(n));
        }
    });
    cameras.set_qr_scanner(&qr_scanner);
    if (!lora.start())   std::cerr << "[main] LoRa not available on "   << lora_port   << "\n";
    if (!knob.start())   std::cerr << "[main] SmartKnob not available on " << knob_port << "\n";

    // Active face backend: native in-process renderer if enabled; otherwise
    // prefer the Protoface daemon if its socket exists or we auto-launched it
    // (commands no-op until the reconnect loop connects), else the Teensy.
    // active_face + face_proxy are forward-declared near the top of main()
    // so callbacks constructed earlier can capture them. Here we just point
    // the proxy at whichever real backend is appropriate for the current
    // pf_mode now that native_ctrl / protoface_ctrl / teensy all exist.
    active_face =
        native_ctrl ? static_cast<IFaceController*>(native_ctrl.get())
        : (ProtoFaceController::socket_exists() || pf_autostart)
            ? static_cast<IFaceController*>(&protoface_ctrl)
            : static_cast<IFaceController*>(&teensy);

    // Hot-swap: when the menu toggles Protoface > Hardware > Backend, we
    // stop the live NativeFaceController, retire it into a graveyard so any
    // mid-call audio/sensor thread doesn't reference a destructed object,
    // build the new PanelOutput per the updated config, swap the active
    // face pointer, and start the new controller. panel_driver.py is
    // started/stopped as part of the same handoff: HUB75 needs it, MAX7219
    // doesn't.
    std::vector<std::unique_ptr<face::NativeFaceController>> ctrl_graveyard;
    auto swap_backend = [&](const std::string& new_backend) {
        if (new_backend != "hub75" && new_backend != "max7219" &&
            new_backend != "rgb_matrix") return;
        if (new_backend == pf_backend) return;
        std::cout << "[main] backend hot-swap: " << pf_backend
                  << " -> " << new_backend << "\n";

        cfg["protoface"]["backend"] = new_backend;
        pf_backend = new_backend;

        if (!native_ctrl) {
            // Daemon mode — no native controller to swap; the menu update
            // still updates cfg so the choice persists.
            return;
        }

        // Stop the running controller, then retain it. Setting active_face
        // to the new controller is enough to redirect future calls; any
        // in-flight call on the old pointer keeps working because we don't
        // destruct it here.
        native_ctrl->stop();
        ctrl_graveyard.push_back(std::move(native_ctrl));

        face::RenderConfig rc = pf_build_render_config(cfg, &pf_hub75);
        // Per-backend face folder (see startup-path comment above).
        if (pf_backend != "hub75") {
            const std::string suffix = "_" + pf_backend;
            for (auto& pn : rc.panels)
                if (!pn.face.active.empty()) pn.face.active += suffix;
            // MAX7219 / RGB-matrix panels are discrete 8x8 LED grids: a
            // sub-pixel wiggle that reads as gentle motion on HUB75 just
            // smears pixels on these backends and looks wrong both on the
            // panels and in the preview. Zero the wiggle on every panel.
            // Particle effects (sparkle / rain / etc.) likewise don't read
            // on a handful of 8x8 modules — disable them entirely via the
            // RenderConfig flag (also stops set_effect from re-installing
            // them after start-up).
            for (auto& pn : rc.panels) {
                pn.face.wiggle.amplitude_x = 0.0;
                pn.face.wiggle.amplitude_y = 0.0;
                pn.particles = "none";
            }
            rc.effects_enabled = false;
            // The legacy HUB75 layout uses face_left (0,0,64,32) +
            // face_right (64,0,64,32) where face_right is mirror_of
            // face_left, so the canvas ends up holding the face twice
            // (one copy + a horizontal flip). On MAX7219 / RGB matrix
            // each chain reads a specific slice of the canvas (eye_l,
            // eye_r, nose, mouth — positioned around the nose centre),
            // so the right "mirror copy" panel just doubles what the
            // preview shows and never matches the actual chain layout.
            // Shrink the renderer canvas to the face content width
            // (2*mirror_x) and replace the panel pair with one panel
            // covering it. canvas_w == panel_w means the face PNG is
            // loaded at the same dimensions the editor saves it in —
            // otherwise cv::resize squishes the user's art when the
            // panel is narrower than the editor canvas.
            if (!rc.panels.empty()) {
                // Width is fully determined by the chain layouts now —
                // every zone mirrors around the same axis, so the canvas
                // is just 2 * mirror_x. Grows when the user picks bigger
                // panels; shrinks when they pick smaller ones.
                const int face_w = pf_canvas_w_for_layout(pf_eye_layout,
                                                          pf_mouth_layout,
                                                          pf_nose_layout);
                rc.canvas_w = face_w;
                face::PanelCfg solo = rc.panels.front();
                solo.name      = "face";
                solo.x         = 0;
                solo.y         = 0;
                solo.w         = face_w;
                solo.h         = rc.canvas_h;
                solo.mirror_of.clear();
                rc.panels.clear();
                rc.panels.push_back(std::move(solo));
            }
        }
        // Ensure the per-backend face folder(s) exist (see startup-path note).
        {
            std::error_code mkec;
            for (const auto& pn : rc.panels)
                if (!pn.face.active.empty())
                    fs::create_directories(fs::path(rc.faces_dir) / pn.face.active, mkec);
        }
        rc.state_path = (fs::path(cfg_path).parent_path() /
                          "protoface_state.json").string();
        auto new_output = pf_build_panel_output(cfg, rc,
                                                pf_eye_layout,
                                                pf_mouth_layout,
                                                pf_nose_layout,
                                                &pf_hub75);
        native_ctrl = std::make_unique<face::NativeFaceController>(
            rc, std::move(new_output));
        active_face = native_ctrl.get();
        native_ctrl->set_face_colors(state.face.face_colors);
        native_ctrl->set_menu_item(10, state.face.pride_sharp ? 1 : 0);  // pride sharp-bands
        native_ctrl->set_motion_particles(pf_motion_particles);
        native_ctrl->set_menu_item(11, (state.face.pride_angle / 15) & 0xFF);  // pride rotation
        native_ctrl->start();
        native_ctrl->set_blink_enabled(pf_blink_enabled);
        native_ctrl->set_blink_timing(pf_blink_min, pf_blink_max, pf_blink_duration);
        native_ctrl->set_expression_fade(pf_expr_fade);
        native_ctrl->set_glitch(pf_glitch);
        native_ctrl->set_active_layout_name(pf_hub75_active);

        // panel_driver.py choreography. The Python shim is only needed for
        // HUB75 (it reads /dev/shm frames and pushes them via piomatter);
        // both MAX7219 and RGB matrix backends drive spidev directly. Kill
        // on either of those — safe even if it wasn't running.
        if (new_backend == "max7219" || new_backend == "rgb_matrix") {
            std::system("pkill -f panel_driver.py 2>/dev/null");
        } else if (new_backend == "hub75" && pf_launch_driver) {
            int gpw = 64, gph = 32, gchain = 2, gpar = 1;
            pf_hub75_driver_geometry(pf_hub75, gpw, gph, gchain, gpar);
            pf_launch_panel_driver(bin_dir, rc.canvas_w, rc.canvas_h,
                                   gpw, gph, gchain, gpar, pf_hub75.pinout,
                                   pf_hub75.color_order, pf_hub75.camera_mode,
                                   pf_hub75.camera_planes,
                                   pf_hub75.camera_temporal_planes);
        }
    };

    // Edit… launcher used by Files > Faces > <slot> > Edit and Mouth Shapes
    // > <slot> > Edit. Only meaningful when the active backend has covered
    // regions (MAX7219 or RGB matrix today) — for HUB75 / daemon mode the
    // menu's visible_fn hides the leaf so this never runs.
    auto edit_face = [&](const std::string& expression) {
        if (!native_ctrl || !menu_ptr) return;
        // HUB75 backend: editor uses the picked panel inventory directly.
        // Native backends (MAX7219 / RGB matrix): use the chain layout
        // pickers as the source of truth for per-zone bboxes. Picker
        // changes take effect on the next Edit... without a backend rebuild.
        int cw, ch;
        PfFaceZones zones;
        if (pf_backend == "hub75" && pf_hub75.panel_count > 0) {
            pf_hub75_canvas(pf_hub75, cw, ch);
            cw = std::max(native_ctrl->canvas_width(), cw);
            ch = std::max(native_ctrl->canvas_height(), ch);
            const auto plist = pf_hub75_panels(pf_hub75);
            for (const auto& p : plist)
                zones.regions.push_back({p.name, p.rect});
            zones.mirror_x = cw / 2;
        } else {
            cw = std::max(native_ctrl->canvas_width(),
                pf_canvas_w_for_layout(pf_eye_layout,
                                       pf_mouth_layout,
                                       pf_nose_layout));
            ch = native_ctrl->canvas_height();
            zones = pf_compute_face_zones(pf_eye_layout,
                                          pf_mouth_layout,
                                          pf_nose_layout,
                                          cw, ch);
        }
        std::vector<cv::Rect> covered;
        covered.reserve(zones.regions.size());
        for (const auto& nr : zones.regions) covered.push_back(nr.rect);
        if (covered.empty()) {
            covered.emplace_back(0, 0, cw, ch);
            zones.regions.push_back({"face", cv::Rect(0, 0, cw, ch)});
        }
        // Friendlier labels (chain.name → display string). Custom names
        // pass through unchanged so user-defined zones still surface.
        auto pretty_label = [](const std::string& name) -> std::string {
            if (name == "eye_l")   return "Left Eye";
            if (name == "eye_r")   return "Right Eye";
            if (name == "nose")    return "Nose";
            if (name == "mouth")   return "Mouth";
            if (name == "mouth_l") return "Left Mouth";
            if (name == "mouth_r") return "Right Mouth";
            if (name == "face")    return "Face";
            return name;
        };
        std::vector<std::string> labels;
        labels.reserve(zones.regions.size());
        for (const auto& nr : zones.regions) labels.push_back(pretty_label(nr.name));
        const std::string abs_path = face_proxy.face_image_path(expression);
        if (abs_path.empty()) return;

        // Preload any blink eye polygons from the face folder's config.json
        // (canvas coords) so the editor shows them and round-trips them on save.
        // Accepts the new {"points":[[x,y],...]} polygon form and the legacy
        // {x,y,w,h} rectangle (promoted to a 4-corner polygon for editing).
        std::vector<menu::FaceEditor::EyePoly> eye_polys;
        {
            const fs::path cfgp = fs::path(abs_path).parent_path() / "config.json";
            std::ifstream ef(cfgp);
            if (ef) {
                try {
                    json ej; ef >> ej;
                    auto rd = [&](const char* k){
                        if (!ej.contains(k) || !ej[k].is_object()) return;
                        const auto& d = ej[k];
                        menu::FaceEditor::EyePoly poly;
                        if (d.contains("points") && d["points"].is_array()) {
                            for (const auto& pt : d["points"])
                                if (pt.is_array() && pt.size() == 2)
                                    poly.emplace_back(pt[0].get<int>(), pt[1].get<int>());
                        } else {                              // legacy rectangle
                            const int x = d.value("x", 0), y = d.value("y", 0);
                            const int w = std::max(1, d.value("w", 1));
                            const int h = std::max(1, d.value("h", 1));
                            poly = { {x, y}, {x + w - 1, y},
                                     {x + w - 1, y + h - 1}, {x, y + h - 1} };
                        }
                        if (poly.size() >= 3) eye_polys.push_back(std::move(poly));
                    };
                    rd("eye_left"); rd("eye_right");
                } catch (...) {}
            }
        }

        // Color canvas for the always-RGB WS2812 matrix, or whenever face-colour
        // pass-through is on (so HUB75 faces can be drawn in colour too).
        const menu::FaceEditor::Mode mode =
            (pf_backend == "rgb_matrix" || state.face.face_colors)
                ? menu::FaceEditor::Mode::Color
                : menu::FaceEditor::Mode::Mono;

        char title[96];
        std::snprintf(title, sizeof(title),
                      "Edit face: %s  (%s)",
                      expression.c_str(), pf_backend.c_str());
        menu_ptr->open_face_editor(
            title, abs_path, cw, ch, std::move(covered), std::move(labels),
            zones.mirror_x,
            mode, {} /* default palette */,
            std::move(eye_polys),
            /* on_commit */ [&face_proxy, &native_ctrl, expression]
                (const cv::Mat& rgba_canvas, const std::string& target_path,
                 const std::vector<menu::FaceEditor::EyePoly>& eye_polys) {
                // Convert RGBA back to BGRA for cv::imwrite (PNG storage
                // expects native channel order in OpenCV).
                cv::Mat bgra;
                cv::cvtColor(rgba_canvas, bgra, cv::COLOR_RGBA2BGRA);
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(target_path).parent_path(), ec);
                // Auto-backup the version we're about to overwrite (last 15 kept)
                // so an edit can always be undone from the Versions menu.
                fvers::backup_current(target_path, 15);
                if (!cv::imwrite(target_path, bgra)) {
                    std::fprintf(stderr, "[editor] save failed: %s\n",
                                 target_path.c_str());
                    return;
                }
                // Persist blink eye polygons (canvas coords) into the face
                // folder's config.json, merging so expressions/blink keys are
                // preserved. Stored as {"points":[[x,y],...]}; the loader fills
                // each polygon to a mask so a region blink only closes the eye(s)
                // inside the shape. Always rewrite both keys so clearing an eye
                // (drawing fewer shapes) removes the stale one.
                {
                    const std::filesystem::path cfgp =
                        std::filesystem::path(target_path).parent_path() / "config.json";
                    json ej = json::object();
                    { std::ifstream ef(cfgp);
                      if (ef) { try { ef >> ej; } catch (...) { ej = json::object(); } }
                      if (!ej.is_object()) ej = json::object(); }
                    auto wr = [&](const char* k, const menu::FaceEditor::EyePoly& poly){
                        json pts = json::array();
                        for (const auto& p : poly) pts.push_back({p.x, p.y});
                        ej[k] = {{"points", std::move(pts)}};
                    };
                    ej.erase("eye_left"); ej.erase("eye_right");
                    if (!eye_polys.empty())          wr("eye_left",  eye_polys[0]);
                    if (eye_polys.size() > 1)        wr("eye_right", eye_polys[1]);
                    // draw_size lets single-panel faces scale regions; multi-
                    // panel slices use canvas coords directly (ignored there).
                    ej["draw_size"] = {rgba_canvas.cols, rgba_canvas.rows};
                    std::ofstream of(cfgp);
                    if (of) of << ej.dump(2);
                }
                // Rebuild the face loader so the new PNG shows up immediately
                // — then pop the saved expression on-face so the user sees
                // their work without leaving the menu. set_face_by_name
                // falls back to neutral gracefully when the name isn't an
                // expression in the loader's set (mouth-shape PNGs).
                if (native_ctrl) native_ctrl->reload_active_face();
                face_proxy.set_face_by_name(expression);
            },
            /* on_cancel */ {},
            /* on_preview */ [&native_ctrl, expression, &face_proxy]
                (const cv::Mat& rgba_canvas, double duration_s) {
                if (!native_ctrl) return;
                // Pop the expression so the user is looking at it, then push
                // the in-progress canvas as a transient. The renderer thread
                // will composite material + effects on top.
                face_proxy.set_face_by_name(expression);
                native_ctrl->push_transient_face(expression, rgba_canvas, duration_s);
            },
            /* live_frame */ [&native_ctrl](cv::Mat& out) -> bool {
                return native_ctrl && native_ctrl->latest_frame(out);
            },
            /* preview_duration_s */ pf_preview_duration_s);
    };

    // Now that face_proxy exists, hook the audio engine's per-period
    // (volume, mouth_open) into it. Also pushes the analyzer's classified
    // viseme shape so the FaceLoader blends the right overlay; when visemes
    // are disabled we don't push at all and the FaceLoader's "mouth_open"
    // default stays selected.
    audio.set_face_drive_callback(
        [&face_proxy, &accessory_leds, voice = audio.voice()](double vol, double mouth) {
            // Accessory LEDs use the analyzer's broadband volume for any zone
            // running Pattern::Level — even if mouth-open is disabled.
            accessory_leds.set_audio_volume(static_cast<float>(vol));
            if (voice && voice->visemes_enabled())
                face_proxy.set_mouth_shape(voice->mouth_shape());
            face_proxy.set_audio_drive(vol, mouth);
        });

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

    // menu_ptr / boop_sensor_ptr are forward-declared near the top of main();
    // they live here as nullptrs and get pointed at the real objects further
    // down. bg_lib_ptr is local to this block — only the menu hot-swap path
    // captures it and that's all constructed below here.
    bool panel_preview_enabled = false;
    BackgroundLibrary* bg_lib_ptr = nullptr;   // set after bg_lib is constructed
    std::vector<MenuItem> quick_items;   // curated corner/radial quick-menu tree
    std::string cfg_gifs_dir = pf_build_render_config(cfg).gifs_dir;

    // Phone Inbox — watches the configured drop directory (default
    // ~/Downloads) and toasts an Import prompt when a face PNG or GIF
    // appears. Handlers capture face_proxy + cfg_gifs_dir by reference so
    // they hit whichever backend is live at the moment the user accepts.
    const bool pi_enabled = pi_cfg.enabled;
    integrations::PhoneInbox phone_inbox(
        state, std::move(pi_cfg),
        /* import_face */
        [&face_proxy](const std::string& src_path,
                      const std::string& expression) -> bool {
            return face_proxy.import_face_image(expression, src_path);
        },
        /* import_gif */
        [&face_proxy, &cfg_gifs_dir](const std::string& src_path,
                                     const std::string& filename) -> bool {
            namespace fs = std::filesystem;
            if (cfg_gifs_dir.empty()) return false;
            std::error_code ec;
            fs::create_directories(cfg_gifs_dir, ec);
            const fs::path dst = fs::path(cfg_gifs_dir) / filename;
            fs::copy_file(src_path, dst,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) return false;
            // Bind to the first empty slot so the user can play it
            // immediately via the GIFs quick menu. If every slot is
            // bound, fall through and let the user pick one manually
            // later — the file still lives in gifs_dir.
            for (uint8_t i = 0; i < 8; ++i) {
                if (face_proxy.gif_slot(i).empty()) {
                    face_proxy.bind_gif_slot(i, filename);
                    break;
                }
            }
            return true;
        });
    if (pi_enabled) {
        if (!phone_inbox.start())
            std::cout << "[phone-inbox] disabled (failed to open watch dir)\n";
        else
            std::cout << "[phone-inbox] watching " << phone_inbox.watch_dir() << "\n";
    } else {
        std::cout << "[phone-inbox] disabled (cfg.phone_inbox.enabled=false)\n";
    }
    std::string cfg_bg_user_dir;
    if (const char* home = std::getenv("HOME"))
        cfg_bg_user_dir = std::string(home) + "/protohud/backgrounds";
    // GL texture handles for USB camera sources — declared before build_menu so the
    // camera image-setting context panels can sample the live feed.  usb_preview_req
    // is set by those panels (1/2/3) and consumed by the render loop below.
    GLuint tex_usb1 = 0, tex_usb2 = 0, tex_usb3 = 0;
    int    usb_preview_req = 0;

    // ── Configurable GPIO switch map ─────────────────────────────────────────
    // Up to kGpioSlots assignable pins; each has a short-press and optional
    // long-press function plus pull bias and polarity. Loaded from
    // cfg["gpio"]["pins"], edited via the GPIO Buttons menu (applies on the next
    // launch — use the Restart action / scripts/restart.sh), and persisted on
    // save. The poller (input::GpioInputs) is built from these further down.
    constexpr int kGpioSlots = 8;
    std::array<input::GpioPinCfg, kGpioSlots> gpio_pins{};
    bool gpio_inputs_enabled = false;
    // Installed once the poller exists (below); the GPIO Buttons menu calls it
    // to rebuild the poller live after edits.
    auto gpio_reload = std::make_shared<std::function<void()>>();
    if (cfg.contains("gpio") && cfg["gpio"].is_object()) {
        const json& jg = cfg["gpio"];
        gpio_inputs_enabled = jval(jg, "enabled", false);
        if (jg.contains("pins") && jg["pins"].is_array()) {
            const auto& arr = jg["pins"];
            for (size_t i = 0; i < arr.size() && i < static_cast<size_t>(kGpioSlots); ++i) {
                if (!arr[i].is_object()) continue;
                const json& jp = arr[i];
                auto& s = gpio_pins[i];
                s.gpio       = jval(jp, "gpio", s.gpio);
                s.active_low = jval(jp, "active_low", s.active_low);
                const std::string pull = jp.value("pull", std::string("up"));
                s.pull       = (pull == "down") ? 2 : (pull == "none" ? 0 : 1);
                s.short_fn   = input::gpio_func_from_id(jp.value("short", std::string("none")));
                s.long_fn    = input::gpio_func_from_id(jp.value("long",  std::string("none")));
                s.long_ms    = std::clamp(jval(jp, "long_ms", s.long_ms), 200, 3000);
            }
        } else {
            // Migrate the legacy fixed 3-button layout so existing configs keep
            // a working menu Select/Back without manual re-assignment.
            gpio_pins[0].gpio     = jval(jg, "button_1_gpio", 17);
            gpio_pins[0].short_fn = input::GpioFunc::MenuSelect;
            gpio_pins[0].long_fn  = input::GpioFunc::MenuBack;
        }
    }

    // ── Optional button/switch coprocessor (input::CoprocInputs) ──────────────
    // Opt-in: when inputs.coprocessor.enabled, an external MCU (RP2350/RP2040)
    // debounces the switches and streams presses to the Pi over USB CDC (or
    // I²C). Events resolve to the same input::GpioFunc dispatch as the GPIO
    // slots, so it's additive (or, with replace_local_gpio, the only source).
    // Reload + status are installed after gpio_dispatch exists (mirrors the
    // GPIO poller's shared-after-construction pattern).
    input::CoprocConfig coproc_cfg;
    auto coproc_reload = std::make_shared<std::function<void()>>();
    auto coproc_status = std::make_shared<std::function<std::string()>>();
    if (cfg.contains("inputs") && cfg["inputs"].is_object() &&
        cfg["inputs"].contains("coprocessor") &&
        cfg["inputs"]["coprocessor"].is_object()) {
        const json& jc = cfg["inputs"]["coprocessor"];
        coproc_cfg.enabled            = jval(jc, "enabled", false);
        coproc_cfg.transport          = jc.value("transport", coproc_cfg.transport);
        coproc_cfg.device             = jc.value("device",    coproc_cfg.device);
        coproc_cfg.baud               = jval(jc, "baud",      coproc_cfg.baud);
        coproc_cfg.i2c_bus            = jc.value("i2c_bus",   coproc_cfg.i2c_bus);
        coproc_cfg.i2c_addr           = jval(jc, "i2c_addr",  coproc_cfg.i2c_addr);
        coproc_cfg.irq_gpio           = jval(jc, "irq_gpio",  coproc_cfg.irq_gpio);
        coproc_cfg.replace_local_gpio = jval(jc, "replace_local_gpio", false);
        coproc_cfg.heartbeat_timeout_ms =
            jval(jc, "heartbeat_timeout_ms", coproc_cfg.heartbeat_timeout_ms);
        if (jc.contains("buttons") && jc["buttons"].is_array()) {
            for (const auto& jb : jc["buttons"]) {
                if (!jb.is_object()) continue;
                const int id = jval(jb, "id", -1);
                if (id < 0) continue;
                coproc_cfg.short_map[id] =
                    input::gpio_func_from_id(jb.value("short", std::string("none")));
                coproc_cfg.long_map[id] =
                    input::gpio_func_from_id(jb.value("long",  std::string("none")));
            }
        }
    }

    // Command FIFO (optional) — write a GpioFunc id to the FIFO to drive ProtoHUD
    // (e.g. a KDE Connect "Run Command": `echo menu_open > /run/protohud/cmd`).
    input::CmdFifoConfig cmd_fifo_cfg;
    if (cfg.contains("inputs") && cfg["inputs"].is_object() &&
        cfg["inputs"].contains("command_fifo") &&
        cfg["inputs"]["command_fifo"].is_object()) {
        const json& jf = cfg["inputs"]["command_fifo"];
        cmd_fifo_cfg.enabled = jval(jf, "enabled", false);
        cmd_fifo_cfg.path    = jf.value("path", cmd_fifo_cfg.path);
    }

    // KDE Connect lists — editable in the Phone menu, applied live to the bridge
    // and persisted to cfg["kdeconnect"]. ignore_list mutes servers/chats;
    // message_apps picks which apps get the big chat toast.
    auto split_csv_vec = [](const std::string& csv) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= csv.size()) {
            const size_t c = csv.find(',', pos);
            std::string tok = csv.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
            const size_t b = tok.find_first_not_of(" \t");
            const size_t e = tok.find_last_not_of(" \t");
            if (b != std::string::npos) out.push_back(tok.substr(b, e - b + 1));
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        return out;
    };
    std::vector<std::string> kdc_ignore, kdc_msgapps;
    {
        const std::string def_msg =
            "Discord,Messages,Messenger,Signal,WhatsApp,Telegram,SMS,Slack";
        std::string ig, ms = def_msg;
        if (cfg.contains("kdeconnect") && cfg["kdeconnect"].is_object()) {
            ig = cfg["kdeconnect"].value("ignore_list",  std::string());
            ms = cfg["kdeconnect"].value("message_apps", def_msg);
        }
        kdc_ignore  = split_csv_vec(ig);
        kdc_msgapps = split_csv_vec(ms);
    }

    // Expression-coupled effects flag (persisted under protoface). Applied to
    // the native renderer now and toggled live from the effects menu.
    bool pf_expr_effects = cfg.contains("protoface") && cfg["protoface"].is_object()
        && cfg["protoface"].value("expression_effects", false);
    if (native_ctrl) native_ctrl->set_expression_effects(pf_expr_effects);

    // Effects Live Preview tick — populated inside build_menu, polled each frame.
    auto pf_live_tick = std::make_shared<std::function<void()>>();
    bool sched_link_pushed = false;   // one-shot guard for "send scheduler link on startup"

    MenuBuildContext menu_ctx;
    menu_ctx.teensy  = &face_proxy;
    // Live rendered face canvas (face + material + effects) for the Effects
    // context-panel preview. Reads the current native_ctrl each call so it keeps
    // working across backend swaps; empty on the Teensy/daemon backends.
    menu_ctx.live_face_frame = [&native_ctrl](cv::Mat& out) -> bool {
        return native_ctrl && native_ctrl->latest_frame(out);
    };
    menu_ctx.xr      = &xr;
    menu_ctx.cameras = &cameras;
    menu_ctx.lora    = &lora;
    menu_ctx.knob    = &knob;
    menu_ctx.audio   = &audio;
    menu_ctx.state   = &state;
    menu_ctx.android_mirror  = &android_mirror;
    menu_ctx.android_overlay = &android_overlay_active;
    menu_ctx.pip_cfg1 = &pip_overlay_cfg1;
    menu_ctx.pip_cfg2 = &pip_overlay_cfg2;
    menu_ctx.pip_cfg3 = &pip_overlay_cfg3;
    menu_ctx.pip_cam1_overlay = &pip_cam1_overlay_active;
    menu_ctx.pip_cam2_overlay = &pip_cam2_overlay_active;
    menu_ctx.pip_cam3_overlay = &pip_cam3_overlay_active;
    menu_ctx.android_cfg = &android_overlay_cfg;
    menu_ctx.hud_col = &hud.colors();
    menu_ctx.hud_cfg = &hud.config();
    menu_ctx.menu_sys_pp = &menu_ptr;
    menu_ctx.mpu9250 = &mpu9250;
    menu_ctx.bno055  = &bno055;
    menu_ctx.bno08x  = &bno086;
    menu_ctx.gif_names = &gif_names;
    menu_ctx.bt_mon = &bt_mon;
    menu_ctx.sys_panel_active   = &sys_panel_active;
    menu_ctx.fps_overlay_active = &fps_overlay_active;
    menu_ctx.state_ptr = &state;
    menu_ctx.active_face_pp = &active_face;
    menu_ctx.teensy_option  = static_cast<IFaceController*>(&teensy);
    // In native mode hide the daemon's Start/Restart + source-switch menu
    // items (null fp_option).
    menu_ctx.fp_option = native_ctrl ? nullptr
                                     : static_cast<IFaceController*>(&protoface_ctrl);
    menu_ctx.panel_preview_pp          = &panel_preview_enabled;
    menu_ctx.protoface_preview_cfg     = &protoface_preview_cfg;
    menu_ctx.protoface_preview_view_pp = &protoface_preview_view;
    menu_ctx.map_dir = cfg_map_dir;
    menu_ctx.left_eye_src  = &left_eye_src;
    menu_ctx.right_eye_src = &right_eye_src;
    menu_ctx.multicam_layout = &multicam_layout;
    menu_ctx.multicam_usb_a  = &multicam_usb_a;
    menu_ctx.multicam_usb_b  = &multicam_usb_b;
    menu_ctx.multicam_top_a  = &multicam_top_a;
    menu_ctx.multicam_top_b  = &multicam_top_b;
    menu_ctx.profiles    = &profiles;
    menu_ctx.hud_presets = &hud_presets;
    menu_ctx.quick_out   = &quick_items;
    menu_ctx.gifs_dir    = cfg_gifs_dir;
    menu_ctx.bg_lib_pp   = &bg_lib_ptr;
    menu_ctx.bg_user_dir = cfg_bg_user_dir;
    menu_ctx.boop_sensor_pp = &boop_sensor_ptr;
    menu_ctx.voice_analyzer = audio.voice();
    menu_ctx.leds = &accessory_leds;
    menu_ctx.fans = &cooling_fans;
    menu_ctx.swap_backend = swap_backend;
    menu_ctx.pf_backend_p = &pf_backend;
    menu_ctx.edit_face    = edit_face;
    menu_ctx.pf_eye_layout_p   = &pf_eye_layout;
    menu_ctx.pf_mouth_layout_p = &pf_mouth_layout;
    menu_ctx.pf_nose_layout_p  = &pf_nose_layout;
    menu_ctx.pf_hub75_p         = &pf_hub75;
    menu_ctx.pf_hub75_layouts_p = &pf_hub75_layouts;
    menu_ctx.pf_hub75_active_p  = &pf_hub75_active;
    menu_ctx.pf_layout_changed = [&]{
        if (!native_ctrl) return;
        native_ctrl->set_active_layout_name(pf_hub75_active);
        // Push per-panel flips live so the orientation
        // toggles take effect on the panels immediately.
        std::vector<std::array<bool, 2>> flips;
        for (int i = 0; i < pf_hub75.panel_count && i < 4; ++i)
            flips.push_back({pf_hub75.flip_x[i], pf_hub75.flip_y[i]});
        native_ctrl->set_panel_flips(flips);
    };
    menu_ctx.pf_blink_enabled_p = &pf_blink_enabled;
    menu_ctx.pf_blink_min_p     = &pf_blink_min;
    menu_ctx.pf_blink_max_p     = &pf_blink_max;
    menu_ctx.pf_blink_dur_p     = &pf_blink_duration;
    menu_ctx.pf_expr_fade_p     = &pf_expr_fade;
    menu_ctx.pf_preview_duration_p = &pf_preview_duration_s;
    menu_ctx.pf_anim_push = [&]{
        if (!native_ctrl) return;
        native_ctrl->set_blink_enabled(pf_blink_enabled);
        native_ctrl->set_blink_timing(pf_blink_min,
                                      pf_blink_max,
                                      pf_blink_duration);
        native_ctrl->set_expression_fade(pf_expr_fade);
        native_ctrl->set_glitch(pf_glitch);
    };
    menu_ctx.pf_set_effect_json = [&](const nlohmann::json& spec){
        if (native_ctrl) native_ctrl->set_effect_json(spec);
    };
    menu_ctx.pf_get_effect_json = [&]() -> nlohmann::json {
        return native_ctrl ? native_ctrl->get_effect_json() : nlohmann::json();
    };
    menu_ctx.pf_set_expr_effects = [&](bool v){
        if (native_ctrl) native_ctrl->set_expression_effects(v);
    };
    menu_ctx.pf_expr_effects_p = &pf_expr_effects;
    menu_ctx.pf_motion_particles_p = &pf_motion_particles;
    menu_ctx.pf_set_motion_particles = [&](bool v){
        pf_motion_particles = v;
        if (native_ctrl) native_ctrl->set_motion_particles(v);
    };
    menu_ctx.pf_weather_effects_p = &pf_weather_effects;
    menu_ctx.pf_set_weather_effects = [&](bool v){
        pf_weather_effects = v;
        weather_fx_resync  = true;   // the render loop re-maps immediately
    };
    menu_ctx.pf_live_tick = pf_live_tick;
    menu_ctx.cfg_root = &cfg;
    // USB camera preview wiring
    menu_ctx.tex_usb1 = &tex_usb1;
    menu_ctx.tex_usb2 = &tex_usb2;
    menu_ctx.tex_usb3 = &tex_usb3;
    menu_ctx.usb_preview_req = &usb_preview_req;
    menu_ctx.pf_gradient_p = &pf_gradient;
    menu_ctx.pf_set_material = [&](const std::string& spec){
        if (native_ctrl) native_ctrl->set_material_spec(spec);
    };
    menu_ctx.pf_restart_renderer = [&]{
        if (!pf_launch_driver || pf_backend != "hub75" || !native_ctrl) {
            Notification n; n.type = NotifType::App;
            n.title = "Panel restart unavailable";
            n.body  = "HUB75 panel driver is not in use.";
            n.auto_dismiss_s = 5.f;
            std::lock_guard<std::mutex> lk(state.mtx);
            state.notifs.push(std::move(n));
            return;
        }
        int gpw = 64, gph = 32, gchain = 2, gpar = 1;
        pf_hub75_driver_geometry(pf_hub75, gpw, gph, gchain, gpar);
        // pf_launch_panel_driver now stops the old driver, waits for it
        // to release the PIO/DMA, then relaunches (see its comment).
        pf_launch_panel_driver(bin_dir, native_ctrl->canvas_width(),
            native_ctrl->canvas_height(), gpw, gph, gchain, gpar,
            pf_hub75.pinout, pf_hub75.color_order, pf_hub75.camera_mode,
            pf_hub75.camera_planes, pf_hub75.camera_temporal_planes);
        Notification n; n.type = NotifType::App;
        n.title = "Panel driver restarted";
        n.body  = "If panels stay dark, check /tmp/panel_driver.log";
        n.auto_dismiss_s = 6.f;
        std::lock_guard<std::mutex> lk(state.mtx);
        state.notifs.push(std::move(n));
    };
    menu_ctx.gpio_pins_p     = gpio_pins.data();
    menu_ctx.gpio_slot_count = kGpioSlots;
    menu_ctx.gpio_inputs_enabled_p = &gpio_inputs_enabled;
    menu_ctx.gpio_reload = gpio_reload;
    menu_ctx.kdc_p        = kdc_menu_ptr;
    menu_ctx.kdc_ignore_p = &kdc_ignore;
    menu_ctx.kdc_msg_p    = &kdc_msgapps;
    menu_ctx.coproc_enabled_p = &coproc_cfg.enabled;
    menu_ctx.coproc_reload = coproc_reload;
    menu_ctx.coproc_status = coproc_status;
    menu_ctx.pf_glitch_p = &pf_glitch;

    MenuSystem menu(build_menu(menu_ctx));
    menu_ptr = &menu;
    menu.set_quick_items(std::move(quick_items));

    // ── Input event marshalling ───────────────────────────────────────────────
    // The SmartKnob, wireless controller, GPIO poller, button coprocessor and
    // command FIFO all deliver events on their own reader/poll threads, but
    // MenuSystem, the toast renderer and the pip/landing flags have no locks —
    // they belong to the render thread. Reader threads post their handlers
    // here; the render loop drains the queue once per frame so everything
    // below actually runs on the render thread. (Keyboard + gamepad already
    // fire on the render thread and stay direct.)
    std::mutex                        input_events_mtx;
    std::deque<std::function<void()>> input_events;
    auto post_input = [&input_events_mtx, &input_events](std::function<void()> fn){
        std::lock_guard<std::mutex> lk(input_events_mtx);
        input_events.push_back(std::move(fn));
    };
    auto drain_input_events = [&input_events_mtx, &input_events]{
        std::deque<std::function<void()>> ev;
        {
            std::lock_guard<std::mutex> lk(input_events_mtx);
            ev.swap(input_events);
        }
        for (auto& fn : ev) fn();
    };

    // Wire wireless controller callbacks now that menu exists. Callbacks fire
    // on the SerialPort reader thread → post to the render thread.
    if (wireless_enabled) {
        wireless.on_menu([&]{ post_input([&]{
            if (menu.is_open()) menu.close(); else menu.open();
        }); });
        wireless.on_select([&]{ post_input([&]{
            if      (menu.is_open())          menu.select();
            else if (hud.toast_has_focused()) hud.toast_select(state);
        }); });
        wireless.on_back([&]{ post_input([&]{
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        }); });
        // Editor lockdown — when the face editor is the active overlay the
        // wireless cursor must not page through the menu underneath. Up/Down
        // still route through menu.navigate (which forwards to face_editor_'s
        // vertical cursor step); Left/Right route through menu.back/select,
        // which would cancel/paint inside the editor — drop those.
        // Same editing_value() flip as the keyboard/gamepad handlers, so Up
        // always increases a slider value (it used to decrease here only).
        wireless.on_nav_up   ([&]{ post_input([&]{
            if (menu.is_open()) menu.navigate(menu.editing_value() ? +1 : -1); }); });
        wireless.on_nav_down ([&]{ post_input([&]{
            if (menu.is_open()) menu.navigate(menu.editing_value() ? -1 : +1); }); });
        wireless.on_nav_left ([&]{ post_input([&]{
            if (menu.is_face_editor_open())   return;
            // Color picker: Left/Right move within the SV square / hue strip /
            // swatch rows — unless the OSK is up on top (hex entry).
            if (menu.is_color_picker_open() && !menu.is_keyboard_open())
                { menu.overlay_move(-1, 0); return; }
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        }); });
        wireless.on_nav_right([&]{ post_input([&]{
            if (menu.is_face_editor_open())   return;
            if (menu.is_color_picker_open() && !menu.is_keyboard_open())
                { menu.overlay_move(+1, 0); return; }
            if      (hud.toast_has_focused()) hud.toast_navigate(+1);
            else if (menu.is_open())          menu.select();
        }); });
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
        menu.set_ui_scale        (jval  (jm, "ui_scale",         menu.ui_scale()));
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

    // Seed the shared color-picker history ("RRGGBB" strings, newest first).
    if (cfg.contains("color_history") && cfg["color_history"].is_array()) {
        std::vector<uint32_t> hist;
        for (const auto& s : cfg["color_history"])
            if (s.is_string())
                hist.push_back(static_cast<uint32_t>(
                    std::strtoul(s.get<std::string>().c_str(), nullptr, 16)) & 0xFFFFFFu);
        menu::ColorPicker::set_history(hist);
    }

    // Quick (corner/radial) menu style + user-pinned favorites.
    if (cfg.contains("quick_menu")) {
        auto& jq = cfg["quick_menu"];
        std::string st = jq.value("style", std::string("radial"));
        menu.set_quick_style(st == "list" ? QuickStyle::List : QuickStyle::Radial);
        menu.set_radial_tilt(jval(jq, "tilt", menu.radial_tilt()));
        if (jq.contains("favorites") && jq["favorites"].is_array()) {
            std::lock_guard<std::mutex> lk(state.mtx);
            for (auto& k : jq["favorites"])
                if (k.is_string()) state.quick_favorites.insert(k.get<std::string>());
        }
    }

    menu.set_detent_callback([&knob, &menu](int count) {
        knob.set_detents(count);
        // Menus wrap (MenuSystem::navigate uses % n), so the knob must stay
        // free-spinning — keep it unbounded (max <= min). set_range() with real
        // endstops is for non-wrapping ranges (e.g. a 0-100 slider), not menus.
        knob.set_range(/*min*/0, /*max*/0, /*spacing_deg*/count ? 360 / count : 0);
        int depth = menu.menu_depth();
        uint8_t amp = 200, freq = 80, strength = 150;
        if      (depth >= 3) { amp = 150; freq = 60; strength = 100; }
        else if (depth == 1) { amp = 255; freq = 100; strength = 200; }
        knob.set_haptic(amp, freq, strength);
    });

    // ── Startup landing page state (driven by the input callbacks below) ──────
    BackgroundLibrary bg_lib;
    {
        std::vector<std::string> bg_dirs;
        bg_dirs.push_back(bin_dir + "/../assets/backgrounds");
        if (!cfg_bg_user_dir.empty()) bg_dirs.push_back(cfg_bg_user_dir);
        bg_lib.scan(bg_dirs);
    }
    bg_lib_ptr = &bg_lib;   // expose the library to the Files > Backgrounds menu
    // page 0 = main (CONTINUE / PROFILES / QUIT); page 1 = profile picker.
    struct LandingState {
        bool   active       = true;
        int    page         = 0;
        int    cursor       = 0;
        bool   countdown_on = true;
        double deadline     = 0.0;   // glfwGetTime() value the auto-continue fires at
    };
    LandingState landing;
    // Running from a profile file → skip the landing page (avoids a re-exec loop).
    if (!active_profile_name.empty()) landing.active = false;
    // User opted to bypass the landing screen (System > Skip Startup Screen).
    if (state.skip_landing) landing.active = false;
    // ── Temporarily disabled (landing page + profile picker). Keep the
    // surrounding render / nav code in place so re-enabling is a one-line
    // delete. Pairs with the splash_cfg.enabled = false above.
    landing.active = false;
    landing.countdown_on = (landing_continue_timeout_s > 0.0) && landing.active;

    auto landing_count = [&landing, &profiles]() -> int {
        return (landing.page == 0) ? 3 : (profiles.count() + 1);  // +1 = BACK
    };
    auto landing_cancel_countdown = [&landing]{ landing.countdown_on = false; };
    auto landing_nav = [&landing, &landing_count, &landing_cancel_countdown](int d){
        landing_cancel_countdown();
        int n = landing_count();
        if (n <= 0) { landing.cursor = 0; return; }
        landing.cursor = ((landing.cursor + d) % n + n) % n;
    };
    // Resume = relaunch into the last-loaded profile if there is one, else just
    // dismiss the landing page and run with the current config.
    auto landing_resume = [&landing, &profiles, &pending_reexec, &state]{
        std::string lp = profiles.last_path();
        if (!lp.empty()) { pending_reexec = lp; state.quit = true; }
        landing.active = false;
    };
    auto landing_load = [&landing, &profiles, &pending_reexec, &state](int idx){
        if (idx < 0 || idx >= profiles.count()) return;
        profiles.set_last(profiles.name(idx));
        pending_reexec = profiles.path(idx);
        state.quit     = true;
        landing.active = false;
    };
    auto landing_select = [&landing, &state, &profiles, &landing_resume, &landing_load,
                           &landing_cancel_countdown]{
        landing_cancel_countdown();
        if (landing.page == 0) {
            switch (landing.cursor) {
                case 0: landing_resume();                              break;  // Continue
                case 1: landing.page = 1; landing.cursor = 0;         break;  // Profiles
                case 2: state.quit = true; landing.active = false;   break;  // Quit
            }
        } else {
            if (landing.cursor < profiles.count()) landing_load(landing.cursor);
            else { landing.page = 0; landing.cursor = 1; }                    // Back
        }
    };

    // Knob callbacks fire on the SmartKnob serial reader thread → post to the
    // render thread (same marshalling as the wireless controller above).
    knob.on_move([&](int8_t dir, int) { post_input([&, dir]{
        // if (hud.popup_active())    hud.popup_navigate(dir);  // modal popup disabled
        if      (menu.is_keyboard_open()) menu.osk_step(dir);
        else if (landing.active)        landing_nav(dir);
        else if (menu.is_open())        menu.navigate(dir);
        else if (hud.toast_has_focused()) hud.toast_navigate(dir);
    }); });

    knob.on_status([&state](uint8_t status, uint8_t param) {
        if      (status == KnobStatus::READY) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.health.knob_ready = true;
            std::cout << "[knob] ready\n";
        }
        else if (status == KnobStatus::ENTERING_SLEEP) std::cout << "[knob] entering sleep\n";
        else if (status == KnobStatus::WOKE_UP)        std::cout << "[knob] woke up reason=" << (int)param << "\n";
    });

    knob.on_button([&](uint8_t btn, uint8_t ev) { post_input([&, btn, ev]{
        if (landing.active) {
            if ((ev == KnobButtonEvent::PRESS || ev == KnobButtonEvent::LONG_PRESS)
                && btn == KnobButton::ENCODER)
                landing_select();
            return;
        }
        if (ev == KnobButtonEvent::DOUBLE_TAP) {
            // Double-tap BACK = start/stop video recording (Start toggles).
            if (btn == KnobButton::BACK) {
                std::lock_guard<std::mutex> lk(state.mtx);
                state.video_request = VideoRequest::Start;
            }
            return;
        }
        if (ev != KnobButtonEvent::PRESS && ev != KnobButtonEvent::LONG_PRESS) return;
        if (btn == KnobButton::ENCODER) {
            if (ev == KnobButtonEvent::LONG_PRESS) {
                if (menu.is_open()) menu.close(); else menu.open();
            } else {
                if      (menu.is_open())          menu.select();
                else if (hud.toast_has_focused()) hud.toast_select(state);
            }
        } else if (btn == KnobButton::BACK) {
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        }
    }); });

    // ── GPIO switch input (configurable map) ──────────────────────────────────
    bool pip_left_active  = false, pip_right_active  = false;  // PiP toggle state
    bool kb_pip_left      = false, kb_pip_right      = false;  // keyboard-driven

    // Edge-detection state for direct GLFW key polling
    bool prev_key[12] = {};  // slots: 1=K1 2=K2 3=K3 4=K4 5=, 6=. 7=K5 8=K6 9=K7

    // USB stream lifecycle: track previous combined pip-active state per slot
    bool prev_p1 = false, prev_p2 = false, prev_p3 = false;

    // Resolve scripts/restart.sh relative to the running binary so the System:
    // Restart GPIO function works regardless of install location.
    std::string restart_script;
    {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            std::string p(exe);                                       // .../build/protohud
            auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
            s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
            restart_script = p + "/scripts/restart.sh";
        }
    }

    // Map an assigned function to its action. gpio_apply always runs on the
    // RENDER thread — the GPIO / coprocessor / command-FIFO reader threads go
    // through gpio_dispatch below, which posts onto the per-frame input queue.
    // (The menu, toasts and the pip flags have no locks of their own, so
    // touching them from a reader thread was a use-after-free waiting on a
    // button press.)
    // Face expression jump + "return to set face": the first jump remembers the
    // expression that was playing, so FaceReturn snaps back to it. Guarded because
    // dispatch can fire from the GPIO / coprocessor / command-FIFO threads.
    std::string cmd_base_face;
    bool        cmd_face_jumped = false;
    std::mutex  cmd_face_mtx;
    auto jump_face = [&](const std::string& name) {
        {
            std::lock_guard<std::mutex> lk(cmd_face_mtx);
            if (!cmd_face_jumped) {
                cmd_base_face   = face_proxy.current_expression();
                cmd_face_jumped = true;
            }
        }
        face_proxy.set_face_by_name(name);
    };
    auto return_face = [&] {
        std::string base; bool had;
        {
            std::lock_guard<std::mutex> lk(cmd_face_mtx);
            had = cmd_face_jumped; base = cmd_base_face; cmd_face_jumped = false;
        }
        if (had) face_proxy.set_face_by_name(base.empty() ? "neutral" : base);
    };
    auto jump_material = [&](int idx) {
        face_proxy.set_menu_item(8, static_cast<uint8_t>(idx));
        std::lock_guard<std::mutex> lk(state.mtx);
        state.face.material_color = static_cast<uint8_t>(idx);
    };

    auto gpio_apply = [&, restart_script](input::GpioFunc f) {
        using F = input::GpioFunc;
        switch (f) {
        case F::BoopSnout: fire_boop(sensor::BoopSensor::Zone::Snout);      break;
        case F::BoopLeft:  fire_boop(sensor::BoopSensor::Zone::LeftCheek);  break;
        case F::BoopRight: fire_boop(sensor::BoopSensor::Zone::RightCheek); break;
        case F::BoopBoth:  fire_boop(sensor::BoopSensor::Zone::BothCheeks); break;
        case F::MenuOpen:  if (menu.is_open()) menu.close(); else menu.open(); break;
        case F::MenuSelect:
            if      (menu.is_open())          menu.select();
            else if (hud.toast_has_focused()) hud.toast_select(state);
            else                              menu.open();
            break;
        case F::MenuBack:  if (menu.is_open()) menu.back(); break;
        case F::SystemRestart:
            if (!restart_script.empty())
                std::system(("setsid '" + restart_script +
                             "' </dev/null >/tmp/protohud-restart.log 2>&1 &").c_str());
            break;
        case F::SystemShutdown:
            std::system("sudo -n poweroff 2>/dev/null || poweroff 2>/dev/null &");
            break;
        case F::CamAfLeft:  if (cameras.owl_left())  cameras.owl_left()->start_autofocus();  break;
        case F::CamAfRight: if (cameras.owl_right()) cameras.owl_right()->start_autofocus(); break;
        case F::CamPipLeft:  pip_left_active  = !pip_left_active;  break;
        case F::CamPipRight: pip_right_active = !pip_right_active; break;
        case F::CamCaptureLeft:
            { std::lock_guard<std::mutex> lk(state.mtx); state.capture_request = CaptureRequest::Left; }  break;
        case F::CamCaptureRight:
            { std::lock_guard<std::mutex> lk(state.mtx); state.capture_request = CaptureRequest::Right; } break;
        case F::CamSwap:
            { std::lock_guard<std::mutex> lk(state.mtx); state.cameras_swapped = !state.cameras_swapped; } break;
        case F::PhoneRing: if (kdc_menu_ptr) kdc_menu_ptr->ring_phone(); break;
        case F::FaceNeutral:    jump_face("neutral");   break;
        case F::FaceHappy:      jump_face("happy");     break;
        case F::FaceAngry:      jump_face("angry");     break;
        case F::FaceSad:        jump_face("sad");       break;
        case F::FaceSurprised:  jump_face("surprised"); break;
        case F::FaceReturn:     return_face();          break;
        case F::MatRainbow:     jump_material(8);       break;
        case F::MatPride:       jump_material(22);      break;
        case F::MatProgress:    jump_material(23);      break;
        case F::MatTrans:       jump_material(24);      break;
        case F::MatBi:          jump_material(25);      break;
        case F::MatPan:         jump_material(26);      break;
        case F::MatLesbian:     jump_material(27);      break;
        case F::MatNonbinary:   jump_material(28);      break;
        case F::MatAsexual:     jump_material(29);      break;
        case F::MatGenderfluid: jump_material(30);      break;
        case F::MatGenderqueer: jump_material(31);      break;
        case F::MatAromantic:   jump_material(32);      break;
        case F::MatIntersex:    jump_material(33);      break;
        // ── Camera / capture / display helpers ───────────────────────────────
        case F::CamCaptureStereo:
            { std::lock_guard<std::mutex> lk(state.mtx); state.capture_request = CaptureRequest::Stereo; } break;
        case F::RecToggle:
            { std::lock_guard<std::mutex> lk(state.mtx);
              state.video_request = state.video_recording ? VideoRequest::Stop : VideoRequest::Start; } break;
        case F::CamZoomIn:
            { std::lock_guard<std::mutex> lk(state.mtx);
              const float z = std::clamp(state.zoom_left.zoom + 0.25f, 1.0f, 3.0f);
              state.zoom_left.zoom = z; state.zoom_right.zoom = z; } break;
        case F::CamZoomOut:
            { std::lock_guard<std::mutex> lk(state.mtx);
              const float z = std::clamp(state.zoom_left.zoom - 0.25f, 1.0f, 3.0f);
              state.zoom_left.zoom = z; state.zoom_right.zoom = z; } break;
        case F::NightVisionToggle:
            { std::lock_guard<std::mutex> lk(state.mtx); state.night_vision.nv_enabled = !state.night_vision.nv_enabled; } break;
        case F::TheaterToggle:
            { std::lock_guard<std::mutex> lk(state.mtx); state.theater_mode = !state.theater_mode; } break;
        case F::XrRecenter:     xr.recenter_tracking(); break;
        // ── Face browse + look adjust ────────────────────────────────────────
        case F::FaceNext:       face_proxy.next_expression(); break;
        case F::FacePrev:       face_proxy.prev_expression(); break;
        case F::MaterialNext: {
            int next;
            { std::lock_guard<std::mutex> lk(state.mtx); next = (state.face.material_color + 1) % 34; }
            jump_material(next);
        } break;
        case F::FaceBrightUp: {
            int b;
            { std::lock_guard<std::mutex> lk(state.mtx);
              b = std::clamp(static_cast<int>(state.face.brightness) + 25, 0, 255);
              state.face.brightness = static_cast<uint8_t>(b); }
            face_proxy.set_brightness(static_cast<uint8_t>(b));
        } break;
        case F::FaceBrightDown: {
            int b;
            { std::lock_guard<std::mutex> lk(state.mtx);
              b = std::clamp(static_cast<int>(state.face.brightness) - 25, 0, 255);
              state.face.brightness = static_cast<uint8_t>(b); }
            face_proxy.set_brightness(static_cast<uint8_t>(b));
        } break;
        case F::EffectNext: {
            int id;
            { std::lock_guard<std::mutex> lk(state.mtx);
              id = (state.face.effect_id + 1) % 27;   // pf_effect_names count (None..Circuit)
              state.face.effect_id = static_cast<uint8_t>(id); }
            face_proxy.set_effect(static_cast<uint8_t>(id));
        } break;
        case F::FaceRestart:    face_proxy.restart(); break;
        case F::None: default: break;
        }
    };

    // Shared entry point for the GPIO poller, button coprocessor and command
    // FIFO — their reader threads only enqueue; the render loop applies.
    auto gpio_dispatch = [&post_input, &gpio_apply](input::GpioFunc f) {
        post_input([&gpio_apply, f]{ gpio_apply(f); });
    };

    // Local GPIO polling runs unless a coprocessor is enabled in replace mode
    // (then the coproc is the sole button source).
    auto local_gpio_wanted = [&]{
        return gpio_inputs_enabled &&
               !(coproc_cfg.enabled && coproc_cfg.replace_local_gpio);
    };

    auto gpio_inputs = std::make_unique<input::GpioInputs>(
        std::vector<input::GpioPinCfg>(gpio_pins.begin(), gpio_pins.end()), gpio_dispatch);
    if (local_gpio_wanted() && !gpio_inputs->init())
        std::cerr << "[main] GPIO input map init failed (no pins assigned or chip busy)\n";

    // Live reload: tear down the poll thread + release the lines, then rebuild
    // from the current slots. Runs on the main thread (a menu action), so the
    // old GpioInputs dtor joins its thread before the new one starts.
    *gpio_reload = [&]{
        gpio_inputs.reset();
        gpio_inputs = std::make_unique<input::GpioInputs>(
            std::vector<input::GpioPinCfg>(gpio_pins.begin(), gpio_pins.end()), gpio_dispatch);
        if (local_gpio_wanted() && !gpio_inputs->init())
            std::cerr << "[main] GPIO reload: init failed (no pins assigned or chip busy)\n";
        else
            std::cout << "[gpio] input map reloaded\n";
    };

    // ── Button coprocessor source (optional, opt-in) ─────────────────────────
    // Shares gpio_dispatch with the GPIO poller. Reload rebuilds it on a menu
    // toggle; status surfaces the link state to the GPIO Buttons menu.
    auto coproc_inputs = std::make_unique<input::CoprocInputs>(coproc_cfg, gpio_dispatch);
    if (coproc_cfg.enabled && !coproc_inputs->init())
        std::cerr << "[main] button coprocessor init failed (transport unavailable)\n";

    *coproc_reload = [&]{
        coproc_inputs.reset();
        coproc_inputs = std::make_unique<input::CoprocInputs>(coproc_cfg, gpio_dispatch);
        if (coproc_cfg.enabled && !coproc_inputs->init())
            std::cerr << "[main] coprocessor reload: init failed (transport unavailable)\n";
        // Re-evaluate the local poller — replace-mode may have just changed
        // whether GPIO should be running.
        if (gpio_reload && *gpio_reload) (*gpio_reload)();
    };
    *coproc_status = [&]() -> std::string {
        if (!coproc_cfg.enabled)        return "disabled";
        if (!coproc_inputs)             return "offline";
        return coproc_inputs->connected() ? "connected" : "offline";
    };

    // ── Command FIFO source (optional) ───────────────────────────────────────
    // Same shared gpio_dispatch — a line written to the FIFO is a GpioFunc id.
    input::CmdFifo cmd_fifo(cmd_fifo_cfg, gpio_dispatch);
    if (cmd_fifo_cfg.enabled && !cmd_fifo.start())
        std::cerr << "[main] command FIFO init failed (" << cmd_fifo_cfg.path << ")\n";

    // ── Gamepad (SDL2, optional) ──────────────────────────────────────────────
    GamepadInput gamepad;
    gamepad.init();
    // Expanded-map (Helldivers view) pan/zoom helpers — shared by gamepad + keyboard.
    auto map_pan = [&state](float dx, float dy){
        auto& mo = state.map_overlay;
        mo.view_pan_x = std::clamp(mo.view_pan_x + dx, -0.6f, 0.6f);
        mo.view_pan_y = std::clamp(mo.view_pan_y + dy, -0.6f, 0.6f);
    };
    auto map_zoom = [&state](float dz){
        auto& mo = state.map_overlay;
        mo.view_zoom = std::clamp(mo.view_zoom + dz, 1.0f, 6.0f);
    };
    gamepad.on_menu([&menu, &landing, &state]{
        if (landing.active) return;             // Start does nothing on the landing page
        if (state.map_overlay.expanded) { state.map_overlay.expanded = false; return; }
        // Start opens the radial QUICK menu (the deep menu is reachable from its
        // "Full Settings" wedge, or via keyboard F1).
        if      (menu.is_deep_open()) menu.close_deep();
        else if (menu.is_open())      menu.close();
        else                          menu.open();
    });
    gamepad.on_select([&menu, &hud, &state, &landing, &landing_select]{
        if      (state.map_overlay.expanded)   // A toggles map rotation lock
            state.map_overlay.rotate_with_heading = !state.map_overlay.rotate_with_heading;
        else if (landing.active)          landing_select();
        else if (menu.is_open())          menu.select();
        else if (hud.toast_has_focused()) hud.toast_select(state);
    });
    gamepad.on_back([&menu, &hud, &landing, &state]{
        if      (state.map_overlay.expanded) state.map_overlay.expanded = false;
        else if (menu.is_keyboard_open()) menu.osk_backspace();
        else if (landing.active)          { if (landing.page == 1) { landing.page = 0; landing.cursor = 1; landing.countdown_on = false; } }
        else if (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_up   ([&menu, &landing, &landing_nav, &state, &map_pan]{ if (state.map_overlay.expanded){map_pan(0,+0.06f);return;} if (menu.is_keyboard_open()){menu.osk_move(0,-1);return;} if (landing.active){landing_nav(-1);return;} if (menu.is_open()) menu.navigate(menu.editing_value() ? +1 : -1); });
    gamepad.on_nav_down ([&menu, &landing, &landing_nav, &state, &map_pan]{ if (state.map_overlay.expanded){map_pan(0,-0.06f);return;} if (menu.is_keyboard_open()){menu.osk_move(0,+1);return;} if (landing.active){landing_nav(+1);return;} if (menu.is_open()) menu.navigate(menu.editing_value() ? -1 : +1); });
    gamepad.on_nav_left ([&menu, &hud, &landing, &bg_lib, &state, &map_pan]{
        if (menu.is_face_editor_open())   return;   // editor owns the d-pad
        if      (state.map_overlay.expanded) map_pan(+0.06f, 0);
        else if (menu.is_keyboard_open()) menu.osk_move(-1, 0);
        else if (menu.is_color_picker_open()) menu.overlay_move(-1, 0);
        else if (landing.active)          bg_lib.prev();
        else if (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_right([&menu, &hud, &state, &landing, &bg_lib, &map_pan]{
        if (menu.is_face_editor_open())   return;
        if      (state.map_overlay.expanded) map_pan(-0.06f, 0);
        else if (menu.is_keyboard_open()) menu.osk_move(+1, 0);
        else if (menu.is_color_picker_open()) menu.overlay_move(+1, 0);
        else if (landing.active)          bg_lib.next();
        else if (hud.toast_has_focused()) hud.toast_navigate(+1);
        else if (menu.is_open())          menu.select();
    });
    // LB/RB: zoom the expanded map, else cycle the landing background, switch
    // deep-menu tabs while it's open, otherwise toggle the PiPs. While the
    // editor is open the shoulder buttons cycle the palette (via the menu_system
    // handler) so we must not also tab through the menu underneath.
    gamepad.on_pip_left ([&menu, &kb_pip_left, &landing, &bg_lib, &state, &map_zoom] {
        if (menu.is_face_editor_open())  return;
        if      (state.map_overlay.expanded) map_zoom(-0.4f);
        else if (landing.active)        bg_lib.prev();
        else if (menu.is_deep_open())   menu.prev_tab();
        else                            kb_pip_left  = !kb_pip_left;
    });
    gamepad.on_pip_right([&menu, &kb_pip_right, &landing, &bg_lib, &state, &map_zoom]{
        if (menu.is_face_editor_open())  return;
        if      (state.map_overlay.expanded) map_zoom(+0.4f);
        else if (landing.active)        bg_lib.next();
        else if (menu.is_deep_open())   menu.next_tab();
        else                            kb_pip_right = !kb_pip_right;
    });
    gamepad.on_af([&cameras]{
        if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
        if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
    });
    gamepad.on_capture([&state]{
        std::lock_guard<std::mutex> lk(state.mtx);
        state.capture_request = CaptureRequest::Stereo;
    });

    // ── GL texture handles for camera sources ─────────────────────────────────
    // tex_usb1/2/3 and usb_preview_req are declared above (before build_menu).

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

    // ── Signal handling: Ctrl+C / SIGTERM → graceful quit + 5s force-kill ──────
    // The handler only touches atomics — the old version constructed a
    // std::thread (allocates) inside the handler, which deadlocks on the
    // malloc lock if the signal lands mid-allocation on any thread. The
    // watchdog thread below enforces the 5s force-exit deadline instead.
    static std::atomic<bool>    g_sig_quit{false};
    static std::atomic<int64_t> g_sig_quit_at{0};
    {
        static std::atomic<bool>* g_quit = &state.quit;
        auto handler = [](int) {
            if (g_quit) g_quit->store(true);
            // time(2) is async-signal-safe; record the deadline start.
            g_sig_quit_at.store(static_cast<int64_t>(::time(nullptr)));
            g_sig_quit.store(true);
        };
        std::signal(SIGINT,  handler);
        std::signal(SIGTERM, handler);
    }

    // ── Render-loop watchdog: force-exit if the loop stalls for 8 s ──────────
    std::atomic<uint64_t> wd_heartbeat { 0 };
    std::atomic<bool>     wd_stop      { false };
    std::thread watchdog([&wd_heartbeat, &wd_stop] {
        uint64_t prev  = 0;
        int      stall = 0;
        while (!wd_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (wd_stop.load(std::memory_order_relaxed)) break;
            // Signal-initiated quit: if cleanup stalls past 5s, force a CLEAN
            // exit. Exit 0, not 1: SIGINT/SIGTERM is an explicit quit request
            // (Ctrl+C, `kill`, or the watchdog script forwarding a stop), and
            // scripts/watchdog.sh treats non-zero as a crash and relaunches.
            if (g_sig_quit.load(std::memory_order_relaxed)) {
                if (::time(nullptr) -
                        g_sig_quit_at.load(std::memory_order_relaxed) >= 5) {
                    std::cerr << "[signal] cleanup timed out — forcing clean exit\n";
                    std::_Exit(0);
                }
                continue;   // render loop is exiting — skip the stall check
            }
            uint64_t cur = wd_heartbeat.load(std::memory_order_relaxed);
            stall = (cur == prev) ? stall + 1 : 0;
            prev  = cur;
            if (stall >= 8) {
                std::cerr << "[watchdog] render loop stalled for 8 s — forcing exit\n";
                std::_Exit(1);
            }
        }
    });

    // ── Main render loop ──────────────────────────────────────────────────────

    KeyRepeat rep_nav_up, rep_nav_down, rep_toast_prev, rep_toast_next;

    // M long-press state: short tap = toggle map; hold 1.5 s = cycle next map
    double m_press_t    = -1.0;
    bool   m_long_fired = false;

    // Video recorder — driven once per frame from the render thread.
    VideoRecorder video_recorder;

    // ── Startup landing page ─────────────────────────────────────────────────
    // Halo-style screen shown after the splash; gates startup until the user picks
    // Continue (resume last profile), opens the Profiles picker, or quits. If the
    // Continue countdown runs out, the last-loaded profile auto-loads. Background
    // comes from the image library; camera/serial threads keep running underneath.
    landing.deadline = glfwGetTime() + landing_continue_timeout_s;
    while (landing.active && !glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {
        wd_heartbeat.fetch_add(1, std::memory_order_relaxed);  // keep the watchdog from force-exiting
        drain_input_events();   // knob/wireless nav posted from reader threads
        gamepad.poll();
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(xr.glfw_window(), &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        hud.set_dt(0.016f);
        hud.begin_menu_frame();

        // Keyboard nav (dev / desktop).
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    landing_nav(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  landing_nav(+1);
        if (landing.page == 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  bg_lib.prev();
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) bg_lib.next();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter))      landing_select();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && landing.page == 1) {
            landing.page = 0; landing.cursor = 1; landing_cancel_countdown();
        }

        // Continue countdown → auto-load the last profile when it expires.
        // NOTE: never break out of this loop mid-frame — begin_menu_frame() has
        // already started an ImGui frame, so we must finish it (render + present)
        // or the next NewFrame() asserts. A selection just clears landing.active;
        // we render one last frame and the while-condition exits cleanly.
        double remaining = 0.0;
        if (landing.active && landing.countdown_on && landing.page == 0) {
            remaining = landing.deadline - glfwGetTime();
            if (remaining <= 0.0) landing_resume();
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const float W = static_cast<float>(fw), H = static_cast<float>(fh);

        // Background image (stretched to fill) + a slight dim, else a gradient.
        GLuint bgt = bg_lib.texture();
        if (bgt) {
            dl->AddImage((ImTextureID)(intptr_t)bgt, {0.f, 0.f}, {W, H});
            dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(0, 0, 0, 90));
        } else {
            dl->AddRectFilledMultiColor({0.f, 0.f}, {W, H},
                IM_COL32(14, 20, 28, 255), IM_COL32(14, 20, 28, 255),
                IM_COL32(4, 7, 11, 255),   IM_COL32(4, 7, 11, 255));
        }

        ImFont* font   = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * menu.ui_scale();
        const ImU32 accent = menu.accent_color();
        const float mx = W * 0.06f, my = H * 0.10f;

        // Title / breadcrumb.
        dl->AddText(font, fs * 1.4f, {mx, my}, IM_COL32(255, 255, 255, 235), "PROTOHUD");
        if (landing.page == 1) {
            ImVec2 tsz = font->CalcTextSizeA(fs * 1.4f, 1e9f, 0.f, "PROTOHUD");
            dl->AddText(font, fs * 1.0f, {mx + tsz.x + 16.f, my + fs * 0.4f},
                        (accent & 0x00FFFFFFu) | (210u << 24), ">  PROFILES");
        }

        // Build the current page's rows.
        std::vector<std::string> rows;
        if (landing.page == 0) {
            rows = { "CONTINUE", "PROFILES", "QUIT" };
        } else {
            for (int i = 0; i < profiles.count(); ++i) rows.push_back(profiles.name(i));
            rows.push_back("BACK");
        }

        // Profile that Continue / the countdown will resume (shown on the row).
        const std::string last_nm = profiles.last_name();

        const float row_h = fs * 1.3f + 18.f;
        const float lw    = W * 0.34f;
        float ly = my + fs * 1.4f + 40.f;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            bool sel = (i == landing.cursor);
            ImVec2 rmin{mx, ly}, rmax{mx + lw, ly + row_h};
            if (sel) dl->AddRectFilled(rmin, rmax, IM_COL32(255, 255, 255, 235));
            ImU32 tcol = sel ? IM_COL32(10, 12, 14, 255) : IM_COL32(230, 235, 240, 210);
            float ty = ly + (row_h - fs * 1.2f) * 0.5f;
            std::string label = rows[i];
            std::transform(label.begin(), label.end(), label.begin(), ::toupper);
            dl->AddText(font, fs * 1.2f, {mx + 14.f, ty}, tcol, label.c_str());
            dl->AddLine({mx, rmax.y}, {mx + lw, rmax.y},
                        (accent & 0x00FFFFFFu) | (70u << 24), 1.f);
            // CONTINUE row: show the profile that will resume + countdown badge,
            // right-aligned (countdown furthest right, profile name to its left).
            if (landing.page == 0 && i == 0) {
                float rx = mx + lw - 12.f;
                if (landing.countdown_on && remaining > 0.0) {
                    char cd[24]; snprintf(cd, sizeof(cd), "%ds", (int)std::ceil(remaining));
                    ImVec2 csz = font->CalcTextSizeA(fs * 0.95f, 1e9f, 0.f, cd);
                    ImU32 cc = sel ? IM_COL32(10, 12, 14, 255) : ((accent & 0x00FFFFFFu) | (210u << 24));
                    dl->AddText(font, fs * 0.95f, {rx - csz.x, ty}, cc, cd);
                    rx -= csz.x + 10.f;
                }
                std::string resume = last_nm.empty()
                    ? std::filesystem::path(cfg_path).stem().string()
                    : last_nm;
                std::transform(resume.begin(), resume.end(), resume.begin(), ::toupper);
                ImVec2 nsz = font->CalcTextSizeA(fs * 0.95f, 1e9f, 0.f, resume.c_str());
                ImU32 nc = sel ? IM_COL32(10, 12, 14, 255) : ((accent & 0x00FFFFFFu) | (185u << 24));
                dl->AddText(font, fs * 0.95f, {rx - nsz.x, ty}, nc, resume.c_str());
            }
            ly += row_h;
        }
        if (landing.page == 1 && profiles.count() == 0) {
            dl->AddText(font, fs * 0.95f, {mx + 14.f, ly + 6.f},
                        (accent & 0x00FFFFFFu) | (150u << 24),
                        "No profiles yet \xC2\xB7 save one in the menu (Profiles tab)");
        }

        const char* hint = (landing.page == 0)
            ? "ENTER / A  SELECT     UP/DOWN  MOVE     LEFT/RIGHT / LB-RB  BACKGROUND"
            : "ENTER / A  LOAD (RESTART)     UP/DOWN  MOVE     ESC / B  BACK";
        dl->AddText(font, fs * 0.95f, {mx, H - my * 0.5f},
                    (accent & 0x00FFFFFFu) | (190u << 24), hint);

        hud.render_menu_overlay();
        xr.present();
    }

    // ── Config snapshot writer ────────────────────────────────────────────────
    // Mutate `cfg` with all current runtime settings (HUD colors/style, vision,
    // cameras, Protoface, menu, etc.). Factored out so both the exit-save AND the
    // "save profile" feature write the exact same snapshot. save_config_to() always
    // writes (used for profile files); the exit path skips writing config.json when
    // it failed to parse on load (no-clobber, handled by the caller).
    auto mutate_cfg = [&] {
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
        cfg["hud"]["compass_tape"]        = state.compass_tape;
        cfg["hud"]["legacy_hud"]          = state.legacy_hud;
        cfg["landing"]["skip"]            = state.skip_landing;
        cfg["hud"]["expanded_show_debug"] = state.expanded_show_debug;
        cfg["hud"]["expanded_hide_info"]  = state.expanded_hide_info;
        {
            static const char* kAxes[] = { "roll", "pitch", "yaw" };
            cfg["compass"]["axis"]   = kAxes[static_cast<int>(state.compass_axis)];
            cfg["compass"]["invert"] = state.compass_invert;
        }
        cfg["hud"]["flip_vertical"]             = hud.config().hud_flip_vertical;
        cfg["hud"]["effects"]["type"]           = static_cast<int>(state.effects_cfg.effect);
        cfg["hud"]["effects"]["palette"]        = static_cast<int>(state.effects_cfg.palette);

        // Boop sensor zones — preserve the hardware-level fields (enabled,
        // i2c_bus, i2c_addr, electrode mapping) and rewrite the user-tunable
        // bits the menu owns.
        {
            auto& jb = cfg["boop"];
            jb["coalesce_window_s"] = state.boop_coalesce_window_s;
            auto& jzones = jb["zones"];
            if (!jzones.is_array() || jzones.size() < 4)
                jzones = json::array({ json{}, json{}, json{}, json{} });
            std::lock_guard<std::mutex> lk(state.mtx);
            for (int i = 0; i < 4; ++i) {
                jzones[i]["enabled"]    = state.boop_zones[i].enabled;
                jzones[i]["expression"] = state.boop_zones[i].expression;
                jzones[i]["duration_s"] = state.boop_zones[i].duration_s;
                jzones[i]["threshold"]  = state.boop_zones[i].threshold;
                jzones[i]["electrode"]  = state.boop_zones[i].electrode;
                const auto& et = state.boop_zones[i].eye_trigger;
                auto& je = jzones[i]["eye_trigger"];
                je["enabled"]    = et.enabled;
                je["count"]      = et.count;
                je["window_s"]   = et.window_s;
                je["anim"]       = et.anim;
                je["speed"]      = et.speed;
                je["size"]       = et.size;
                je["duration_s"] = et.duration_s;
                je["color"]      = json::array({ et.r, et.g, et.b });
            }
        }

        {
            std::lock_guard<std::mutex> lk(state.mtx);
            auto& jls = cfg["light_sensor"];
            jls["enabled"]              = state.light_squint.enabled;
            jls["dark_threshold_lux"]   = state.light_squint.dark_threshold_lux;
            jls["bright_threshold_lux"] = state.light_squint.bright_threshold_lux;
            jls["transition_window_s"]  = state.light_squint.transition_window_s;
            jls["expression"]           = state.light_squint.expression;
            jls["duration_s"]           = state.light_squint.duration_s;
            jls["cooldown_s"]           = state.light_squint.cooldown_s;
        }
        cfg["voice_mouth"]["enabled"]             = state.voice_mouth.enabled;
        cfg["voice_mouth"]["sensitivity"]         = state.voice_mouth.sensitivity;
        cfg["voice_mouth"]["noise_gate"]          = state.voice_mouth.noise_gate;
        cfg["voice_mouth"]["attack_ms"]           = state.voice_mouth.attack_ms;
        cfg["voice_mouth"]["release_ms"]          = state.voice_mouth.release_ms;
        cfg["voice_mouth"]["band_lo_hz"]          = state.voice_mouth.band_lo_hz;
        cfg["voice_mouth"]["band_hi_hz"]          = state.voice_mouth.band_hi_hz;
        // Accessory LEDs — pull from the manager's live snapshot so anything
        // the menu changed persists across launches. Hardware-level fields
        // (spi_device / speed_hz / color_order / zone start+count) stay
        // untouched: those are wiring concerns owned by the user's config.
        {
            auto& jl = cfg["accessory_leds"];
            jl["global_brightness"] = static_cast<int>(accessory_leds.global_brightness());
            auto& jzones = jl["zones"];
            if (!jzones.is_array() || jzones.size() < accessory::ZoneCount)
                jzones = json::array({json{}, json{}, json{}, json{}});
            static const char* pat_name[] = { "off", "solid", "breathe", "level" };
            for (int i = 0; i < accessory::ZoneCount; ++i) {
                auto zc = accessory_leds.zone(static_cast<accessory::Zone>(i));
                jzones[i]["pattern"]    = pat_name[static_cast<int>(zc.pattern)];
                jzones[i]["color"]      = json::array({ zc.r, zc.g, zc.b });
                jzones[i]["breathe_hz"] = zc.breathe_hz;
            }
        }

        cfg["voice_mouth"]["visemes_enabled"]     = state.voice_mouth.visemes_enabled;
        cfg["voice_mouth"]["viseme_round_max_hz"] = state.voice_mouth.viseme_round_max_hz;
        cfg["voice_mouth"]["viseme_open_max_hz"]  = state.voice_mouth.viseme_open_max_hz;
        cfg["voice_mouth"]["viseme_small_max_hz"] = state.voice_mouth.viseme_small_max_hz;

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

        cfg["scheduler"]["lead_minutes"] = state.scheduler_lead_min;

        cfg["display"]["fullscreen"] = state.win_fullscreen.load();
        cfg["display"]["frameless"]  = state.win_frameless.load();

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

        cfg["protoface"]["mode"]                = pf_mode;
        cfg["protoface"]["backend"]             = pf_backend;
        cfg["protoface"]["autostart"]           = pf_autostart;
        cfg["protoface"]["face_colors"]         = state.face.face_colors;
        cfg["protoface"]["pride_sharp"]         = state.face.pride_sharp;
        cfg["protoface"]["motion_particles"]    = pf_motion_particles;
        cfg["protoface"]["weather_effects"]     = pf_weather_effects;
        cfg["protoface"]["pride_angle"]         = state.face.pride_angle;
        cfg["protoface"]["layout"]["eye"]       = pf_eye_layout;
        cfg["protoface"]["layout"]["mouth"]     = pf_mouth_layout;
        cfg["protoface"]["layout"]["nose"]      = pf_nose_layout;
        // Sync the working layout into the named-map before serialising so
        // the in-flight edits the user made to the active layout land on disk.
        pf_hub75_layouts[pf_hub75_active] = pf_hub75;
        cfg["protoface"]["hub75_active"] = pf_hub75_active;
        auto layout_to_json = [](const PfHub75Layout& L) {
            json jh = json::object();
            jh["panel_size"]       = L.panel_size;
            jh["arrangement"]      = L.arrangement;
            jh["panel_count"]      = L.panel_count;
            jh["pinout"]           = L.pinout;
            jh["color_order"]      = L.color_order;
            jh["camera_mode"]            = L.camera_mode;
            jh["camera_planes"]          = L.camera_planes;
            jh["camera_temporal_planes"] = L.camera_temporal_planes;
            jh["panel_size_per"]   = json::array({L.panel_size_per[0], L.panel_size_per[1],
                                                  L.panel_size_per[2], L.panel_size_per[3]});
            jh["defaults_applied"] = L.defaults_applied;
            jh["nudge_dx"]         = json::array({L.nudge_dx[0], L.nudge_dx[1],
                                                  L.nudge_dx[2], L.nudge_dx[3]});
            jh["nudge_dy"]         = json::array({L.nudge_dy[0], L.nudge_dy[1],
                                                  L.nudge_dy[2], L.nudge_dy[3]});
            jh["flip_x"]           = json::array({L.flip_x[0], L.flip_x[1],
                                                  L.flip_x[2], L.flip_x[3]});
            jh["flip_y"]           = json::array({L.flip_y[0], L.flip_y[1],
                                                  L.flip_y[2], L.flip_y[3]});
            return jh;
        };
        json jlayouts = json::object();
        for (const auto& [name, L] : pf_hub75_layouts) jlayouts[name] = layout_to_json(L);
        cfg["protoface"]["hub75_layouts"] = std::move(jlayouts);
        // Drop the legacy single-block key so we don't accumulate stale data
        // from older versions when the user moves between layouts.
        cfg["protoface"].erase("hub75");
        cfg["protoface"]["animation"]["blink_enabled"]   = pf_blink_enabled;
        cfg["protoface"]["animation"]["blink_min"]       = pf_blink_min;
        cfg["protoface"]["animation"]["blink_max"]       = pf_blink_max;
        cfg["protoface"]["animation"]["blink_duration"]  = pf_blink_duration;
        cfg["protoface"]["animation"]["expression_fade"] = pf_expr_fade;
        cfg["protoface"]["animation"]["preview_duration_s"] = pf_preview_duration_s;
        cfg["protoface"]["glitch"] = pf_glitch.to_json();
        {
            auto& jg = cfg["protoface"]["gradient"];
            jg["count"]     = std::clamp(pf_gradient.count, 2, 6);
            jg["smooth"]    = pf_gradient.smooth;
            jg["angle"]     = pf_gradient.angle;
            jg["speed"]     = pf_gradient.speed;
            jg["mirror"]    = pf_gradient.mirror;
            json jcolors = json::array();
            for (int i = 0; i < 6; ++i)
                jcolors.push_back(json::array({pf_gradient.colors[i][0],
                                               pf_gradient.colors[i][1],
                                               pf_gradient.colors[i][2]}));
            jg["colors"] = std::move(jcolors);
        }
        cfg["protoface"]["preview"]["anchor_x"] = protoface_preview_cfg.anchor_x;
        cfg["protoface"]["preview"]["anchor_y"] = protoface_preview_cfg.anchor_y;
        cfg["protoface"]["preview"]["pan_x"]    = protoface_preview_cfg.pan_x;
        cfg["protoface"]["preview"]["pan_y"]    = protoface_preview_cfg.pan_y;
        cfg["protoface"]["preview"]["size"]     = protoface_preview_cfg.size;
        cfg["protoface"]["preview"]["view"]     = protoface_preview_view;

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
        cfg["cameras"]["usb_cam_1"]["width"]             = cameras.usb1_cfg().width;
        cfg["cameras"]["usb_cam_1"]["height"]            = cameras.usb1_cfg().height;
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
        cfg["cameras"]["usb_cam_2"]["width"]             = cameras.usb2_cfg().width;
        cfg["cameras"]["usb_cam_2"]["height"]            = cameras.usb2_cfg().height;
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
        cfg["cameras"]["usb_cam_3"]["width"]             = cameras.usb3_cfg().width;
        cfg["cameras"]["usb_cam_3"]["height"]            = cameras.usb3_cfg().height;
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
            // Configurable GPIO switch map.
            cfg["gpio"]["enabled"] = gpio_inputs_enabled;
            json jpins = json::array();
            for (int i = 0; i < kGpioSlots; ++i) {
                const auto& s = gpio_pins[i];
                json jp;
                jp["gpio"]       = s.gpio;
                jp["active_low"] = s.active_low;
                jp["pull"]       = (s.pull == 2) ? "down" : (s.pull == 0 ? "none" : "up");
                jp["short"]      = input::gpio_func_id(s.short_fn);
                jp["long"]       = input::gpio_func_id(s.long_fn);
                jp["long_ms"]    = s.long_ms;
                jpins.push_back(std::move(jp));
            }
            cfg["gpio"]["pins"] = std::move(jpins);
        }
        {
            // KDE Connect lists (edited in the Phone menu).
            auto join_csv = [](const std::vector<std::string>& v){
                std::string csv;
                for (size_t i = 0; i < v.size(); ++i) { if (i) csv += ','; csv += v[i]; }
                return csv;
            };
            cfg["kdeconnect"]["ignore_list"]  = join_csv(kdc_ignore);
            cfg["kdeconnect"]["message_apps"] = join_csv(kdc_msgapps);
        }
        cfg["notifications"]["persist"] = state.notif_persist;
        {
            static const char* kNames[] = { "center","outside","left","right","top","bottom" };
            cfg["cameras"]["theater_anchor"] = kNames[static_cast<int>(state.theater_anchor)];
        }

        cfg["qr"]["scan_main"] = state.qr_scan_main;
        cfg["qr"]["scan_usb"]  = state.qr_scan_usb;

        cfg["fps_avg_interval_s"] = state.fps_avg_interval_s;
        cfg["i2c_scan_bus"]       = state.i2c_scan_bus;

        cfg["landing"]["continue_timeout_s"] = landing_continue_timeout_s;

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
            cfg["map"]["compass_ring"]        = mo.compass_ring;
            cfg["map"]["battery_arc"]         = mo.battery_arc;
            cfg["map"]["system_debug"]        = mo.system_debug;
            cfg["map"]["clock"]               = mo.clock;
            cfg["map"]["clock_date"]          = mo.clock_date;
            cfg["map"]["portrait"]            = mo.portrait;
            cfg["map"]["portrait_right_half"] = mo.portrait_right_half;
            cfg["map"]["portrait_scale"]      = mo.portrait_scale;
            cfg["map"]["zoom"]                = mo.zoom;
        }

        {
            const auto& ip = state.info_panel;
            cfg["info_panel"]["enabled"]   = ip.enabled;
            cfg["info_panel"]["anchor_x"]  = ip.anchor_x;
            cfg["info_panel"]["anchor_y"]  = ip.anchor_y;
            cfg["info_panel"]["size_px"]    = ip.size_px;
            cfg["info_panel"]["cycle_sec"]  = ip.cycle_sec;
            cfg["info_panel"]["clock_face"] = ip.clock_face;
            json sh = json::array();
            for (int i = 0; i < static_cast<int>(InfoWidget::Count); ++i)
                sh.push_back(ip.show[i]);
            cfg["info_panel"]["show"] = sh;
        }

        cfg["hud_dock"]["bottom"]   = state.hud_dock.bottom;
        cfg["hud_dock"]["v_offset"] = state.hud_dock.v_offset;

        {
            const auto& wc = state.weather_cfg;
            cfg["weather"]["enabled"]      = wc.enabled;
            cfg["weather"]["auto_locate"]  = wc.auto_locate;
            cfg["weather"]["metric"]       = wc.metric;
            cfg["weather"]["lat"]          = wc.lat;
            cfg["weather"]["lon"]          = wc.lon;
            cfg["weather"]["place"]        = wc.place;
            cfg["weather"]["interval_min"] = wc.interval_min;
        }

        // Per-eye resolution lives under each camera's own block now. The legacy
        // top-level "resolution" block (which forced BOTH eyes to one value) is
        // retired on save so independent per-eye settings persist; the loader
        // still honours it for back-compat with configs from older builds.
        cfg["cameras"]["owlsight_left"]["width"]   = state.camera_resolution.width;
        cfg["cameras"]["owlsight_left"]["height"]  = state.camera_resolution.height;
        cfg["cameras"]["owlsight_left"]["fps"]     = state.camera_resolution.fps;
        cfg["cameras"]["owlsight_right"]["width"]  = state.camera_resolution_right.width;
        cfg["cameras"]["owlsight_right"]["height"] = state.camera_resolution_right.height;
        cfg["cameras"]["owlsight_right"]["fps"]    = state.camera_resolution_right.fps;
        cfg.erase("resolution");

        // Persist CSI display rotation so the in-app setting survives a
        // restart (rotation_deg lives next to the camera-id/width/height/fps
        // block where the user already finds the rest of the per-eye config).
        cfg["cameras"]["owlsight_left"]["rotation_deg"]  = cameras.owl_left_rotation();
        cfg["cameras"]["owlsight_right"]["rotation_deg"] = cameras.owl_right_rotation();

        // Per-eye AF/AE/WB/ISP/HDR controls. Refresh state from the live cameras
        // (no-op if a camera is absent — the loaded values are kept), then write
        // them under each camera's "controls" block.
        read_cam_controls(cameras.owl_left(),  state.camera_controls_left);
        read_cam_controls(cameras.owl_right(), state.camera_controls_right);
        write_cam_controls(cfg["cameras"]["owlsight_left"]["controls"],  state.camera_controls_left);
        write_cam_controls(cfg["cameras"]["owlsight_right"]["controls"], state.camera_controls_right);

        auto eye_src_str = [](EyeSource s) -> const char* {
            switch (s) {
                case EyeSource::USB1:      return "usb1";
                case EyeSource::USB2:      return "usb2";
                case EyeSource::USB3:      return "usb3";
                case EyeSource::CSI_LEFT:  return "csi_left";
                case EyeSource::CSI_RIGHT: return "csi_right";
                default:                   return "csi";
            }
        };
        cfg["cameras"]["left_eye_source"]  = eye_src_str(left_eye_src);
        cfg["cameras"]["right_eye_source"] = eye_src_str(right_eye_src);
        cfg["cameras"]["multicam_layout"]  = multicam_layout;
        cfg["cameras"]["multicam_usb_a"]   = eye_src_str(multicam_usb_a);
        cfg["cameras"]["multicam_usb_b"]   = eye_src_str(multicam_usb_b);
        cfg["cameras"]["multicam_top_a"]   = eye_src_str(multicam_top_a);
        cfg["cameras"]["multicam_top_b"]   = eye_src_str(multicam_top_b);

        auto& jm = cfg["menu_style"];
        jm["accent_color"]     = color_to_json(menu.accent_color());
        jm["bg_color"]         = color_to_json(menu.bg_color());
        jm["bg_enabled"]       = menu.bg_enabled();
        jm["border_color"]     = color_to_json(menu.border_color());
        jm["border_thickness"]  = menu.border_thickness();
        jm["border_enabled"]    = menu.border_enabled();
        jm["ui_scale"]          = menu.ui_scale();
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

        // Quick (corner/radial) menu style + pinned favorites.
        cfg["quick_menu"]["style"] =
            (menu.quick_style() == QuickStyle::Radial) ? "radial" : "list";
        cfg["quick_menu"]["tilt"] = menu.radial_tilt();
        {
            json arr = json::array();
            std::lock_guard<std::mutex> lk(state.mtx);
            for (const auto& k : state.quick_favorites) arr.push_back(k);
            cfg["quick_menu"]["favorites"] = arr;
        }

        // Shared color-picker history ("RRGGBB" strings, newest first).
        {
            json arr = json::array();
            char hx[8];
            for (uint32_t c : menu::ColorPicker::get_history()) {
                std::snprintf(hx, sizeof(hx), "%06X", c & 0xFFFFFFu);
                arr.push_back(hx);
            }
            cfg["color_history"] = arr;
        }

        // Persist MPU-9250 enabled state + calibration biases so the menu's
        // "Active" toggle and calibration survive a restart.
        if (mpu9250.is_running() || mpu9250.is_enabled() || cfg.contains("mpu9250")) {
            float bx, by, bz;
            mpu9250.get_mag_bias(bx, by, bz);
            cfg["mpu9250"]["enabled"]        = mpu9250.is_enabled();
            cfg["mpu9250"]["mag_bias"]       = json::array({ bx, by, bz });
            cfg["mpu9250"]["mount_rotation"] = mpu9250.get_mount_rotation();
            cfg["mpu9250"]["heading_axes"]   = mpu9250.get_heading_axes();
        }

        // IMU source selector — map enum back to the string config takes.
        {
            const char* s = "auto";
            switch (state.imu_source) {
            case AppState::ImuSource::Bno08x:  s = "bno086";  break;
            case AppState::ImuSource::Bno055:  s = "bno055";  break;
            case AppState::ImuSource::Mpu9250: s = "mpu9250"; break;
            case AppState::ImuSource::Viture:  s = "viture";  break;
            case AppState::ImuSource::None:    s = "none";    break;
            case AppState::ImuSource::Auto:
            default:                           s = "auto";    break;
            }
            cfg["imu_source"] = s;
        }
    };

    // Snapshot current settings into `cfg` and write them to `path`. Always writes
    // (callers gate config.json on cfg_parse_failed themselves). Returns success.
    auto save_config_to = [&](const std::string& path) -> bool {
        try {
            mutate_cfg();
            FILE* f = fopen(path.c_str(), "w");
            if (!f) { std::cerr << "[cfg] cannot write to " << path << "\n"; return false; }
            std::string s = cfg.dump(2);
            fwrite(s.c_str(), 1, s.size(), f);
            fclose(f);
            std::cout << "[cfg] saved to " << path << "\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[cfg] save failed: " << e.what() << "\n";
            return false;
        }
    };

    // ── HUD/menu preset writer + applier ───────────────────────────────────────
    // A preset is a *small* JSON: just the hud_colors + menu_style (+ a couple hud
    // fields) — the visual subset of the full config. Saved/loaded live with no
    // restart. Reuses mutate_cfg() to capture the current look, and the same
    // setters as startup to apply.
    auto save_hud_preset = [&](const std::string& path) -> bool {
        try {
            mutate_cfg();   // refresh cfg with the current visual state
            json p;
            if (cfg.contains("hud_colors")) p["hud_colors"] = cfg["hud_colors"];
            if (cfg.contains("menu_style")) p["menu_style"] = cfg["menu_style"];
            if (cfg.contains("hud")) {
                if (cfg["hud"].contains("glow_intensity"))
                    p["hud"]["glow_intensity"] = cfg["hud"]["glow_intensity"];
                if (cfg["hud"].contains("indicator_bg_enabled"))
                    p["hud"]["indicator_bg_enabled"] = cfg["hud"]["indicator_bg_enabled"];
            }
            FILE* f = fopen(path.c_str(), "w");
            if (!f) { std::cerr << "[preset] cannot write " << path << "\n"; return false; }
            std::string s = p.dump(2);
            fwrite(s.c_str(), 1, s.size(), f);
            fclose(f);
            std::cout << "[preset] saved " << path << "\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[preset] save failed: " << e.what() << "\n";
            return false;
        }
    };
    auto apply_hud_preset = [&](const std::string& path) -> bool {
        bool pf = false;
        json p = load_config(path, &pf);
        if (pf || !p.is_object()) return false;
        if (p.contains("hud_colors")) {
            auto& jc = p["hud_colors"];
            auto& c  = hud.colors();
            c.glow_base        = jcolor(jc, "glow_base",        c.glow_base);
            c.text_fill        = jcolor(jc, "text_fill",        c.text_fill);
            c.ind_good         = jcolor(jc, "ind_good",         c.ind_good);
            c.ind_inactive     = jcolor(jc, "ind_inactive",     c.ind_inactive);
            c.ind_fail         = jcolor(jc, "ind_fail",         c.ind_fail);
            c.compass_tick     = jcolor(jc, "compass_tick",     c.compass_tick);
            c.compass_glow     = jcolor(jc, "compass_glow",     c.compass_glow);
            c.compass_bg_color = jcolor(jc, "compass_bg_color", c.compass_bg_color);
        }
        if (p.contains("hud")) {
            auto& jh = p["hud"];
            hud.config().glow_intensity       = jval(jh, "glow_intensity",       hud.config().glow_intensity);
            hud.config().indicator_bg_enabled = jval(jh, "indicator_bg_enabled", hud.config().indicator_bg_enabled);
        }
        if (p.contains("menu_style")) {
            auto& jm = p["menu_style"];
            menu.set_accent_color    (jcolor(jm, "accent_color",     menu.accent_color()));
            menu.set_bg_color        (jcolor(jm, "bg_color",         menu.bg_color()));
            menu.set_bg_enabled      (jval  (jm, "bg_enabled",       menu.bg_enabled()));
            menu.set_border_color    (jcolor(jm, "border_color",     menu.border_color()));
            menu.set_border_thickness(jval  (jm, "border_thickness", menu.border_thickness()));
            menu.set_border_enabled  (jval  (jm, "border_enabled",   menu.border_enabled()));
            menu.set_ui_scale        (jval  (jm, "ui_scale",         menu.ui_scale()));
            std::string ss = jm.value("selection_style", std::string("filled_row"));
            menu.set_selection_style(ss == "accent_bar"
                ? SelectionStyle::ACCENT_BAR : SelectionStyle::FILLED_ROW);
            std::string a = jm.value("anchor", std::string("top_left"));
            if      (a == "top_right")    menu.set_anchor(MenuAnchor::TopRight);
            else if (a == "bottom_left")  menu.set_anchor(MenuAnchor::BottomLeft);
            else if (a == "bottom_right") menu.set_anchor(MenuAnchor::BottomRight);
            else                          menu.set_anchor(MenuAnchor::TopLeft);
        }
        return true;
    };

    // Kick off a one-shot autofocus on the OWLsight (CSI) cameras at boot; once it
    // locks (or times out) both settle into SLAVE focus (handled in the loop).
    bool   boot_af_pending = false;
    double boot_af_t0      = 0.0;
    if (cameras.owl_left() || cameras.owl_right()) {
        if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
        if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
        state.focus_left.mode  = CameraFocusState::Mode::AUTO;
        state.focus_right.mode = CameraFocusState::Mode::AUTO;
        boot_af_pending = true;
        boot_af_t0      = glfwGetTime();
    }

    double prev_time = glfwGetTime();
    uint32_t notif_last_saved = state.notifs.next_id;   // debounced log persistence
    size_t   notif_last_size  = state.notifs.items.size();
    double   notif_next_save  = 0.0;

    // Persistent snapshot target — constructing a fresh AppState every frame
    // re-allocated every vector/deque/string member (hundreds of allocations)
    // while holding state.mtx, convoying every sensor/input worker. Reusing
    // one instance lets the copy-assignments below recycle capacity; every
    // field the HUD reads is reassigned under the lock each frame.
    AppState snap;

    // Weather Sync bookkeeping — re-evaluated every ~60 s (and immediately on
    // toggle); only pushed to the face when the mapped spec actually changes.
    auto        weather_fx_last = std::chrono::steady_clock::now() - std::chrono::minutes(2);
    std::string weather_fx_sent = "null";

    while (!glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {
        wd_heartbeat.fetch_add(1, std::memory_order_relaxed);

        // Apply a pending window-mode change (Settings > Fullscreen / Frameless).
        if (state.win_mode_dirty.exchange(false))
            xr.apply_window_mode(state.win_fullscreen.load(), state.win_frameless.load());

        // Weather Sync: the face's ambient effect follows real conditions.
        // Cheap check (a steady_clock compare) every frame; real work ~1/min.
        if (native_ctrl &&
            (weather_fx_resync ||
             std::chrono::steady_clock::now() - weather_fx_last >= std::chrono::seconds(60))) {
            weather_fx_last   = std::chrono::steady_clock::now();
            weather_fx_resync = false;
            nlohmann::json spec;
            if (pf_weather_effects) {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (state.weather.ok)
                    spec = weather_effect_spec(state.weather.code, state.weather.is_day);
            }
            const std::string key = spec.dump();
            if (key != weather_fx_sent) {
                weather_fx_sent = key;
                native_ctrl->set_weather_effect(spec);
            }
        }
        if (state.win_resize_dirty.exchange(false)) {
            const int rw = state.win_resize_w.load(), rh = state.win_resize_h.load();
            if (rw > 0 && rh > 0 && xr.glfw_window())
                glfwSetWindowSize(xr.glfw_window(), rw, rh);
        }

        // ── Delta time ────────────────────────────────────────────────────────
        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - prev_time);
        prev_time  = now;

        // ── Drain marshalled input (knob / wireless / GPIO / coproc / FIFO) ──
        // Reader threads only enqueue; their menu/toast/pip actions run here,
        // on the render thread, before anything draws this frame.
        drain_input_events();

        // ── Feed IMU motion to the face (motion-reactive particle layers) ─────
        // Snapshot under the lock, then push without it (face_proxy locks its
        // own mutex). No-op unless a Protoface particle layer opted into motion.
        {
            double m_head, m_yaw, m_pitch, m_roll, m_accel;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                const auto& d = state.imu_data;
                m_head  = state.imu_bno.heading_deg;
                m_yaw   = d.bno_gyro_dps[2];          // yaw rate about vertical
                m_roll  = d.bno_euler[1];
                m_pitch = d.bno_euler[2];
                m_accel = std::sqrt(d.bno_accel_g[0]*d.bno_accel_g[0] +
                                    d.bno_accel_g[1]*d.bno_accel_g[1] +
                                    d.bno_accel_g[2]*d.bno_accel_g[2]);
            }
            face_proxy.set_motion(m_head, m_yaw, m_pitch, m_roll, m_accel);
        }

        // Effects Live Preview — re-apply the builder spec on change (no-op
        // unless the user enabled Live Preview in Face > Effects > Custom).
        if (pf_live_tick && *pf_live_tick) (*pf_live_tick)();

        // Scheduler "send link on startup": once both the daemon's URL and the
        // phone are ready, push the web link over KDE Connect exactly once.
        if (!sched_link_pushed && state.sched_send_link_startup &&
            kdc_menu_ptr && kdc_menu_ptr->device_ready()) {
            std::string url;
            { std::lock_guard<std::mutex> lk(state.mtx); url = state.scheduler_status.web_url; }
            if (!url.empty() && kdc_menu_ptr->send_ping("ProtoHUD scheduler: " + url)) {
                sched_link_pushed = true;
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App;
                n.title = "Scheduler link sent"; n.body = url; n.auto_dismiss_s = 6.f;
                state.notifs.push(std::move(n));
            }
        }

        // ── Notification log persistence (debounced, ~5s) ─────────────────────
        // Dirty on a new push (next_id) OR a delete/clear (size change).
        if (state.notif_persist && now >= notif_next_save) {
            size_t   sz;
            uint32_t next_id;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                sz      = state.notifs.items.size();
                next_id = state.notifs.next_id;
            }
            bool dirty = state.notif_dirty.exchange(false);
            if (next_id != notif_last_saved || sz != notif_last_size || dirty) {
                save_notifs();
                notif_last_saved = next_id;
                notif_last_size  = sz;
                notif_next_save  = now + 5.0;
            }
        }

        // ── CSI boot auto-retry ───────────────────────────────────────────────
        // Recover a CSI eye that came up wedged at boot. Runs on the render
        // thread (DmaCamera::init needs the GL context, which is current here).
        if (csi_retries_left > 0) {
            const int up = (cameras.owl_left_ok() ? 1 : 0) + (cameras.owl_right_ok() ? 1 : 0);
            if (up >= csi_expected) {
                csi_retries_left = 0;   // all expected eyes are up
            } else if (std::chrono::steady_clock::now() >= csi_next_retry) {
                std::cout << "[cam] CSI auto-retry (" << csi_retries_left << " left)\n";
                cameras.reinit_owls();
                --csi_retries_left;
                csi_next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
        }

        // ── Re-apply AppState-held camera settings after any CSI re-init ──────
        // reinit_owls() rebuilds the DmaCameras. Rotation + the AF/AE/WB/ISP/HDR
        // controls are restored inside it, but focus mode/position and the
        // AWB-enable toggle live in AppState, so re-apply those whenever a
        // rebuild happened (resolution change, CSI auto-retry, manual reinit).
        {
            static uint32_t last_owl_gen = cameras.reinit_generation();
            const uint32_t gen = cameras.reinit_generation();
            if (gen != last_owl_gen) {
                last_owl_gen = gen;
                auto reapply = [](DmaCamera* c, const CameraFocusState& f, bool awb_auto){
                    if (!c) return;
                    if (f.mode == CameraFocusState::Mode::AUTO) {
                        c->start_autofocus();
                    } else {                         // MANUAL / SLAVE → fixed lens
                        c->stop_autofocus();
                        c->set_focus_position(f.focus_position);
                    }
                    c->set_awb_enable(awb_auto);
                };
                reapply(cameras.owl_left(),  state.focus_left,
                        state.night_vision.csi_awb_left);
                reapply(cameras.owl_right(), state.focus_right,
                        state.night_vision.csi_awb_right);
            }
        }

        // ── Start frame: tick HUD state + begin ImGui for input/menu ─────────
        hud.set_dt(dt);
        hud.begin_menu_frame();

        // ── Profile requests (posted by the Profiles menu) ────────────────────
        // Save = write a snapshot now; Load = relaunch into that profile (restart);
        // Delete = remove the file. Strings are swapped out under the lock.
        {
            std::string save_req, load_req, del_req;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                save_req.swap(state.profile_save_name);
                load_req.swap(state.profile_load_name);
                del_req.swap(state.profile_delete_name);
            }
            if (!save_req.empty()) {
                std::string nm = ProfileManager::sanitize(save_req);
                if (save_config_to(profiles.path_for(nm))) {
                    profiles.scan();
                    Notification n; n.type = NotifType::App;
                    n.title = "Profile saved"; n.body = nm; n.auto_dismiss_s = 4.f;
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.notifs.push(std::move(n));
                }
            }
            if (!del_req.empty()) {
                bool ok = profiles.remove(del_req);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "Profile deleted" : "Delete failed";
                n.body = del_req; n.auto_dismiss_s = 4.f;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.notifs.push(std::move(n));
            }
            if (!load_req.empty() && profiles.exists(load_req)) {
                // Save the current config, mark the profile as last-loaded, then
                // request a clean restart into it (re-exec happens at shutdown).
                profiles.set_last(load_req);
                pending_reexec = profiles.path_for(load_req);
                state.quit = true;
            }
        }
        if (state.quit) break;

        // ── HUD/menu preset requests (visual-only, applied live, no restart) ──
        {
            std::string save_req, load_req, del_req;
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                save_req.swap(state.hud_preset_save_name);
                load_req.swap(state.hud_preset_load_name);
                del_req.swap(state.hud_preset_delete_name);
            }
            if (!save_req.empty()) {
                std::string nm = ProfileManager::sanitize(save_req);
                bool ok = save_hud_preset(hud_presets.path_for(nm));
                if (ok) hud_presets.scan();
                Notification n; n.type = NotifType::App;
                n.title = ok ? "Preset saved" : "Save failed";
                n.body = nm; n.auto_dismiss_s = 4.f;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.notifs.push(std::move(n));
            }
            if (!del_req.empty()) {
                bool ok = hud_presets.remove(del_req);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "Preset deleted" : "Delete failed";
                n.body = del_req; n.auto_dismiss_s = 4.f;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.notifs.push(std::move(n));
            }
            if (!load_req.empty() && hud_presets.exists(load_req)) {
                bool ok = apply_hud_preset(hud_presets.path_for(load_req));
                Notification n; n.type = NotifType::App;
                n.title = ok ? "Preset applied" : "Apply failed";
                n.body = load_req; n.auto_dismiss_s = 4.f;
                std::lock_guard<std::mutex> lk(state.mtx);
                state.notifs.push(std::move(n));
            }
        }

        // ── M key: toggle the expanded map view (tap) / cycle the map (hold) ──
        // Handled before the input dispatch so the tap/hold state machine stays
        // continuous across the open↔close transition (no reopen-on-release).
        // Skipped while typing on the on-screen keyboard. Shift+M (recenter) is
        // handled in the normal-hotkeys branch below. Also skip when the
        // face editor is on top — it owns the keyboard while open.
        if (!menu.is_keyboard_open() && !menu.is_face_editor_open()) {
            const bool m_held = ImGui::IsKeyDown(ImGuiKey_M) && !ImGui::GetIO().KeyShift;
            if (m_held && m_press_t < 0.0) {
                m_press_t    = glfwGetTime();
                m_long_fired = false;
            } else if (!m_held && m_press_t >= 0.0) {
                if (!m_long_fired) {                       // tap → toggle expanded view
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (state.map_overlay.expanded) {
                        state.map_overlay.expanded = false;
                    } else {
                        state.map_overlay.expanded   = true;
                        state.map_overlay.view_zoom  = 1.f;
                        state.map_overlay.view_pan_x = 0.f;
                        state.map_overlay.view_pan_y = 0.f;
                    }
                }
                m_press_t = -1.0;
            }
            // Hold 1.5 s (while not expanded) → cycle to the next map in the folder.
            if (m_held && !m_long_fired && m_press_t >= 0.0 &&
                    glfwGetTime() - m_press_t >= 1.5 && !state.map_overlay.expanded) {
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
                }
                m_long_fired = true;
            }
        }

        // ── Keyboard input (via ImGui, which owns GLFW callbacks) ─────────────
        // The expanded map and the on-screen keyboard each capture ALL keystrokes
        // while up (so pan/zoom or typing can't trigger app hotkeys).
        if (state.map_overlay.expanded) {
            auto& mo = state.map_overlay;
            if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  map_pan(+0.012f, 0.f);
            if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) map_pan(-0.012f, 0.f);
            if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    map_pan(0.f, +0.012f);
            if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  map_pan(0.f, -0.012f);
            if (ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd))      map_zoom(+0.04f);
            if (ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract)) map_zoom(-0.04f);
            if (key_pressed(ImGuiKey_R))   // lock/unlock map rotation to the compass
                mo.rotate_with_heading = !mo.rotate_with_heading;
            if (key_pressed(ImGuiKey_N) || key_pressed(ImGuiKey_Escape))
                mo.expanded = false;   // M is handled by the global toggle below
        } else if (menu.is_keyboard_open()) {
            if (key_pressed(ImGuiKey_UpArrow))    menu.osk_move(0, -1);
            if (key_pressed(ImGuiKey_DownArrow))  menu.osk_move(0, +1);
            if (key_pressed(ImGuiKey_LeftArrow))  menu.osk_move(-1, 0);
            if (key_pressed(ImGuiKey_RightArrow)) menu.osk_move(+1, 0);
            if (key_pressed(ImGuiKey_Enter))      menu.osk_activate();
            if (key_pressed(ImGuiKey_Backspace))  menu.osk_backspace();
            if (key_pressed(ImGuiKey_Escape) || key_pressed(ImGuiKey_F1)) menu.osk_cancel();
            for (ImWchar ch : ImGui::GetIO().InputQueueCharacters)
                menu.osk_input_char(static_cast<unsigned int>(ch));
        } else if (!menu.is_face_editor_open()) {
        // Editor lockdown: skip ALL global ImGui hotkeys (Escape/P quit,
        // Ctrl+Q/K force-kill, I menu toggle, F1 deep-menu toggle, Tab tab
        // switch, N expanded map, E/Q/W/D vision-assist, C capture) while
        // the face editor owns the screen. The editor has its own keyboard
        // handling in menu_system.cpp; the user can Back out of the editor
        // first if they need any of these.
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
        // F1 toggles the full-screen "deep menu".
        if (key_pressed(ImGuiKey_F1)) {
            if (menu.is_deep_open()) menu.close_deep();
            else { menu.close(); menu.open_deep(); }
        }
        // Per-camera autofocus: [ = Left camera, ] = Right camera.
        if (key_pressed(ImGuiKey_LeftBracket)) {
            if (cameras.owl_left()) cameras.owl_left()->start_autofocus();
            state.focus_left.mode = CameraFocusState::Mode::AUTO;
        }
        if (key_pressed(ImGuiKey_RightBracket)) {
            if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
            state.focus_right.mode = CameraFocusState::Mode::AUTO;
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
            // While editing a value, Up increases / Down decreases; otherwise Up =
            // previous item, Down = next item.
            const bool ev = menu.editing_value();
            if (rep_nav_up  .tick(ImGui::IsKeyDown(ImGuiKey_UpArrow)))   menu.navigate(ev ? +1 : -1);
            if (rep_nav_down.tick(ImGui::IsKeyDown(ImGuiKey_DownArrow))) menu.navigate(ev ? -1 : +1);
            // Left/Right arrows route to menu.back / menu.select — and those
            // forward to face_editor_.back / .primary when the editor is up,
            // which would cancel or paint on every arrow press. The editor
            // polls Left/Right directly for cursor_step, so skip the menu
            // forwarding while the editor owns the keyboard. Enter / Backspace
            // still work (paint / cancel). The color picker wants Left/Right
            // as horizontal movement (SV square / hue strip / swatch rows),
            // routed through overlay_move instead of back/select.
            const bool editor_up = menu.is_face_editor_open();
            const bool cp_up     = menu.is_color_picker_open();
            if (cp_up) {
                if (key_pressed(ImGuiKey_LeftArrow))  menu.overlay_move(-1, 0);
                if (key_pressed(ImGuiKey_RightArrow)) menu.overlay_move(+1, 0);
            }
            if (key_pressed(ImGuiKey_Enter)) {
                if (ImGui::GetIO().KeyCtrl) menu.secondary();   // Ctrl+Select → settings
                else                        menu.select();
            } else if (!editor_up && !cp_up && key_pressed(ImGuiKey_RightArrow)) {
                menu.select();
            }
            if (key_pressed(ImGuiKey_Backspace) ||
                (!editor_up && !cp_up && key_pressed(ImGuiKey_LeftArrow)))  menu.back();
            // Deep-menu tab switching (Tab / Shift+Tab).
            if (menu.is_deep_open() && key_pressed(ImGuiKey_Tab)) {
                if (ImGui::GetIO().KeyShift) menu.prev_tab(); else menu.next_tab();
            }
        }
        // N — open the expanded (Helldivers-style) map view.
        if (key_pressed(ImGuiKey_N)) {
            state.map_overlay.expanded = true;
            state.map_overlay.view_zoom = 1.0f;
            state.map_overlay.view_pan_x = state.map_overlay.view_pan_y = 0.f;
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
        // V — start/stop video recording (same flow as the assigned button's
        // double-tap). Start toggles: starts when idle, stops when recording.
        if (key_pressed(ImGuiKey_V)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.video_request = VideoRequest::Start;
        }
        // F — toggle FPS overlay
        if (key_pressed(ImGuiKey_F)) fps_overlay_active = !fps_overlay_active;
        // Y — toggle vsync (diagnostic: tells a display-refresh cap apart from
        // real render cost; watch the [perf] fps line jump when off).
        if (key_pressed(ImGuiKey_Y)) xr.set_vsync(!xr.vsync());
        // Shift+M — calibrate north (Set My Direction); edge-only
        if (ImGui::GetIO().KeyShift && key_pressed(ImGuiKey_M)) {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.map_north_deg = state.compass_heading;
            state.map_overlay.calibrated    = true;
        }
        // Space — dismiss focused toast or close menu (back)
        if (key_pressed(ImGuiKey_Space)) {
            if (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())     menu.back();
        }

        // Ctrl+1..9/0 — switch face (expression);  Alt+1..9/0 — play a GIF.
        // Number maps to a 0-based index: 1→0 … 9→8, 0→9. Routed through the
        // FaceProxy so it follows whichever backend (Protoface/Teensy) is active.
        // Gated to menu-closed so the bare number keys keep driving the camera
        // PiPs below; the camera block also ignores keys while Ctrl/Alt is held.
        if (!menu.is_open() && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyAlt)) {
            static const ImGuiKey kNumKeys[10] = {
                ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0,
            };
            for (int i = 0; i < 10; ++i) {
                if (!key_pressed(kNumKeys[i])) continue;
                if (ImGui::GetIO().KeyCtrl) face_proxy.set_face(static_cast<uint8_t>(i));
                else                        face_proxy.play_gif(static_cast<uint8_t>(i));
            }
        }
        }  // end if (!menu.is_keyboard_open())

        // ── Wireless controller pip state ─────────────────────────────────────
        bool wc_pip_left  = wireless_enabled && wireless.pip_left_active();
        bool wc_pip_right = wireless_enabled && wireless.pip_right_active();

        // ── USB camera stream lifecycle ───────────────────────────────────────
        // Rising edge  → open stream in background (window appears on first frame).
        // Falling edge → close stream, clear texture (no stale frame on re-open).
        // A camera image-setting panel (Brightness/Exposure/WB/Resolution) sets
        // usb_preview_req to its slot while visible; that forces the stream open and
        // keeps the texture uploaded (want_n), and hides the on-screen PiP for that
        // slot so the feed shows only in the context pane.  On exit the request
        // clears, restoring the PiP if it was on or closing a temp-opened stream.
        int preview = usb_preview_req;   // set during last frame's menu draw
        usb_preview_req = 0;             // re-set this frame if a panel is still up
        // Editor full-screen mode: while the face editor is open we close
        // every USB camera stream and hide the on-screen PiPs. The existing
        // open/close edge logic on want1/2/3 picks the streams back up when
        // the user closes the editor. CSI eye cameras keep running so the
        // renderer still has eye textures behind the editor overlay.
        const bool editor_open = menu.is_face_editor_open();
        bool p1 = (pip_cam1_overlay_active || pip_left_active  || kb_pip_left  || wc_pip_left)
                  && !editor_open;
        bool p2 = (pip_cam2_overlay_active || pip_right_active || kb_pip_right || wc_pip_right)
                  && !editor_open;
        bool p3 = pip_cam3_overlay_active && !editor_open;
        // Multi-cam layout needs its two chosen USB cameras open + uploading
        // even when no PiP is showing them. Fold that into the want flags so
        // the stream-lifecycle edge logic opens/closes them automatically.
        auto mc_needs = [&](EyeSource s){
            return multicam_layout && (multicam_usb_a == s || multicam_usb_b == s ||
                                       multicam_top_a == s || multicam_top_b == s);
        };
        bool want1 = (p1 || preview == 1 || mc_needs(EyeSource::USB1)) && !editor_open;
        bool want2 = (p2 || preview == 2 || mc_needs(EyeSource::USB2)) && !editor_open;
        bool want3 = (p3 || preview == 3 || mc_needs(EyeSource::USB3)) && !editor_open;
        if (want1 && !prev_p1) { tex_usb1 = 0; std::thread([&cameras]{ cameras.open_usb1(); }).detach(); }
        if (!want1 && prev_p1) { cameras.close_usb1(); tex_usb1 = 0; }
        if (want2 && !prev_p2) { tex_usb2 = 0; std::thread([&cameras]{ cameras.open_usb2(); }).detach(); }
        if (!want2 && prev_p2) { cameras.close_usb2(); tex_usb2 = 0; }
        if (want3 && !prev_p3) { tex_usb3 = 0; std::thread([&cameras]{ cameras.open_usb3(); }).detach(); }
        if (!want3 && prev_p3) { cameras.close_usb3(); tex_usb3 = 0; }
        prev_p1 = want1; prev_p2 = want2; prev_p3 = want3;

        // ── Camera texture uploads (CPU paths) ────────────────────────────────
        if (use_beast_cam) beast_cam.get_frame(tex_beast);
        if (want1) cameras.get_usb1(tex_usb1);
        if (want2) cameras.get_usb2(tex_usb2);
        if (want3) cameras.get_usb3(tex_usb3);
        android_mirror.get_frame(tex_android);

        // PiP toggle state (pip_left_active / pip_right_active) is now flipped
        // directly by the GPIO dispatch (Camera: PiP toggle), so there's no
        // per-frame button-hold polling here anymore.

        // ── Gamepad poll ──────────────────────────────────────────────────────
        gamepad.poll();

        // ── Keyboard button emulation (direct GLFW polling, edge-detected) ──
        // 1/2/3     = toggle USB cam PiP 1/2/3   Shift+1/2/3 = autofocus that cam
        // 0         = toggle manual/auto focus    4 = autofocus both cameras
        // - / =     = focus near / far (step 20 of 0-1000)
        // (Ctrl/Alt + number is reserved for face/GIF hotkeys handled above.)
        // Skipped while the face editor owns the keyboard (1-6 → tool select,
        // - / = → brush size); we still run edge() to keep prev_key fresh so
        // releases after the editor closes don't fire stale events.
        {
            GLFWwindow* win = static_cast<GLFWwindow*>(xr.glfw_window());
            const bool editor_owns_kb = menu.is_face_editor_open();
            auto edge = [&](int n, int glfw_key) -> bool {
                bool now = (glfwGetKey(win, glfw_key) == GLFW_PRESS);
                bool fired = now && !prev_key[n] && !editor_owns_kb;
                prev_key[n] = now;
                return fired;
            };
            // Ctrl/Alt + number drives the face/GIF hotkeys above, so skip the
            // camera number handlers while either is held (edge() still runs to
            // keep prev_key state current and avoid a stale release-edge).
            const bool face_mod =
                (glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                 glfwGetKey(win, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
                 glfwGetKey(win, GLFW_KEY_LEFT_ALT)  == GLFW_PRESS ||
                 glfwGetKey(win, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
            // ── Manual per-camera focus ───────────────────────────────────────
            //   9 / 0  → LEFT camera  (near / far)
            //   - / =  → RIGHT camera (near / far)
            // Nudging puts that camera into MANUAL focus so the position sticks.
            constexpr int FOCUS_STEP = 20;
            auto nudge_focus = [&](DmaCamera* c, CameraFocusState& f, int delta){
                if (!c) return;
                int pos = std::clamp(c->get_focus_position() + delta, 0, 1000);
                c->stop_autofocus();
                c->set_focus_position(pos);
                std::lock_guard<std::mutex> lk(state.mtx);
                f.mode = CameraFocusState::Mode::MANUAL;
                f.focus_position = pos;
            };
            if (edge(1, GLFW_KEY_9)     && !menu.is_open() && !face_mod)
                nudge_focus(cameras.owl_left(),  state.focus_left,  -FOCUS_STEP);
            if (edge(0, GLFW_KEY_0)     && !menu.is_open() && !face_mod)
                nudge_focus(cameras.owl_left(),  state.focus_left,  +FOCUS_STEP);
            if (edge(5, GLFW_KEY_MINUS) && !menu.is_open() && !face_mod)
                nudge_focus(cameras.owl_right(), state.focus_right, -FOCUS_STEP);
            if (edge(6, GLFW_KEY_EQUAL) && !menu.is_open() && !face_mod)
                nudge_focus(cameras.owl_right(), state.focus_right, +FOCUS_STEP);
            // 4: autofocus both cameras
            if (edge(4, GLFW_KEY_4) && !face_mod) {
                if (cameras.owl_left())  cameras.owl_left()->start_autofocus();
                if (cameras.owl_right()) cameras.owl_right()->start_autofocus();
                std::lock_guard<std::mutex> lk(state.mtx);
                state.focus_left.mode  = CameraFocusState::Mode::AUTO;
                state.focus_right.mode = CameraFocusState::Mode::AUTO;
            }
            // 1/2/3 — toggle USB cam PiP;  Shift+1/2/3 — trigger autofocus on that cam
            {
                bool shift = (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                if (!menu.is_open()) {
                    if (edge(7, GLFW_KEY_1) && !face_mod) {
                        if (shift) cameras.set_usb1_ctrl(V4L2_CID_FOCUS_AUTO, 1);
                        else       pip_cam1_overlay_active = !pip_cam1_overlay_active;
                    }
                    if (edge(8, GLFW_KEY_2) && !face_mod) {
                        if (shift) cameras.set_usb2_ctrl(V4L2_CID_FOCUS_AUTO, 1);
                        else       pip_cam2_overlay_active = !pip_cam2_overlay_active;
                    }
                    if (edge(9, GLFW_KEY_3) && !face_mod) {
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

        // ── Scheduler reminders ───────────────────────────────────────────────
        // Raise a lead-time reminder and an at-start reminder for each event.
        // Locked because SchedulerMonitor mutates scheduler_events concurrently.
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            const time_t now_t  = time(nullptr);
            const time_t lead_s = static_cast<time_t>(state.scheduler_lead_min) * 60;
            auto local_hm = [](time_t t) {
                struct tm tm{}; localtime_r(&t, &tm);
                char b[8]; strftime(b, sizeof(b), "%H:%M", &tm);
                return std::string(b);
            };
            auto body_for = [&](const ScheduledEvent& ev) {
                std::string b = ev.all_day ? "All day" : local_hm(ev.start_utc);
                if (!ev.location.empty()) b += "  @ " + ev.location;
                return b;
            };
            for (auto& e : state.scheduler_events) {
                if (e.start_utc == 0) continue;

                // Lead reminder (skipped for all-day; they only fire the at-start path).
                const time_t lead_at = e.start_utc - lead_s;
                const bool snoozed_due = e.snooze_until > 0 && now_t >= e.snooze_until
                                         && now_t < e.start_utc;
                if (!e.all_day &&
                    ((!e.fired_lead && now_t >= lead_at && now_t < e.start_utc) || snoozed_due)) {
                    e.fired_lead   = true;
                    e.snooze_until = 0;
                    long mins = static_cast<long>((e.start_utc - now_t + 59) / 60);
                    if (mins < 0) mins = 0;
                    Notification n;
                    n.type           = NotifType::App;
                    n.title          = "In " + std::to_string(mins) + " min: " + e.title;
                    n.body           = body_for(e);
                    n.timestamp      = static_cast<int64_t>(now_t);
                    n.auto_dismiss_s = 0.f;
                    const std::string uid = e.uid;
                    n.actions.push_back({"DISMISS", [](AppState&){}});
                    n.actions.push_back({"SNOOZE 5", [uid](AppState& s){
                        std::lock_guard<std::mutex> g(s.mtx);
                        for (auto& ev : s.scheduler_events)
                            if (ev.uid == uid) { ev.snooze_until = time(nullptr) + 300; break; }
                    }});
                    state.notifs.push(std::move(n));
                }

                // At-start reminder.
                if (!e.fired_start && now_t >= e.start_utc) {
                    e.fired_start = true;
                    Notification n;
                    n.type           = NotifType::App;
                    n.title          = (e.all_day ? "Today: " : "Now: ") + e.title;
                    n.body           = body_for(e);
                    n.timestamp      = static_cast<int64_t>(now_t);
                    n.auto_dismiss_s = 0.f;
                    n.actions.push_back({"DISMISS", [](AppState&){}});
                    state.notifs.push(std::move(n));
                }
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

        // ── Boot autofocus → slave ─────────────────────────────────────────────
        // Run one AF cycle on the OWLsight (CSI) cameras at startup, then settle
        // both into SLAVE (hold the locked focus). Falls through after a timeout if
        // the camera never reports a lock.
        if (boot_af_pending) {
            const bool l_done = !cameras.owl_left()  || cameras.owl_left()->is_af_locked();
            const bool r_done = !cameras.owl_right() || cameras.owl_right()->is_af_locked();
            if ((l_done && r_done) || (glfwGetTime() - boot_af_t0) > 5.0) {
                if (cameras.owl_left())  cameras.owl_left()->stop_autofocus();
                if (cameras.owl_right()) cameras.owl_right()->stop_autofocus();
                state.focus_left.mode  = CameraFocusState::Mode::SLAVE;
                state.focus_right.mode = CameraFocusState::Mode::SLAVE;
                boot_af_pending = false;
                std::cout << "[cam] boot autofocus complete → slave focus mode\n";
            }
        }

        // ── Dock positioning ──────────────────────────────────────────────────
        // Place the minimap (right) and info panel (left) so the minimap's compass
        // cardinals sit flush against the screen edges. Done here, per frame, since
        // it depends on the live display size + minimap radius (which the fractional
        // anchors can't know). Both are render-thread-owned fields.
        {
            const float dw = static_cast<float>(xr.display_width());
            const float dh = static_cast<float>(xr.display_height());
            if (dw > 1.f && dh > 1.f) {
                const float ringR = state.map_overlay.size_px;
                // Outward chrome radius: cardinal letters sit at ringR+26 (+glyph).
                const float cr = ringR + (state.map_overlay.compass_ring ? 34.f : 10.f);
                const float voff = state.hud_dock.v_offset;
                const float cyv = (state.hud_dock.bottom ? (dh - cr) : cr) + voff;
                state.map_overlay.anchor_x = (dw - cr) / dw;   // minimap: right, flush
                state.map_overlay.anchor_y = cyv / dh;
                state.map_overlay.pan_x = 0.f; state.map_overlay.pan_y = 0.f;
                state.info_panel.anchor_x = cr / dw;           // info panel: left, mirrored
                state.info_panel.anchor_y = cyv / dh;
                state.info_panel.pan_x = 0.f; state.info_panel.pan_y = 0.f;
            }
        }

        // ── State snapshot ────────────────────────────────────────────────────
        // Also update render-thread metrics (frame time, knob event age) here
        // so they're included in the snapshot under a single lock acquisition.
        // (snap itself is declared once above the loop — see comment there.)
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
            snap.compass_tape       = state.compass_tape;
            snap.legacy_hud         = state.legacy_hud;
            snap.imu_pose           = state.imu_pose;
            snap.focus_left         = state.focus_left;
            snap.focus_right        = state.focus_right;
            snap.night_vision       = state.night_vision;
            snap.clock_cfg          = state.clock_cfg;
            snap.pp_cfg             = state.pp_cfg;
            snap.timer_alarm        = state.timer_alarm;
            snap.scheduler_events   = state.scheduler_events;
            snap.scheduler_status   = state.scheduler_status;
            snap.scheduler_lead_min = state.scheduler_lead_min;
            snap.effects_cfg        = state.effects_cfg;
            snap.map_overlay        = state.map_overlay;
            snap.info_panel         = state.info_panel;
            snap.expanded_show_debug = state.expanded_show_debug;
            snap.expanded_hide_info  = state.expanded_hide_info;
            snap.weather            = state.weather;
            snap.weather_cfg        = state.weather_cfg;
            snap.sys_metrics        = state.sys_metrics;
            snap.gpu                = state.gpu;
            snap.imu_data           = state.imu_data;
            snap.wifi               = state.wifi;
            snap.ping               = state.ping;
            snap.ssh                = state.ssh;
            snap.bt_devices         = state.bt_devices;
            snap.serial_metrics     = state.serial_metrics;
            snap.camera_resolution  = state.camera_resolution;
            snap.camera_resolution_right = state.camera_resolution_right;
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

        // ── Perf readout (TEMPORARY) — once/second fps so the post-process win
        // can be confirmed after toggling Vision Assist off. (F also shows fps in
        // the HUD.)  Lightweight: no glFinish, no per-phase timing.
        {
            static double s_diag_last = 0.0;
            static int    s_diag_frames = 0;
            static double s_diag_ft_sum = 0.0;
            ++s_diag_frames;
            s_diag_ft_sum += dt;
            if (now - s_diag_last >= 1.0) {
                double avg_ms = (s_diag_frames > 0)
                              ? (s_diag_ft_sum / s_diag_frames) * 1000.0 : 0.0;
                fprintf(stderr, "[perf] fps=%.1f ft=%.1fms\n",
                        avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0, avg_ms);
                s_diag_last   = now;
                s_diag_frames = 0;
                s_diag_ft_sum = 0.0;
            }
        }

        // If XR glasses IMU is fresh, recompute compass heading from the selected
        // axis on the render thread.  axis/invert are only read and written on the
        // render thread so no mutex is needed — avoids the data race that made the
        // menu setting invisible to the SDK IMU callback thread.
        {
            const int64_t now_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            // xr_active flag stays useful for the debug UI even when the
            // user pinned the source to MPU9250 / BNO055.
            snap.imu_data.xr_active =
                (now_us - last_xr_imu_us.load()) < 2'000'000LL;
            // pick_imu_heading() walks state.imu_source + the per-source
            // freshness slots and returns the heading the HUD compass
            // should use this frame. Replaces the old "Viture always wins,
            // MPU fills in when stale" hardcoded path.
            snap.compass_heading = pick_imu_heading(state, now_us);
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
                float alpha = 1.f - std::exp(-dt / kTau);
                constexpr float kD2R = 3.14159265f / 180.f;
                float fs = std::sin(s_smooth * kD2R) + alpha * (std::sin(raw * kD2R) - std::sin(s_smooth * kD2R));
                float fc = std::cos(s_smooth * kD2R) + alpha * (std::cos(raw * kD2R) - std::cos(s_smooth * kD2R));
                s_smooth = std::atan2(fs, fc) / kD2R;
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
        // Battery level for the minimap arc: prefer the dedicated wireless
        // controller, else fall back to the SDL gamepad's coarse level.
        {
            int bpct = wireless_enabled ? wireless.battery_pct() : -1;
            if (bpct < 0) bpct = gamepad.battery_pct();
            snap.health.wireless_battery_pct = bpct;
        }

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
        auto make_theater_vp = [&snap](int fw, int fh, bool right_eye) -> std::array<int,4> {
            using TA = AppState::TheaterAnchor;
            // Each eye preserves its OWN camera aspect ratio (the eyes can run
            // different resolutions now).
            const auto& cam_res = right_eye ? snap.camera_resolution_right
                                            : snap.camera_resolution;
            float cam_ar  = (float)cam_res.width / cam_res.height;
            float disp_ar = (float)fw / fh;
            int vp_w, vp_h, vp_x, vp_y;
            if (cam_ar < disp_ar) {   // pillarbox: black left/right
                vp_h = fh; vp_w = (int)(fh * cam_ar); vp_y = 0;
                switch (snap.theater_anchor) {
                    case TA::Center:
                        vp_x = right_eye ? 0 : fw - vp_w;
                        break;
                    case TA::Outside:
                        vp_x = right_eye ? fw - vp_w : 0;
                        break;
                    case TA::Left:
                        vp_x = fw - vp_w;
                        break;
                    case TA::Right:
                        vp_x = 0;
                        break;
                    default:
                        vp_x = (fw - vp_w) / 2;
                        break;
                }
            } else {                  // letterbox: black top/bottom
                vp_w = fw; vp_h = (int)(fw / cam_ar); vp_x = 0;
                switch (snap.theater_anchor) {
                    case TA::Top:    vp_y = fh - vp_h;        break;
                    case TA::Bottom: vp_y = 0;                 break;
                    default:         vp_y = (fh - vp_h) / 2;  break;
                }
            }
            return { vp_x, vp_y, vp_w, vp_h };
        };

        // Returns the USB RGBA texture for the given eye source slot.
        auto usb_tex_for = [&](EyeSource src) -> GLuint {
            if (src == EyeSource::USB1) return tex_usb1;
            if (src == EyeSource::USB2) return tex_usb2;
            if (src == EyeSource::USB3) return tex_usb3;
            return 0;
        };

        // Multi-cam composite. The TOP HALF is a single full-width camera that
        // DIFFERS per eye (top_src), so in side-by-side the top reads as two
        // images — one per eye — instead of a 2x2 split mirrored into both eyes
        // (which showed 4 images with 2 duplicates). The BOTTOM half stays as the
        // two selected USB cams side by side. Sub-viewports inside the bound eye
        // FBO mean each feed's existing draw call fills only its region; the CSI
        // rotation from Cameras > Left/Right Camera > Rotation still applies.
        auto draw_multicam_into_current_fbo = [&](int fw, int fh,
                                                  EyeSource top_src, EyeSource bot_src) {
            const int hh  = fh / 2;             // top half height
            const int top = fh - hh;            // bottom half height; top starts at y=top

            // Draw the chosen source into the current sub-viewport. Left/Right
            // CSI are explicit (no global swap applied — the picker is explicit);
            // USB sources blit their texture.
            auto draw_src = [&](EyeSource s){
                switch (s) {
                    case EyeSource::CSI_LEFT:  cameras.draw_owl_left();  break;
                    case EyeSource::CSI_RIGHT: cameras.draw_owl_right(); break;
                    case EyeSource::CSI:       cameras.draw_owl_left();  break;
                    default: cameras.draw_tex_fullscreen(usb_tex_for(s)); break;
                }
            };

            glViewport(0, top, fw, hh);  draw_src(top_src);   // top half (per eye)
            glViewport(0, 0,   fw, top); draw_src(bot_src);   // bottom half (per eye)

            // Restore full viewport so any later passes (post-process /
            // HUD overlays into this FBO) see the whole eye region.
            glViewport(0, 0, fw, fh);
        };

        // Left eye
        {
            xr.eye_left().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (multicam_layout) {
                // Quad layout overrides the per-eye source pickers and
                // theater mode — it owns the whole eye FBO.
                draw_multicam_into_current_fbo(xr.eye_left().w, xr.eye_left().h,
                                               multicam_top_a, multicam_usb_a);
            } else {
                if (snap.theater_mode) {
                    auto vp = make_theater_vp(xr.eye_left().w, xr.eye_left().h, false);
                    glViewport(vp[0], vp[1], vp[2], vp[3]);
                }

                bool drew = false;
                if (use_beast_cam && tex_beast != 0) {
                    // Beast passthrough — TODO: fullscreen blit once Beast path is ported
                    drew = false;
                }
                if (!drew) {
                    if (left_eye_src == EyeSource::CSI)
                        drew = snap.cameras_swapped
                            ? cameras.draw_owl_right(snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y)
                            : cameras.draw_owl_left (snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y);
                    else if (left_eye_src == EyeSource::CSI_LEFT)   // explicit Cam 0
                        drew = cameras.draw_owl_left (snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y);
                    else if (left_eye_src == EyeSource::CSI_RIGHT)  // explicit Cam 1
                        drew = cameras.draw_owl_right(snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y);
                    else
                        drew = cameras.draw_tex_fullscreen(usb_tex_for(left_eye_src));
                }

                if (snap.theater_mode)
                    glViewport(0, 0, xr.eye_left().w, xr.eye_left().h);
            }

            xr.eye_left().unbind();
        }

        // Right eye
        {
            xr.eye_right().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (multicam_layout) {
                draw_multicam_into_current_fbo(xr.eye_right().w, xr.eye_right().h,
                                               multicam_top_b, multicam_usb_b);
            } else {
                if (snap.theater_mode) {
                    auto vp = make_theater_vp(xr.eye_right().w, xr.eye_right().h, true);
                    glViewport(vp[0], vp[1], vp[2], vp[3]);
                }

                bool drew = false;
                if (use_beast_cam && tex_beast != 0) {
                    drew = false;
                }
                if (!drew) {
                    if (right_eye_src == EyeSource::CSI)
                        drew = snap.cameras_swapped
                            ? cameras.draw_owl_left (snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y)
                            : cameras.draw_owl_right(snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y);
                    else if (right_eye_src == EyeSource::CSI_LEFT)   // explicit Cam 0
                        drew = cameras.draw_owl_left (snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y);
                    else if (right_eye_src == EyeSource::CSI_RIGHT)  // explicit Cam 1
                        drew = cameras.draw_owl_right(snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y);
                    else
                        drew = cameras.draw_tex_fullscreen(usb_tex_for(right_eye_src));
                }

                if (snap.theater_mode)
                    glViewport(0, 0, xr.eye_right().w, xr.eye_right().h);
            }

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
        if (snap.capture_request != CaptureRequest::None) {
            do_capture(snap.capture_request, xr, cfg_photo_dir, state);  // resets request
            // Burst: re-arm another stereo shot next frame until the count runs out.
            std::lock_guard<std::mutex> lk(state.mtx);
            if (state.capture_burst > 0) {
                --state.capture_burst;
                state.capture_request = CaptureRequest::Stereo;
            }
        }

        // ── Full-resolution still capture (reinit to max → settle → grab) ──────
        // Briefly switches that camera to its largest sensor mode, lets it settle
        // a few frames, grabs a CPU NV12 frame as a JPEG, then restores the live
        // resolution. Both eyes blank during the re-inits (a few seconds) — it's
        // a deliberate "take a photo" action.
        {
            static int  fr_state  = 0;     // 0 idle, 1 settling
            static int  fr_eye    = 0;     // 1 left, 2 right
            static int  fr_settle = 0;
            static CameraResolutionState fr_saved;
            int req = 0;
            { std::lock_guard<std::mutex> lk(state.mtx);
              req = state.fullres_capture_req; state.fullres_capture_req = 0; }

            if (fr_state == 0 && req != 0) {
                DmaCamera* c = (req == 2) ? cameras.owl_right() : cameras.owl_left();
                if (c && !c->supported_modes().empty()) {
                    fr_eye          = req;
                    fr_saved.width  = c->width();
                    fr_saved.height = c->height();
                    fr_saved.fps    = c->fps();
                    const auto& mx  = c->supported_modes().front();   // largest (sorted)
                    const int   fps = mx.max_fps > 0 ? mx.max_fps : c->fps();
                    { Notification n; n.type = NotifType::App;
                      n.title = "Capturing full-res photo\xE2\x80\xA6";
                      n.body  = std::to_string(mx.width) + "\xC3\x97" + std::to_string(mx.height);
                      n.auto_dismiss_s = 5.f;
                      std::lock_guard<std::mutex> lk(state.mtx); state.notifs.push(std::move(n)); }
                    if (req == 2) cameras.set_owl_right_resolution(mx.width, mx.height, fps);
                    else          cameras.set_owl_left_resolution (mx.width, mx.height, fps);
                    fr_settle = 8;     // let AE/AWB settle on the new mode
                    fr_state  = 1;
                }
            } else if (fr_state == 1) {
                if (--fr_settle <= 0) {
                    DmaCamera* c = (fr_eye == 2) ? cameras.owl_right() : cameras.owl_left();
                    std::filesystem::create_directories(cfg_photo_dir);
                    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::string path = cfg_photo_dir + "/protohud_fullres_" +
                        std::string(fr_eye == 2 ? "right" : "left") + "_" +
                        std::to_string(epoch) + ".jpg";
                    const bool ok = c && c->grab_still(path);
                    // Restore the live resolution.
                    if (fr_eye == 2) cameras.set_owl_right_resolution(fr_saved.width, fr_saved.height, fr_saved.fps);
                    else             cameras.set_owl_left_resolution (fr_saved.width, fr_saved.height, fr_saved.fps);
                    { Notification n; n.type = NotifType::App;
                      n.title = ok ? "Full-res photo saved" : "Full-res capture failed";
                      n.body  = ok ? path : std::string("see log");
                      n.auto_dismiss_s = 6.f;
                      std::lock_guard<std::mutex> lk(state.mtx); state.notifs.push(std::move(n)); }
                    fr_state = 0;
                }
            }
        }

        // ── Video recording ───────────────────────────────────────────────────
        // Same clean eye-FBO source as photos; encodes on a worker thread.
        video_recorder.tick(xr, state, cfg_video);

        // ── QR scan — main cameras ────────────────────────────────────────────
        // Periodic readback from the left eye FBO (rate-limited to 2 Hz by the
        // timer below; ZBar's own interval also guards the worker thread).
        // Async PBO read — the old sync glReadPixels drained the GPU pipeline
        // every 500ms, a visible judder spike. The decoder gets the previous
        // tick's frame, which QR scanning doesn't mind.
        cameras.enable_qr_usb(snap.qr_scan_usb);
        if (snap.qr_scan_main) {
            static auto s_last_qr = std::chrono::steady_clock::now();
            static gl::AsyncFboReader  s_qr_reader;
            static std::vector<uint8_t> s_qr_px;
            auto now_qr = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_qr - s_last_qr).count() >= 500) {
                s_last_qr = now_qr;
                int qw = 0, qh = 0;
                if (s_qr_reader.read(xr.eye_left(), s_qr_px, qw, qh))
                    qr_scanner.submit_rgba(s_qr_px.data(), qw, qh);
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
        hud.begin_nvg_overlay(xr.display_width(), xr.display_height());
        // Editor full-screen mode: skip every HUD chrome draw (PiP underlays,
        // info panel + compass + minimap + clock chrome, toast banners) so
        // the editor sits cleanly on top of the eye texture with no UI
        // clutter. Toasts re-appear on close because they're a queue, not a
        // timed overlay.
        if (!editor_open) {
            // Hide the on-screen PiP for a slot whose live-preview panel is up — its
            // feed is shown in the context pane instead (preview set above this frame).
            hud.draw_pip_underlays(tex_usb1, p1 && preview != 1, pip_overlay_cfg1,
                                   tex_usb2, p2 && preview != 2, pip_overlay_cfg2,
                                   tex_usb3, p3 && preview != 3, pip_overlay_cfg3,
                                   xr.display_width(), xr.display_height());
            hud.draw_hud_frame(snap, xr.display_width(), xr.display_height(), fps_overlay_active);
            {
                // Toasts draw from (and mark read/dismissed on) the LIVE queue
                // while worker threads push into it — hold the lock for the
                // few visible cards so the deque can't reallocate mid-draw.
                std::lock_guard<std::mutex> lk(state.mtx);
                hud.draw_toasts(state.notifs, xr.display_width(), xr.display_height());
            }
        }
        hud.end_nvg_overlay();

        // ── Phase 2: ImGui overlays (menu, popups) ────────────────────────
        menu.set_glow_enabled(hud.config().glow_enabled);
        // Eye-local ImGui overlays are duplicated into both SBS eyes at flush
        // time (render_menu_overlay). The radial wheel is positioned in display
        // coords, so it opts out and stays a single instance.
        bool menu_dup = false;
        if (menu.is_deep_open() || menu.is_keyboard_open()
            || menu.is_color_picker_open()) {
            // Keyboard / color picker take over full-screen, so this also
            // covers text entry and color editing opened from the corner /
            // radial quick menu.
            menu.draw_fullscreen(xr.eye_width(), xr.eye_height());
            menu_dup = true;
        } else if (menu.is_open() && menu.quick_style() == QuickStyle::Radial
                   && !menu.is_keyboard_open()) {
            // Radial quick menu encircling the round minimap. Geometry matches
            // draw_map_overlay() (display coords) so the wheel locks onto it even
            // when the map image is off.
            const auto& mo = snap.map_overlay;
            const float dispW = static_cast<float>(xr.display_width());
            const float dispH = static_cast<float>(xr.display_height());
            const float half  = std::max(8.f, static_cast<float>(mo.size_px));
            const float mcx = std::clamp(dispW * mo.anchor_x + mo.pan_x, half, dispW - half);
            const float mcy = std::clamp(dispH * mo.anchor_y + mo.pan_y, half, dispH - half);
            // Point the selected item toward the centre of the minimap's eye-region
            // (SBS: left/right half) so it reads "into" the view.
            const bool sbs = xr.eye_width() < xr.display_width();
            const float region_cx = !sbs ? dispW * 0.5f
                                  : (mcx < dispW * 0.5f ? dispW * 0.25f : dispW * 0.75f);
            const float region_cy = dispH * 0.5f;
            const float focus = std::atan2(region_cy - mcy, region_cx - mcx);
            const bool rotate = (mo.anchor_x < 0.35f || mo.anchor_x > 0.65f ||
                                 mo.anchor_y < 0.35f || mo.anchor_y > 0.65f);
            // Top-docked (minimap in the upper screen half) → flip wedge labels
            // as one run so they read right-way-up; matches the info module.
            const bool radial_top = mcy < dispH * 0.5f;
            menu.draw_radial(mcx, mcy, half, focus, rotate, radial_top);
        } else {
            menu.draw(xr.eye_width(), xr.eye_height());
            menu_dup = true;
        }

        hud.draw_android_overlay(tex_android,
                                  xr.eye_width(), xr.eye_height(),
                                  android_overlay_active,
                                  android_mirror.is_running() && !android_mirror.is_connected(),
                                  android_overlay_cfg,
                                  android_mirror.frame_aspect());

        // Protoface LED preview (top-right corner, above popups). Source
        // depends on backend: HUB75 + daemon mode go through the shm reader
        // (panel_driver.py writes frames into /dev/shm); native MAX7219 /
        // RGB-matrix backends never touch shm, so we copy from the in-process
        // controller and upload here directly. Lambda is reused below for the
        // face portrait beside the minimap so both surfaces stay in sync.
        struct FaceTex { GLuint id = 0; int w = 0; int h = 0; bool native = false; };
        auto pick_face_tex = [&]() -> FaceTex {
            const bool native = (pf_backend == "max7219" || pf_backend == "rgb_matrix")
                                 && native_ctrl;
            if (!native) {
                FaceTex out;
                protoface_ctrl.get_frame_texture(out.id);
                out.w = ShmFrameReader::W;
                out.h = ShmFrameReader::H;
                return out;
            }
            static GLuint native_tex = 0;
            static int    native_w   = 0;
            static int    native_h   = 0;
            cv::Mat rgb;
            if (native_ctrl->latest_frame(rgb) && !rgb.empty()) {
                // Crop the renderer canvas to the actual face area so the
                // preview shows a centred face instead of dead space.
                // The canvas was sized to 2*mirror_x at backend startup,
                // so the same formula here yields a no-op crop when nothing
                // has changed and a tight crop right after a layout edit.
                const int face_w = std::min(rgb.cols,
                    std::max(8, pf_canvas_w_for_layout(pf_eye_layout,
                                                       pf_mouth_layout,
                                                       pf_nose_layout)));
                cv::Mat face_rgb = rgb(cv::Rect(0, 0, face_w, rgb.rows));
                // CV_8UC3 RGB → RGBA upload; GL_NEAREST keeps pixel-art
                // crisp at any zoom — same trade-off the shm path makes.
                cv::Mat rgba;
                cv::cvtColor(face_rgb, rgba, cv::COLOR_RGB2RGBA);
                if (native_tex == 0) {
                    glGenTextures(1, &native_tex);
                    glBindTexture(GL_TEXTURE_2D, native_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                } else {
                    glBindTexture(GL_TEXTURE_2D, native_tex);
                }
                if (rgba.cols != native_w || rgba.rows != native_h) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 rgba.cols, rgba.rows, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data);
                    native_w = rgba.cols;
                    native_h = rgba.rows;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                    rgba.cols, rgba.rows,
                                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            return FaceTex{native_tex, native_w, native_h, true};
        };

        // Fetch the face texture once per frame — preview + portrait share it.
        // (pick_face_tex copies the latest frame, converts, and re-uploads the
        // texture every call; doing it twice doubled that work when both
        // surfaces were on.)
        const bool want_preview  = panel_preview_enabled && !editor_open;
        const bool want_portrait = snap.map_overlay.portrait && !menu.is_open();
        FaceTex ft;
        if (want_preview || want_portrait) ft = pick_face_tex();

        if (want_preview) {
            // On native backends the texture already IS the centred face
            // (canvas-mirroring is HUB75-only), so left/right/full views
            // would all show the same content — force "full" so the user
            // doesn't get a duplicate or half-face.
            const int view = ft.native ? 0 : protoface_preview_view;
            hud.draw_panel_preview(ft.id, ft.w, ft.h, xr.display_width(), xr.display_height(),
                                   protoface_preview_cfg.anchor_x, protoface_preview_cfg.anchor_y,
                                   protoface_preview_cfg.pan_x,    protoface_preview_cfg.pan_y,
                                   protoface_preview_cfg.size,     view);
        }

        // Protoface portrait beside the minimap (closed-menu HUD element).
        if (want_portrait) {
            hud.draw_face_portrait(ft.id, ft.w, ft.h, ft.native,
                                   xr.display_width(), xr.display_height(), snap);
        }

        // System status panel (CPU/RAM/WiFi/ping/BT/SSH/perf/serial).
        // Debug panel: normal toggle, plus an option to show it in the expanded-map
        // view. When expanded, it opens to the right of the info sidebar (~310px).
        if (!editor_open) {
            const bool expanded_view = snap.map_overlay.expanded;
            const bool show_dbg = sys_panel_active ||
                                  (expanded_view && snap.expanded_show_debug);
            const bool dbg_in_expanded = expanded_view && show_dbg;
            const float dbg_x = dbg_in_expanded ? 310.f : 0.f;
            hud.draw_sys_panel(snap, xr.eye_width(), xr.eye_height(), show_dbg,
                               dbg_x, /*narrow=*/dbg_in_expanded);
        }

        // Alarm / timer-expired popups — disabled, toasts handle these now.
        // hud.draw_popups(state, xr.eye_width(), xr.eye_height());

        hud.render_menu_overlay(xr.eye_width(), xr.display_width(), menu_dup);

        // ── Swap ──────────────────────────────────────────────────────────────
        xr.present();
    }

    // Final notification-log flush so the last messages survive a clean exit.
    if (state.notif_persist) save_notifs(/*async=*/false);

    // ── Persist runtime settings ──────────────────────────────────────────────
    // Write current settings back to config.json so they survive a restart. Skip
    // the write if the file failed to parse on load (no-clobber). Uses the same
    // snapshot writer as the "save profile" feature (mutate_cfg + save_config_to).
    if (cfg_parse_failed) {
        std::cerr << "[cfg] " << cfg_path << " failed to parse on load — "
                     "leaving it untouched so your edits aren't lost\n";
    } else {
        save_config_to(cfg_path);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────

    // Guarantee the process exits even if a teardown step hangs (a thread stuck
    // in a blocking driver/syscall that .stop()/.join() then waits on forever).
    // Closing via P/Esc/menu sets state.quit directly with no signal, so unlike
    // SIGINT/SIGTERM there is otherwise no force-exit and a stuck step freezes
    // the program.  Exit 0 so the respawn watchdog treats it as a clean stop.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        std::cerr << "[shutdown] cleanup exceeded 8s — forcing clean exit\n";
        std::_Exit(0);
    }).detach();

    auto step = [](const char* name) { std::cerr << "[shutdown] " << name << "\n"; };

    // Stop watchdog before cleanup so it doesn't fire during intentional shutdown.
    step("watchdog");        wd_stop.store(true); watchdog.join();
    step("video_recorder");  video_recorder.stop();
    step("bt_mon");          bt_mon.stop();
    step("ping_mon");        ping_mon.stop();
    step("wifi_mon");        wifi_mon.stop();
    step("sched_mon");       sched_mon.stop();
    step("weather_mon");     weather_mon.stop();
    step("sys_mon");         sys_mon.stop();
    step("mpu9250");         mpu9250.stop();
    step("bno055");          bno055.stop();
    step("boop_sensor");     boop_sensor.stop();
    step("light_sensor");    light_sensor.stop();
    step("accessory_leds");  accessory_leds.stop();
    step("audio");           audio.stop();
    step("android_mirror");  android_mirror.stop();
    step("hud");             hud.unload();
    step("beast_cam");       beast_cam.stop();
    step("cameras");         cameras.shutdown();
    step("teensy");          teensy.stop();
    step("native_face");
    if (native_ctrl) {
        if (pf_launch_driver) std::system("pkill -f panel_driver.py 2>/dev/null");  // blanks panels via its SIGTERM handler
        native_ctrl->stop();                                                        // blanks the shm too
    }
    step("protoface");       protoface_ctrl.shutdown_daemon(); protoface_ctrl.stop();
    step("lora");            lora.stop();
    step("knob");            knob.stop();
    // Stop the wireless reader before scope unwind — its callbacks post into
    // the input event queue, which is declared later and so dies first.
    step("wireless");        if (wireless_enabled) wireless.stop();
    step("textures");
    if (tex_usb1)    glDeleteTextures(1, &tex_usb1);
    if (tex_usb2)    glDeleteTextures(1, &tex_usb2);
    if (tex_usb3)    glDeleteTextures(1, &tex_usb3);
    if (tex_beast)   glDeleteTextures(1, &tex_beast);
    if (tex_android) glDeleteTextures(1, &tex_android);

    step("post_process");
    pp_fbo_left.destroy();
    pp_fbo_right.destroy();
    pp_prev_left[0].destroy();  pp_prev_left[1].destroy();
    pp_prev_right[0].destroy(); pp_prev_right[1].destroy();
    post_proc.shutdown();

    step("xr");              xr.shutdown();
    step("done");

    // ── Profile restart ───────────────────────────────────────────────────────
    // The user picked/loaded a profile (or the landing countdown auto-resumed the
    // last one): relaunch ProtoHUD with that profile file as its config path. The
    // re-launched instance reads + writes that file, so the active profile auto-
    // saves; launched-with-a-path means it skips the landing page (no re-exec loop).
    if (!pending_reexec.empty()) {
        std::cerr << "[profile] restarting into " << pending_reexec << "\n";
        char* newargv[] = { const_cast<char*>(exe_path.c_str()),
                            const_cast<char*>(pending_reexec.c_str()),
                            nullptr };
        execv(exe_path.c_str(), newargv);
        std::cerr << "[profile] re-exec failed: " << std::strerror(errno) << "\n";
    }
    return 0;
}
