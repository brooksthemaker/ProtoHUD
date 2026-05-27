#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
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
#include "sensor/mpr121_boop_sensor.h"
#include "accessory/accessory_leds.h"
#include "sys/system_monitor.h"
#include "sys/scheduler_monitor.h"
#include "net/weather_monitor.h"
#include "net/wifi_monitor.h"
#include "net/ping_monitor.h"
#include "net/bt_monitor.h"
#include "crash_reporter.h"
#include "capture.h"
#include "video_recorder.h"
#include "qr_scanner.h"
#include "splash.h"
#include "hud/background_library.h"
#include "profile_manager.h"
#include "face/face_config.h"
#include "face/face_image.h"
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
static void apply_hud_dock(AppState& s) {
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
    case AppState::ImuSource::Bno055:  return s.imu_bno.heading_deg;
    case AppState::ImuSource::Mpu9250: return s.imu_mpu.heading_deg;
    case AppState::ImuSource::Viture:  return s.imu_viture.heading_deg;
    case AppState::ImuSource::None:    return s.compass_heading;   // freeze
    case AppState::ImuSource::Auto:
    default:
        // Best fresh source wins. If nothing's fresh, hold the most recent
        // value rather than snapping to zero.
        if (fresh(s.imu_bno))                return s.imu_bno   .heading_deg;
        if (fresh(s.imu_mpu))                return s.imu_mpu   .heading_deg;
        if (fresh(s.imu_viture))             return s.imu_viture.heading_deg;
        if (s.imu_bno   .last_us > 0)        return s.imu_bno   .heading_deg;
        if (s.imu_mpu   .last_us > 0)        return s.imu_mpu   .heading_deg;
        if (s.imu_viture.last_us > 0)        return s.imu_viture.heading_deg;
        return s.compass_heading;
    }
}

// Launch the panel_driver.py piomatter shim as a detached child. Used at
// startup AND from the menu's backend hot-swap when switching back to HUB75.
// fork()+setsid() (instead of `system("... &")`) so SIGINT to the parent
// doesn't take the driver down with it.
static void pf_launch_panel_driver(const std::string& bin_dir,
                                   int canvas_w, int canvas_h) {
    std::string drv = bin_dir + "/../scripts/panel_driver.py";
    std::string cw  = std::to_string(canvas_w);
    std::string chh = std::to_string(canvas_h);
    std::system("pkill -f panel_driver.py 2>/dev/null");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int lf = ::open("/tmp/panel_driver.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); ::close(lf); }
        int nf = ::open("/dev/null", O_RDONLY);
        if (nf >= 0) { dup2(nf, 0); ::close(nf); }
        execlp("python3", "python3", "-u", drv.c_str(),
               "--canvas-w", cw.c_str(), "--canvas-h", chh.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);
    }
    std::cout << "[main] launched panel_driver.py pid=" << pid
              << " (" << drv << ")\n";
}

// Build the PanelOutput that NativeFaceController writes into. Reads
// cfg["protoface"]["backend"]:
//   "hub75"   (default) → ShmPusherOutput; panel_driver.py shuttles to LEDs.
//   "max7219"          → Max7219PanelOutput direct-to-spidev, multi-chain.
//   "rgb_matrix"       → NeoPixelMatrixOutput — WS2812-based 8x8 RGB matrix
//                        drop-ins replacing the MAX7219 modules. Same chain
//                        geometry; full RGB per pixel.
// Pulled out as a free helper so both the startup path and the menu's
// hot-swap action use the exact same construction logic.
static std::unique_ptr<face::PanelOutput>
pf_build_panel_output(const json& cfg, const face::RenderConfig& rc) {
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
                    mc.chains.push_back(std::move(cc));
                }
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
                    nc.chains.push_back(std::move(cc));
                }
            }
        }
        return std::make_unique<face::NeoPixelMatrixOutput>(std::move(nc));
    }
    return std::make_unique<face::ShmPusherOutput>(rc.canvas_w, rc.canvas_h);
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

// Mirror axis (the fence column the face mirrors around): the nose's
// horizontal centre, or canvas_w/2 when there's no nose. Used by both the
// editor and the preview crop. Cheap enough to call per frame.
static int pf_mirror_x(const std::string& nose_layout, int canvas_w) {
    if (nose_layout == "1x1") return 37 + 8 / 2;       // = 41
    if (nose_layout == "1x2") return 33 + 16 / 2;      // = 41
    if (nose_layout == "1x3") return 33 + 24 / 2;      // = 45
    return canvas_w / 2;
}

static PfFaceZones pf_compute_face_zones(
        const std::string& eye_layout,
        const std::string& mouth_layout,
        const std::string& nose_layout,
        int canvas_w,
        int canvas_h)
{
    PfFaceZones out;
    auto& zones = out.regions;

    // Eye geometry: WxH = (cols*8) x (rows*8); left eye anchored at (0, 0).
    int eye_cols = 2, eye_rows = 1;
    if      (eye_layout == "1x3") { eye_cols = 3; eye_rows = 1; }
    else if (eye_layout == "2x2") { eye_cols = 2; eye_rows = 2; }
    else if (eye_layout == "2x3") { eye_cols = 3; eye_rows = 2; }
    const int eye_w = eye_cols * 8;
    const int eye_h = eye_rows * 8;
    zones.push_back({"eye_l", cv::Rect(0, 0, eye_w, eye_h)});

    // Nose rect — drives the mirror axis. "none" → no nose region; mirror
    // axis falls back to canvas_w/2.
    cv::Rect nose_rect;
    bool has_nose = true;
    if      (nose_layout == "1x1") nose_rect = cv::Rect(37, 0,  8, 8);
    else if (nose_layout == "1x2") nose_rect = cv::Rect(33, 0, 16, 8);
    else if (nose_layout == "1x3") nose_rect = cv::Rect(33, 0, 24, 8);
    else                            has_nose = false;
    if (has_nose) zones.push_back({"nose", nose_rect});

    // Mirror "fence" — the column index x s.t. col k mirrors to col 2*x - 1 - k.
    // Always derived from nose_layout (or canvas_w/2 when "none") so the
    // editor, the preview crop, and any other caller stay in sync.
    out.mirror_x = pf_mirror_x(nose_layout, canvas_w);

    // Right eye — mirror of left eye around mirror_x.
    const int rey_x = 2 * out.mirror_x - eye_w;
    zones.push_back({"eye_r", cv::Rect(rey_x, 0, eye_w, eye_h)});

    // Mouth geometry: WxH from layout; bottom-left anchored at (4, canvas_h)
    // so the mouth always sits flush with the canvas bottom regardless of
    // how tall the layout is.
    int mouth_cols = 3, mouth_rows = 1;
    if      (mouth_layout == "1x4") { mouth_cols = 4; mouth_rows = 1; }
    else if (mouth_layout == "2x3") { mouth_cols = 3; mouth_rows = 2; }
    else if (mouth_layout == "2x4") { mouth_cols = 4; mouth_rows = 2; }
    const int mouth_w = mouth_cols * 8;
    const int mouth_h = mouth_rows * 8;
    const int mouth_y = std::max(0, canvas_h - mouth_h);
    zones.push_back({"mouth_l", cv::Rect(4, mouth_y, mouth_w, mouth_h)});
    // Right mouth — mirror around mirror_x. left mouth spans [4, 4+mouth_w);
    // mirrored rect starts at 2*mirror_x - (4 + mouth_w).
    const int rmo_x = 2 * out.mirror_x - (4 + mouth_w);
    zones.push_back({"mouth_r", cv::Rect(rmo_x, mouth_y, mouth_w, mouth_h)});

    return out;
}

static face::RenderConfig pf_build_render_config(const json& cfg) {
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

// Canonical expression slot list for the Files > Faces hub. Matches the
// standard whole-face layout shipped in the Protoface repo (faces/main +
// faces/example_fox config.json): neutral + happy/angry/sad/surprised, plus
// the blink eyelid overlay and the optional mouth-open speech state.
namespace {
struct FaceSlot { const char* expression; const char* label; };
constexpr FaceSlot kFaceSlots[] = {
    {"neutral",   "Neutral"},
    {"happy",     "Happy"},
    {"angry",     "Angry"},
    {"sad",       "Sad"},
    {"surprised", "Surprised"},
    {"blink",     "Blink"},
};
constexpr int kFaceSlotCount = sizeof(kFaceSlots) / sizeof(kFaceSlots[0]);

// Mouth-shape overlays (visemes). Not expressions in their own right — they
// blend on top of whichever expression is active when the voice analyzer
// drives mouth_open > 0 and (later) viseme selection picks one of these
// shapes based on spectral centroid. file_stem maps to faces/<active>/<stem>.png,
// canonicalised by face_image_path() inside NativeFaceController.
struct MouthShape { const char* file_stem; const char* label; };
constexpr MouthShape kMouthShapes[] = {
    {"mouth_small", "Small Open"},   // closed-vowel / M-N-D family
    {"mouth_open",  "Wide Open"},    // AH family (was the single mouth_open)
    {"mouth_smile", "Smile"},        // EE family
    {"mouth_round", "Round"},        // OOH family
};
constexpr int kMouthShapeCount = sizeof(kMouthShapes) / sizeof(kMouthShapes[0]);

// Boop reaction faces. The boop sensor's on_boop callback prefers these
// PNG names per zone when present in the active face folder; otherwise
// falls back to the user-configured expression in state.boop_zones.
// Files > Faces > Boop Reactions surfaces them as standard slot rows
// (Play / Edit / Replace / Clear / Import) so users can author them
// just like any other expression slot.
struct BoopFaceSlot { const char* file_stem; const char* label; };
constexpr BoopFaceSlot kBoopFaceSlots[] = {
    {"boop_snout", "Snout"},
    {"boop_left",  "Left Cheek"},
    {"boop_right", "Right Cheek"},
    {"boop_both",  "Both Cheeks"},
};
constexpr int kBoopFaceSlotCount = sizeof(kBoopFaceSlots) / sizeof(kBoopFaceSlots[0]);
} // namespace

// Open the face image picker for a given expression. On commit copies the
// chosen PNG into the active face folder (canonical filename, e.g. happy.png),
// rebuilds the loader so the face reflects the new image, and switches the
// live expression so the user sees the import immediately.
static void import_face_into_slot(MenuSystem* menu,
                                  IFaceController* teensy,
                                  std::string expression,
                                  std::string label) {
    if (!menu || !teensy) return;
    char title[64];
    std::snprintf(title, sizeof(title), "Import %s face PNG", label.c_str());
    std::string start = menu->file_picker_dir();
    menu->open_file_picker(
        title, std::move(start), {".png"},
        [teensy, expression = std::move(expression)](const std::string& src) {
            if (!teensy->import_face_image(expression, src)) {
                std::fprintf(stderr,
                             "[face] import failed for '%s' from '%s'\n",
                             expression.c_str(), src.c_str());
                return;
            }
            teensy->set_face_by_name(expression);
        });
}

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
        // Panel preview placement (anchor/nudge/size, like the camera PiPs)
        OverlayConfig*    protoface_preview_cfg = nullptr,
        // Panel preview view mode: 0=whole face, 1=left half, 2=right half
        int*              protoface_preview_view_pp = nullptr,
        std::string       map_dir         = "/home/user/Pictures/protohud/maps",
        // Eye source selection (render-thread only, no mutex needed)
        EyeSource* left_eye_src  = nullptr,
        EyeSource* right_eye_src = nullptr,
        // Profile management (Profiles tab: save current / load by restart / delete)
        ProfileManager* profiles = nullptr,
        // HUD/menu visual presets (System tab: built-in themes + save/load/delete)
        ProfileManager* hud_presets = nullptr,
        // Out: curated corner "quick menu" tree (assigned if non-null)
        std::vector<MenuItem>* quick_out = nullptr,
        // GIF folder for the Animations preview (scan order matches play_gif index)
        std::string gifs_dir = {},
        // Landing-page background library (set after construction; pointer-to-pointer
        // so the menu can capture it before bg_lib exists, same pattern as menu_sys_pp)
        BackgroundLibrary** bg_lib_pp = nullptr,
        // User-writable backgrounds folder ($HOME/protohud/backgrounds). Imports
        // land here; bundled defaults under assets/backgrounds are read-only.
        std::string bg_user_dir = {},
        // Boop sensor (set after construction; same pointer-to-pointer pattern).
        // Menu toggles/sliders forward live changes via the BoopSensor interface
        // so the next poll cycle picks up the new threshold / enable state.
        sensor::BoopSensor** boop_sensor_pp = nullptr,
        // Voice analyzer (owned by AudioEngine; main passes its address). Menu
        // sliders write through it so the next FFT cycle uses the new params.
        audio::VoiceAnalyzer* voice_analyzer = nullptr,
        // Accessory LED chain (cheekhubs + fins). Menu toggles/sliders push
        // through its zone setters so the next render tick uses them.
        accessory::AccessoryLeds* leds = nullptr,
        // Hot-swap callback for Protoface > Hardware > Backend; main wires it
        // to the tear-down-and-rebuild routine that swaps NativeFaceController
        // and panel_driver.py for the new backend. pf_backend_p is the live
        // backend name string for the radio's get_state.
        std::function<void(const std::string&)> swap_backend = nullptr,
        const std::string* pf_backend_p = nullptr,
        // Edit… callback for Files > Faces > <expr> > Edit. Main wires it
        // to a routine that polls the native controller for canvas dims +
        // covered chain regions, opens the face editor, writes the PNG on
        // commit, and reloads the face.
        std::function<void(const std::string& expression)> edit_face = nullptr,
        // Chain layout pickers — pointers so the radios can read the live
        // value and mutate it in place. Used by the MAX7219 / RGB matrix
        // editor to draw labelled eye / nose / mouth zones.
        std::string* pf_eye_layout_p   = nullptr,
        std::string* pf_mouth_layout_p = nullptr,
        std::string* pf_nose_layout_p  = nullptr)
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

    // Attach a context panel to a SUBMENU item.  Returns the same item so
    // calls can be chained at the call site:
    //   with_panel(submenu("Position", ...), "Eye Position Preview",
    //              [&state]( ImDrawList* dl, ImVec2 o, ImVec2 s){ ... })
    auto with_panel = [](MenuItem m, std::string title,
                         MenuContextPanelDraw draw) -> MenuItem {
        m.context_panel_title = std::move(title);
        m.context_panel_draw  = std::move(draw);
        return m;
    };

    // Attach a right-pane context description to any item (shown in the deep menu).
    //   with_desc(slider(...), "What this changes and why.")
    auto with_desc = [](MenuItem m, std::string desc) -> MenuItem {
        m.description = std::move(desc);
        return m;
    };

    // Make an option leaf apply its effect as soon as it's highlighted (live
    // preview), so tabbing through zoom/crop/position options updates the preview
    // without a select. Reuses the item's own action.
    auto live = [](MenuItem m) -> MenuItem {
        m.on_highlight = m.action;
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

    std::vector<MenuItem> gifs;
    for (uint8_t i = 0; i < 8; i++) gifs.push_back(gif_leaf(i));

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

    // ── Faces ────────────────────────────────────────────────────────────────
    // Static-PNG preview shared by the Files > Faces hub. Mirrors the GIF
    // preview: the highlighted slot's image is decoded once on the render
    // thread (via face::load_png_rgba) and cached in a GL texture until the
    // path changes.
    struct FacePreview {
        std::string                     loaded_path;    // ("" = no image cached)
        std::filesystem::file_time_type loaded_mtime{}; // detects in-place replace
        cv::Mat     image;         // CV_8UC4 RGBA, panel-sized
        int         want = -1;     // highlighted slot index
        GLuint      tex  = 0;
    };
    auto face_preview = std::make_shared<FacePreview>();

    MenuContextPanelDraw draw_face_preview =
        [face_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *face_preview;
            std::string path, expr_label;
            if (fp.want >= 0 && fp.want < kFaceSlotCount) {
                const auto& s = kFaceSlots[fp.want];
                expr_label = s.label;
                if (teensy->face_image_exists(s.expression))
                    path = teensy->face_image_path(s.expression);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                // 256×128 = 2:1 (HUB75 panel-pair aspect), NEAREST resize
                // preserves the pixel-art look.
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            // 2:1 thumbnail (matches HUB75 panel-pair canvas), centred, leaving
            // a line for the expression label beneath.
            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph},
                              IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!expr_label.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(expr_label.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), expr_label.c_str());
            }
        };

    // One management row per canonical expression: dynamic label, Play /
    // Replace / Clear when present on disk, Import otherwise.
    // Edit… visibility — true only when the active backend has addressable
    // LED regions (today: MAX7219 / RGB matrix). HUB75 + daemon return
    // false here and the leaf stays hidden.
    auto have_led_regions = [teensy]{ return teensy->has_led_face_editor(); };

    auto face_slot_row = [&, face_preview, edit_face](int slot_idx) -> MenuItem {
        const std::string expr  = kFaceSlots[slot_idx].expression;
        const std::string label = kFaceSlots[slot_idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        m.label_fn = [teensy, expr, label]() -> std::string {
            return teensy->face_image_exists(expr) ? label : (label + " (empty)");
        };

        m.on_highlight = [face_preview, slot_idx]{ face_preview->want = slot_idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem play = leaf("Play",
            [teensy, expr]{ teensy->set_face_by_name(expr); });
        play.visible_fn = bound_now;

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        // Edit launches the pixel editor on this slot's PNG. Visible only
        // when the active backend exposes covered LED regions — keeps the
        // option hidden in HUB75 / daemon modes where the editor would
        // have nothing meaningful to draw against.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        m.children = { std::move(play), std::move(edit_it),
                       std::move(replace), std::move(clear), std::move(imp) };
        return m;
    };

    std::vector<MenuItem> face_files_menu;
    for (int i = 0; i < kFaceSlotCount; ++i)
        face_files_menu.push_back(face_slot_row(i));

    // ── Mouth Shapes (viseme overlays) ───────────────────────────────────────
    // Same import/preview pipeline as the expression slots, minus Play (these
    // are overlay assets — calling set_face_by_name("mouth_*") would just
    // fall back to neutral since they aren't entries in the loader's
    // expressions_ map). The voice analyzer's spectral centroid drives shape
    // selection at the FaceLoader layer in a later patch; this one only adds
    // the import slots so users can author the assets ahead of time.
    auto mouth_preview = std::make_shared<FacePreview>();

    MenuContextPanelDraw draw_mouth_preview =
        [mouth_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *mouth_preview;
            std::string path, mouth_label;
            if (fp.want >= 0 && fp.want < kMouthShapeCount) {
                const auto& s = kMouthShapes[fp.want];
                mouth_label = s.label;
                if (teensy->face_image_exists(s.file_stem))
                    path = teensy->face_image_path(s.file_stem);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph},
                              IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!mouth_label.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(mouth_label.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), mouth_label.c_str());
            }
        };

    auto mouth_slot_row = [&, mouth_preview, edit_face, have_led_regions](int idx) -> MenuItem {
        const std::string expr  = kMouthShapes[idx].file_stem;
        const std::string label = kMouthShapes[idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        m.label_fn = [teensy, expr, label]() -> std::string {
            return teensy->face_image_exists(expr) ? label : (label + " (empty)");
        };
        m.on_highlight = [mouth_preview, idx]{ mouth_preview->want = idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        // Edit the mouth-shape PNG with the pixel editor (mono on MAX7219,
        // color on RGB matrix). Same visibility gate as the expression
        // slots — hidden in HUB75 / daemon modes.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        m.children = { std::move(edit_it), std::move(replace),
                       std::move(clear), std::move(imp) };
        return m;
    };

    std::vector<MenuItem> face_mouth_menu;
    for (int i = 0; i < kMouthShapeCount; ++i)
        face_mouth_menu.push_back(mouth_slot_row(i));

    face_files_menu.push_back(
        with_desc(with_panel(submenu("Mouth Shapes", std::move(face_mouth_menu)),
                             "Mouth Preview", draw_mouth_preview),
                  "Viseme overlays. Bond on top of the active expression when "
                  "the voice analyzer drives mouth_open > 0. Asset filenames: "
                  "mouth_small.png, mouth_open.png, mouth_smile.png, "
                  "mouth_round.png in the active face folder."));

    // ── Boop Reactions ───────────────────────────────────────────────────────
    // Authoring slots for the per-zone boop reaction faces. When a PNG
    // exists at faces/<active>/boop_<zone>.png, the on_boop callback above
    // triggers that face instead of the user's configured fallback
    // expression. Filenames: boop_snout / boop_left / boop_right / boop_both.
    auto boop_face_preview = std::make_shared<FacePreview>();
    MenuContextPanelDraw draw_boop_face_preview =
        [boop_face_preview, teensy](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            FacePreview& fp = *boop_face_preview;
            std::string path, lbl;
            if (fp.want >= 0 && fp.want < kBoopFaceSlotCount) {
                const auto& s = kBoopFaceSlots[fp.want];
                lbl = s.label;
                if (teensy->face_image_exists(s.file_stem))
                    path = teensy->face_image_path(s.file_stem);
            }
            const bool have = !path.empty();

            std::filesystem::file_time_type mt{};
            if (have) {
                std::error_code ec;
                mt = std::filesystem::last_write_time(path, ec);
            }
            if (have && (path != fp.loaded_path || mt != fp.loaded_mtime)) {
                fp.image        = face::load_png_rgba(path, 256, 128);
                fp.loaded_path  = path;
                fp.loaded_mtime = mt;
            } else if (!have && !fp.loaded_path.empty()) {
                fp.image        = cv::Mat();
                fp.loaded_path.clear();
                fp.loaded_mtime = {};
            }

            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f);
            const float ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f;
            const float py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));

            if (have && !fp.image.empty() && fp.image.isContinuous()) {
                if (fp.tex == 0) {
                    glGenTextures(1, &fp.tex);
                    glBindTexture(GL_TEXTURE_2D, fp.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, fp.tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fp.image.cols, fp.image.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, fp.image.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(fp.tex)),
                             {px, py}, {px + pw, py + ph});
            } else {
                const char* msg = have ? "Decode failed" : "(empty)";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f,
                             py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), msg);
            }

            if (!lbl.empty()) {
                const ImVec2 ns = ImGui::CalcTextSize(lbl.c_str());
                dl->AddText({o.x + sz.x * 0.5f - ns.x * 0.5f, o.y + sz.y - ns.y},
                            IM_COL32(220, 230, 235, 230), lbl.c_str());
            }
        };

    // Boop slot row: Play pops the dedicated boop face via trigger_boop
    // (so it auto-reverts after the slot's duration). Edit / Replace /
    // Clear / Import behave the same as the expression slot rows.
    auto boop_face_row = [&, boop_face_preview, edit_face, have_led_regions](int idx) -> MenuItem {
        const std::string expr  = kBoopFaceSlots[idx].file_stem;
        const std::string label = kBoopFaceSlots[idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        m.label_fn = [teensy, expr, label]() -> std::string {
            return teensy->face_image_exists(expr) ? label : (label + " (empty)");
        };
        m.on_highlight = [boop_face_preview, idx]{ boop_face_preview->want = idx; };

        auto bound_now = [teensy, expr]{ return teensy->face_image_exists(expr); };

        MenuItem play = leaf("Play",
            [teensy, expr, idx, &state]() {
                double dur = 0.8;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (idx >= 0 && idx < 4) dur = state.boop_zones[idx].duration_s;
                }
                teensy->trigger_boop(expr, dur);
            });
        play.visible_fn = bound_now;

        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem replace = leaf("Replace...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        replace.visible_fn = bound_now;

        MenuItem clear = leaf("Clear",
            [teensy, expr]{ teensy->clear_face_image(expr); });
        clear.visible_fn = bound_now;

        MenuItem imp = leaf("Import...",
            [teensy, menu_sys_pp, expr, label]() {
                import_face_into_slot(menu_sys_pp ? *menu_sys_pp : nullptr,
                                      teensy, expr, label);
            });
        imp.visible_fn = [bound_now]{ return !bound_now(); };

        m.children = { std::move(play), std::move(edit_it),
                       std::move(replace), std::move(clear), std::move(imp) };
        return m;
    };

    std::vector<MenuItem> face_boop_menu;
    for (int i = 0; i < kBoopFaceSlotCount; ++i)
        face_boop_menu.push_back(boop_face_row(i));

    face_files_menu.push_back(
        with_desc(with_panel(submenu("Boop Reactions", std::move(face_boop_menu)),
                             "Boop Reaction Preview", draw_boop_face_preview),
                  "Dedicated face per boop zone. When a slot's PNG exists, "
                  "the boop sensor triggers that face instead of the zone's "
                  "fallback expression. Filenames: boop_snout, boop_left, "
                  "boop_right, boop_both — all in the active face folder."));

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
        with_desc(toggle("Enable Low-Light",
            [&state]{ return state.night_vision.nv_enabled; },
            [&state](bool v){
                state.night_vision.nv_enabled  = v;
                state.night_vision.exposure_ev = v ? 3.0f : 0.0f;
                state.night_vision.shutter_us  = v ? 40000 : 16667;
            }),
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
        zoom_level_menu.push_back(live(leaf_sel(
            z.label,
            [&state, zoom = z.zoom]{
                state.zoom_left.zoom  = zoom;
                state.zoom_right.zoom = zoom;
            },
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

    std::vector<MenuItem> capture_menu = {
        leaf("Left Eye",   [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Left;   }),
        leaf("Right Eye",  [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Right;  }),
        leaf("Both Eyes",  [&state]{ std::lock_guard lk(state.mtx); state.capture_request = CaptureRequest::Stereo; }),
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
        toggle("Record",
            [&state]{ return state.video_recording; },
            [&state](bool v){ std::lock_guard lk(state.mtx);
                              state.video_request = v ? VideoRequest::Start : VideoRequest::Stop; }),
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
        toggle("Enable",
            [&state]{ return state.theater_mode; },
            [&state](bool v){ state.theater_mode = v; }),
        submenu("Position", std::move(theater_pos_menu)),
    };

    // Build per-camera submenu labels from the configured model names.  When both
    // cameras share a model (e.g. default OWLsight pair) disambiguate with #1/#2
    // plus an eye hint; otherwise show "<Model> (Left/Right)".
    auto cam_label = [](std::string model, const char* eye, const char* index) {
        if (model.empty()) model = "Owlsight";
        std::string out = std::move(model);
        if (index) { out += " #"; out += index; }
        out += " (";
        out += eye;
        out += ")";
        return out;
    };
    std::string left_model  = cameras ? cameras->owl_left_model()  : std::string();
    std::string right_model = cameras ? cameras->owl_right_model() : std::string();
    std::string left_norm   = left_model.empty()  ? "Owlsight" : left_model;
    std::string right_norm  = right_model.empty() ? "Owlsight" : right_model;
    const bool same_model   = (left_norm == right_norm);
    std::string left_label  = cam_label(left_model,  "Left",  same_model ? "1" : nullptr);
    std::string right_label = cam_label(right_model, "Right", same_model ? "2" : nullptr);

    std::vector<MenuItem> main_cameras_menu = {
        submenu("Left Eye Source",  make_eye_source_menu(left_eye_src)),
        submenu("Right Eye Source", make_eye_source_menu(right_eye_src)),
        with_panel(
            with_desc(submenu("Raw View", std::move(raw_view_menu)),
                "Pass the camera feed straight through. Toggle Enable to show it; "
                "Position places each eye. The preview at right shows it live."),
            "Raw View Preview", eye_pos_preview),
        toggle("Swap Cameras",
            [&state]{ return state.cameras_swapped; },
            [&state](bool v){ state.cameras_swapped = v; }),
        with_panel(
            submenu("Resolution", std::move(resolution_presets)),
            "Camera Resolution",
            [&state, menu_sys_pp]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                int w = state.camera_resolution.width;
                int h = state.camera_resolution.height;
                if (w <= 0 || h <= 0) { w = 1280; h = 800; }
                float ar = float(w) / float(h);

                char buf[64];
                snprintf(buf, sizeof(buf), "%d x %d", w, h);
                dl->AddText({ o.x, o.y }, IM_COL32(255, 255, 255, 235), buf);
                snprintf(buf, sizeof(buf), "Aspect: %.2f:1 (~%s)",
                         ar,
                         (ar > 1.74f && ar < 1.80f) ? "16:9"
                         : (ar > 1.30f && ar < 1.36f) ? "4:3"
                         : (ar > 1.58f && ar < 1.62f) ? "16:10"
                         : "custom");
                dl->AddText({ o.x, o.y + 18.f }, IM_COL32(200, 200, 200, 220), buf);

                // Hint list — common Raspberry Pi camera modules grouped by AR.
                const float lh = 14.f;
                ImVec2 p{ o.x, o.y + 44.f };
                dl->AddText(p, (accent & 0x00FFFFFFu) | (200u << 24),
                            "PI CAMERA NATIVE AR");
                p.y += lh + 2.f;
                struct Row { const char* ar; const char* mods; };
                static const Row rows[] = {
                    { "4:3",  "IMX219 v2, IMX477 HQ, IMX708 v3 (bin)" },
                    { "16:9", "IMX477 HQ, IMX708 v3 (native)" },
                    { "1:1",  "IMX296, IMX290 (crop)" },
                };
                for (auto& r : rows) {
                    char line[96];
                    snprintf(line, sizeof(line), "%-5s  %s", r.ar, r.mods);
                    dl->AddText(p, IM_COL32(190, 190, 190, 200), line);
                    p.y += lh;
                }
            }),
        with_panel(
            with_desc(submenu("Digital Zoom", std::move(zoom_menu)),
                  "Magnify both eyes equally and choose where the crop is centered. "
                  "Higher zoom shows less of the scene at greater detail. Preview at right."),
            "Digital Zoom Preview", digital_zoom_preview),
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
                // Left eye crop biased toward the right (nose); right eye crop
                // biased toward the left.  bias=0 → centered.
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
            with_desc(submenu("Single Camera", std::move(single_cam_menu)),
                "Show ONE camera filling a region of the screen (full, or a half) instead "
                "of the stereo pair. Choose the camera and anchor; preview at right."),
            "Single Camera Preview", single_cam_preview),
        submenu(left_label,         std::move(left_cam_menu)),
        submenu(right_label,        std::move(right_cam_menu)),
        submenu("Low-Light Mode",   std::move(nv_menu)),
        submenu("Autofocus Both",   std::move(af_both_menu)),
        submenu("Capture Photo",    std::move(capture_menu)),
        submenu("Record Video",     std::move(video_menu)),
        submenu("QR Scan",          std::move(qr_menu)),
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
        slider("Backlight Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
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
    // Helper: build a "Resolution" submenu for one USB camera slot.
    // If the stream is currently open, close it and reopen with the new dimensions.
    auto make_resolution_items = [cameras, &leaf_sel](
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
        submenu("Resolution",  make_resolution_items(
            [cameras]{ return cameras->usb1_cfg(); },
            [cameras](UsbCamConfig c){ cameras->update_usb1_cfg(c); },
            [cameras]{ return cameras && cameras->usb1_ok(); },
            [cameras]{ cameras->close_usb1(); },
            [cameras]{ cameras->open_usb1(); })),
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
        submenu("Resolution", make_resolution_items(
            [cameras]{ return cameras->usb2_cfg(); },
            [cameras](UsbCamConfig c){ cameras->update_usb2_cfg(c); },
            [cameras]{ return cameras && cameras->usb2_ok(); },
            [cameras]{ cameras->close_usb2(); },
            [cameras]{ cameras->open_usb2(); })),
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
        submenu("Resolution",  make_resolution_items(
            [cameras]{ return cameras->usb3_cfg(); },
            [cameras](UsbCamConfig c){ cameras->update_usb3_cfg(c); },
            [cameras]{ return cameras && cameras->usb3_ok(); },
            [cameras]{ cameras->close_usb3(); },
            [cameras]{ cameras->open_usb3(); })),
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
        toggle("Show Overlay",
            [android_overlay]{ return *android_overlay; },
            [android_overlay](bool v){ *android_overlay = v; }),
        submenu("Position", make_position_items(android_cfg)),
        make_size_slider("Size", android_cfg),
    };

    // ── ProtoTracer (Teensy-driven LED matrix) submenu ────────────────────────
    std::vector<MenuItem> prototracer_inner_menu = {
        submenu("Faces",              std::move(effects)),
        submenu("Color",              std::move(colors)),
        submenu("ProtoTracer Palette", std::move(proto_colors)),
        with_panel(submenu("Animations", std::move(gifs)),
                   "GIF Preview", draw_gif_preview),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
        leaf("Release Control", [teensy]{ teensy->release_control(); }),
        leaf("Save Face Config", [teensy]{ teensy->save_config(); }),
        face_picker("Face", 10,
            [&state]{ return static_cast<int>(state.face.face_index); },
            [&state, teensy](int v){
                teensy->set_menu_item(0, static_cast<uint8_t>(v));
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.face_index = static_cast<uint8_t>(v);
            }),
        slider("Accent LED Brightness", 0.f, 10.f, 1.f, "",
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
        toggle("Touch Sensor",
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

    // ── Protoface (shm-fed LED panel) submenu ─────────────────────────────────
    // Protoface has a different effect_id map and material palette than
    // ProtoTracer, so it gets a dedicated control set. Commands forward to the
    // active backend via the FaceProxy — select "Source: Protoface" first.
    std::vector<MenuItem> pf_effects;
    {
        const char* pf_effect_names[] = {
            "None","Sparkle","Embers","Rain","Snow","Confetti","Rings","Fireflies",
            "Fire","Aurora","Blizzard","Sonar","Plasma","Celebration","Galaxy","Party",
            "Clouds","Nebula",   // Protoface-only (ids 16,17); no ProtoTracer equivalent
        };
        const uint8_t pf_effect_count =
            static_cast<uint8_t>(sizeof(pf_effect_names) / sizeof(pf_effect_names[0]));
        for (uint8_t id = 0; id < pf_effect_count; id++)
            pf_effects.push_back(leaf_sel(pf_effect_names[id],
                [teensy, id, &state]{
                    teensy->set_effect(id);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.effect_id = id;
                },
                [&state, id]{ return state.face.effect_id == id; }));
    }

    std::vector<MenuItem> pf_colors;
    pf_colors.push_back(leaf("Teal",   [teensy]{ teensy->set_color(0,220,180);   }));
    pf_colors.push_back(leaf("Red",    [teensy]{ teensy->set_color(255,0,0);     }));
    pf_colors.push_back(leaf("Orange", [teensy]{ teensy->set_color(255,110,0);   }));
    pf_colors.push_back(leaf("Green",  [teensy]{ teensy->set_color(0,255,0);     }));
    pf_colors.push_back(leaf("Blue",   [teensy]{ teensy->set_color(0,90,255);    }));
    pf_colors.push_back(leaf("Purple", [teensy]{ teensy->set_color(160,0,255);   }));
    pf_colors.push_back(leaf("White",  [teensy]{ teensy->set_color(255,255,255); }));
    pf_colors.push_back(color_picker("Custom Color",
        [teensy](uint8_t r, uint8_t g, uint8_t b){ teensy->set_color(r, g, b); },
        [&state]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
            return { state.face.r, state.face.g, state.face.b };
        }));

    std::vector<MenuItem> pf_palette;
    {
        struct PFMat { const char* label; uint8_t idx; };
        const PFMat pf_mats[] = {
            { "Teal",    0 }, { "Yellow", 1 }, { "Orange", 2 }, { "White", 3 },
            { "Green",   4 }, { "Purple", 5 }, { "Red",    6 }, { "Blue",  7 },
            { "Rainbow", 8 }, { "Cool",   9 }, { "Warm",  10 }, { "Black",11 },
        };
        for (const auto& m : pf_mats)
            pf_palette.push_back(leaf_sel(m.label,
                [teensy, idx = m.idx, &state]{
                    teensy->set_menu_item(8, idx);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.material_color = idx;
                },
                [&state, idx = m.idx]{ return state.face.material_color == idx; }));
    }

    std::vector<MenuItem> pf_gifs;
    for (uint8_t i = 0; i < 8; i++) pf_gifs.push_back(gif_leaf(i));

    // ── Protoface > Hardware > Backend ────────────────────────────────────
    // Switches NativeFaceController between HUB75 panels (via piomatter
    // + panel_driver.py) and direct-to-spidev MAX7219 matrices. The toggle
    // fires a hot-swap callback main.cpp owns; HUD keeps rendering through
    // the transition.
    std::vector<MenuItem> pf_backend_items = {
        leaf_sel("HUB75 panels (piomatter)",
            [swap_backend]{ if (swap_backend) swap_backend("hub75"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "hub75"; }),
        leaf_sel("MAX7219 matrices (direct SPI)",
            [swap_backend]{ if (swap_backend) swap_backend("max7219"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "max7219"; }),
        leaf_sel("RGB matrix (WS2812 drop-in for MAX7219)",
            [swap_backend]{ if (swap_backend) swap_backend("rgb_matrix"); },
            [pf_backend_p]{ return pf_backend_p && *pf_backend_p == "rgb_matrix"; }),
    };
    // Chain Layout pickers — drive the editor's eye/nose/mouth bounding
    // boxes. Each radio mutates the live string the editor reads when it
    // opens, so the next "Edit…" shows the updated zones. Only shown for
    // pixel-grid backends (MAX7219 / RGB matrix); HUB75 has no per-zone
    // chain concept.
    auto layout_pick = [&leaf_sel](const char* lbl, std::string* slot,
                                   const char* value) -> MenuItem {
        return leaf_sel(lbl,
            [slot, value]{ if (slot) *slot = value; },
            [slot, value]{ return slot && *slot == value; });
    };
    auto visible_for_native = [pf_backend_p] {
        return pf_backend_p &&
               (*pf_backend_p == "max7219" || *pf_backend_p == "rgb_matrix");
    };
    std::vector<MenuItem> eye_items = {
        layout_pick("1x2  (16x8)",  pf_eye_layout_p, "1x2"),
        layout_pick("1x3  (24x8)",  pf_eye_layout_p, "1x3"),
        layout_pick("2x2  (16x16)", pf_eye_layout_p, "2x2"),
        layout_pick("2x3  (24x16)", pf_eye_layout_p, "2x3"),
    };
    std::vector<MenuItem> mouth_items = {
        layout_pick("1x3  (24x8)",  pf_mouth_layout_p, "1x3"),
        layout_pick("1x4  (32x8)",  pf_mouth_layout_p, "1x4"),
        layout_pick("2x3  (24x16)", pf_mouth_layout_p, "2x3"),
        layout_pick("2x4  (32x16)", pf_mouth_layout_p, "2x4"),
    };
    std::vector<MenuItem> nose_items = {
        layout_pick("None",         pf_nose_layout_p, "none"),
        layout_pick("1x1  (8x8)",   pf_nose_layout_p, "1x1"),
        layout_pick("1x2  (16x8)",  pf_nose_layout_p, "1x2"),
        layout_pick("1x3  (24x8)",  pf_nose_layout_p, "1x3"),
    };
    std::vector<MenuItem> pf_chain_layout_items = {
        with_desc(submenu("Eyes",  std::move(eye_items)),
                  "Panels per eye. The right eye is mirrored on the same row "
                  "so both eyes match. Used by the face editor to outline "
                  "Left Eye / Right Eye zones."),
        with_desc(submenu("Mouth", std::move(mouth_items)),
                  "Mouth panels (anchor 4,25). Used by the face editor to "
                  "outline the Mouth zone."),
        with_desc(submenu("Nose",  std::move(nose_items)),
                  "Nose panels (single row, centred). \"None\" omits the "
                  "nose zone entirely."),
    };
    MenuItem pf_chain_layout_item = with_desc(
        submenu("Chain Layout", std::move(pf_chain_layout_items)),
        "Pick panels per zone — drives the bounding boxes the face editor "
        "highlights so you can see which canvas pixels each panel will "
        "display. Persisted to config.json.");
    pf_chain_layout_item.visible_fn = visible_for_native;

    std::vector<MenuItem> pf_hardware_menu = {
        with_desc(submenu("Backend", std::move(pf_backend_items)),
                  "What LED hardware Protoface paints. Switching tears down "
                  "the running renderer and brings up a new one with the new "
                  "backend; the HUD keeps running through the transition. "
                  "Persists to config.json so the next launch starts here."),
        std::move(pf_chain_layout_item),
    };

    // Visibility predicates — Effects / Face Color / Material Color /
    // Animations / Save Face Config / Release Control are concepts from
    // the existing HUB75 + Protoface-daemon path; the native MAX7219 /
    // RGB-matrix renderer doesn't speak them. Hide on those backends so
    // the menu shows only what the active hardware actually responds to.
    auto visible_for_hub75 = [pf_backend_p] {
        // Default to "shown" if no backend pointer is plumbed — keeps
        // older callers (and the Protoface-daemon mode where pf_backend
        // isn't relevant) working.
        return !pf_backend_p || *pf_backend_p == "hub75";
    };
    auto gated = [](MenuItem m, std::function<bool()> vis) -> MenuItem {
        m.visible_fn = std::move(vis);
        return m;
    };

    // Panel preview controls — toggle + position + size + view — all wrap
    // into one "Panel Preview" submenu so the parent stays tidy when
    // visibility-gating filters cut the menu down to the essentials.
    std::vector<MenuItem> pf_preview_menu;
    if (panel_preview_pp)
        pf_preview_menu.push_back(toggle("Enabled",
            [panel_preview_pp]{ return *panel_preview_pp; },
            [panel_preview_pp](bool v){ *panel_preview_pp = v; }));
    if (protoface_preview_cfg) {
        pf_preview_menu.push_back(submenu("Position",
            make_position_items(protoface_preview_cfg)));
        pf_preview_menu.push_back(make_size_slider("Size", protoface_preview_cfg));
    }
    if (protoface_preview_view_pp) {
        int* vp = protoface_preview_view_pp;
        // The view picker (Whole Face / Left / Right) only makes sense on
        // HUB75: that canvas is a mirrored pair (two physical panels, each
        // holding one face), so the user can choose which half to preview.
        // MAX7219 / RGB-matrix canvases already get cropped to a single
        // centred face in pick_face_tex, so this picker is hidden there.
        MenuItem view_it = submenu("View", std::vector<MenuItem>{
            leaf_sel("Whole Face", [vp]{ *vp = 0; }, [vp]{ return *vp == 0; }),
            leaf_sel("Left Half",  [vp]{ *vp = 1; }, [vp]{ return *vp == 1; }),
            leaf_sel("Right Half", [vp]{ *vp = 2; }, [vp]{ return *vp == 2; }),
        });
        view_it.visible_fn = visible_for_hub75;
        pf_preview_menu.push_back(std::move(view_it));
    }

    std::vector<MenuItem> protoface_inner_menu = {
        gated(submenu("Effects",        std::move(pf_effects)),  visible_for_hub75),
        gated(submenu("Face Color",     std::move(pf_colors)),   visible_for_hub75),
        gated(submenu("Material Color", std::move(pf_palette)),  visible_for_hub75),
        // Face PNGs (per-expression slots, mouth shapes, boop reactions)
        // live here under Protoface rather than the generic Files menu —
        // they're meaningful per-backend, and the editor only makes sense
        // when the active backend supports it (MAX7219 / RGB matrix).
        with_desc(with_panel(submenu("Faces", std::move(face_files_menu)),
                             "Face Preview", draw_face_preview),
                  "Per-expression face PNGs and the in-HUD pixel editor. "
                  "Edit... opens whenever the active backend (Hardware > "
                  "Backend) has an editor capability — today: MAX7219 and "
                  "RGB matrix; HUB75 stays import-only. Files are stored "
                  "in faces/<active>[_<backend>]/ so each panel technology "
                  "keeps its own art."),
        gated(with_panel(submenu("Animations", std::move(pf_gifs)),
                         "GIF Preview", draw_gif_preview), visible_for_hub75),
        slider("Brightness", 0.f, 255.f, 5.f, "%",
            [&state]{ return static_cast<float>(state.face.brightness); },
            [teensy](float v){ teensy->set_brightness(static_cast<uint8_t>(v)); }),
        submenu("Hardware",       std::move(pf_hardware_menu)),
        gated(leaf("Save Face Config", [teensy]{ teensy->save_config(); }),
              visible_for_hub75),
        gated(leaf("Release Control",  [teensy]{ teensy->release_control(); }),
              visible_for_hub75),
    };
    // "Start Protoface" first: launch the daemon (if not running) and make it the
    // active source. Targets the Protoface backend directly (not the proxy).
    if (fp_option) {
        protoface_inner_menu.insert(protoface_inner_menu.begin(),
            gated(leaf("Start Protoface", [fp_option, active_face_pp]{
                fp_option->launch();
                if (active_face_pp) *active_face_pp = fp_option;
            }), visible_for_hub75));
        protoface_inner_menu.insert(protoface_inner_menu.begin() + 1,
            gated(leaf("Restart Protoface", [fp_option, active_face_pp]{
                fp_option->restart();
                if (active_face_pp) *active_face_pp = fp_option;
            }), visible_for_hub75));
    }
    if (!pf_preview_menu.empty())
        protoface_inner_menu.push_back(submenu("Panel Preview",
                                               std::move(pf_preview_menu)));

    // ── Face Display root: Source picker (radios) + per-backend submenus ─────
    std::vector<MenuItem> face_display_menu;
    if (active_face_pp && teensy_option && fp_option) {
        face_display_menu.push_back(leaf_sel("Source: Teensy (ProtoTracer)",
            [active_face_pp, teensy_option]{ *active_face_pp = teensy_option; },
            [active_face_pp, teensy_option]{ return *active_face_pp == teensy_option; }));
        face_display_menu.push_back(leaf_sel("Source: Protoface",
            [active_face_pp, fp_option]{ *active_face_pp = fp_option; },
            [active_face_pp, fp_option]{ return *active_face_pp == fp_option; }));
    }
    face_display_menu.push_back(submenu("ProtoTracer", std::move(prototracer_inner_menu)));
    if (!protoface_inner_menu.empty())
        face_display_menu.push_back(submenu("Protoface", std::move(protoface_inner_menu)));

    // ── Boop sensor (Protoface-side reactive behaviour) ──────────────────────
    // One submenu per zone, each with Enabled / Expression / Hold Duration /
    // Touch Threshold (lower = more sensitive) / Test. Tunable changes mirror
    // into the sensor immediately so the next poll cycle uses them; the
    // values persist to config.json via mutate_cfg on shutdown.
    auto boop_zone_menu = [&, teensy, boop_sensor_pp](int idx, std::string label) -> MenuItem {
        const auto zone_enum = static_cast<sensor::BoopSensor::Zone>(idx);

        std::vector<MenuItem> expr_items;
        for (int ei = 0; ei < kFaceSlotCount; ++ei) {
            const std::string expr     = kFaceSlots[ei].expression;
            const std::string ex_label = kFaceSlots[ei].label;
            expr_items.push_back(leaf_sel(ex_label,
                [&state, idx, expr]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.boop_zones[idx].expression = expr;
                },
                [&state, idx, expr]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return state.boop_zones[idx].expression == expr;
                }));
        }

        std::vector<MenuItem> items = {
            toggle("Enabled",
                [&state, idx]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return state.boop_zones[idx].enabled;
                },
                [&state, idx, boop_sensor_pp, zone_enum](bool v){
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.boop_zones[idx].enabled = v;
                    }
                    if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                        s->set_zone_enabled(zone_enum, v);
                }),
            with_desc(submenu("Expression", std::move(expr_items)),
                      "Which face the boop triggers. Auto-reverts when the "
                      "hold duration elapses."),
            slider("Hold Duration", 0.2f, 3.0f, 0.1f, " s",
                [&state, idx]{
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return static_cast<float>(state.boop_zones[idx].duration_s);
                },
                [&state, idx](float v){
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.boop_zones[idx].duration_s = static_cast<double>(v);
                }),
            // Touch threshold is meaningless for the BothCheeks zone — it's
            // derived from the left/right cheek events via the coalescer.
            // Hide the slider there so the menu stays tidy.
            [&]{
                MenuItem m = with_desc(slider("Touch Threshold", 4.f, 30.f, 1.f, "",
                    [&state, idx]{
                        std::lock_guard<std::mutex> lk(state.mtx);
                        return static_cast<float>(state.boop_zones[idx].threshold);
                    },
                    [&state, idx, boop_sensor_pp, zone_enum](float v){
                        const auto t = static_cast<uint8_t>(v);
                        {
                            std::lock_guard<std::mutex> lk(state.mtx);
                            state.boop_zones[idx].threshold = t;
                        }
                        if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                            s->set_zone_threshold(zone_enum, t);
                    }),
                    "MPR121 touch threshold. Lower numbers are more sensitive; "
                    "raise if you see false triggers.");
                m.visible_fn = [idx]{ return idx != static_cast<int>(sensor::BoopSensor::Zone::BothCheeks); };
                return m;
            }(),
            leaf("Test Boop",
                [teensy, &state, idx]{
                    std::string expr;
                    double dur = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        expr = state.boop_zones[idx].expression;
                        dur  = state.boop_zones[idx].duration_s;
                    }
                    if (!expr.empty()) teensy->trigger_boop(expr, dur);
                }),
        };
        return submenu(std::move(label), std::move(items));
    };

    std::vector<MenuItem> boop_menu = {
        boop_zone_menu(0, "Snout"),
        boop_zone_menu(1, "Left Cheek"),
        boop_zone_menu(2, "Right Cheek"),
        boop_zone_menu(3, "Both Cheeks"),
        with_desc(slider("Coalesce Window", 0.f, 0.30f, 0.01f, " s",
            [&state]{ return state.boop_coalesce_window_s; },
            [&state, boop_sensor_pp](float v){
                state.boop_coalesce_window_s = v;
                if (auto* s = boop_sensor_pp ? *boop_sensor_pp : nullptr)
                    s->set_coalesce_window_s(static_cast<double>(v));
            }),
            "When left and right cheeks both land touch events within this "
            "window, both single-cheek events are suppressed and a Both "
            "Cheeks event fires instead. Set to 0 to disable coalescing "
            "(single-side events fire immediately)."),
    };
    face_display_menu.push_back(
        with_desc(submenu("Boop", std::move(boop_menu)),
                  "Per-zone capacitive-touch reactions. Drives "
                  "Protoface's trigger_boop() — fires the chosen expression "
                  "when the snout, a cheek pad, or both cheeks together are "
                  "touched."));

    // ── Voice → mouth_open driver ────────────────────────────────────────────
    // Sliders write through to the live analyzer so the next FFT cycle picks
    // up new values; state.voice_mouth mirrors them so mutate_cfg can persist
    // to config.json on exit. Disabled by default — flip Enabled to start
    // analysing (or set "voice_mouth.enabled":true in config.json).
    auto voice_apply_band = [voice_analyzer, &state]() {
        if (voice_analyzer)
            voice_analyzer->set_band(state.voice_mouth.band_lo_hz,
                                     state.voice_mouth.band_hi_hz);
    };
    std::vector<MenuItem> voice_menu = {
        toggle("Enabled",
            [&state]{ return state.voice_mouth.enabled; },
            [&state, voice_analyzer](bool v){
                state.voice_mouth.enabled = v;
                if (voice_analyzer) voice_analyzer->set_enabled(v);
            }),
        with_desc(slider("Sensitivity", 0.25f, 6.f, 0.05f, "x",
            [&state]{ return state.voice_mouth.sensitivity; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.sensitivity = v;
                if (voice_analyzer) voice_analyzer->set_sensitivity(v);
            }),
            "Multiplies the speech-band RMS before the gate / clip to 1.0. "
            "Raise if the mouth barely opens during normal speech."),
        with_desc(slider("Noise Gate", 0.f, 0.2f, 0.005f, "",
            [&state]{ return state.voice_mouth.noise_gate; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.noise_gate = v;
                if (voice_analyzer) voice_analyzer->set_noise_gate(v);
            }),
            "Band RMS below this floor reads as silence. Raise to ignore "
            "fan / motor / background noise."),
        with_desc(slider("Attack", 5.f, 150.f, 5.f, " ms",
            [&state]{ return state.voice_mouth.attack_ms; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.attack_ms = v;
                if (voice_analyzer) voice_analyzer->set_attack_ms(v);
            }),
            "Time constant for opening the mouth. Lower feels snappier; "
            "higher smooths over transients."),
        with_desc(slider("Release", 30.f, 600.f, 10.f, " ms",
            [&state]{ return state.voice_mouth.release_ms; },
            [&state, voice_analyzer](float v){
                state.voice_mouth.release_ms = v;
                if (voice_analyzer) voice_analyzer->set_release_ms(v);
            }),
            "Time constant for closing. Higher = mouth lingers open between "
            "syllables (less stuttery)."),
        with_desc(slider("Band Low", 40.f, 1200.f, 10.f, " Hz",
            [&state]{ return state.voice_mouth.band_lo_hz; },
            [&state, voice_apply_band](float v){
                state.voice_mouth.band_lo_hz = v;
                voice_apply_band();
            }),
            "Lower edge of the analysis band. Raise to cut low-frequency "
            "rumble; lower if your voice's fundamentals are getting clipped."),
        with_desc(slider("Band High", 1000.f, 8000.f, 100.f, " Hz",
            [&state]{ return state.voice_mouth.band_hi_hz; },
            [&state, voice_apply_band](float v){
                state.voice_mouth.band_hi_hz = v;
                voice_apply_band();
            }),
            "Upper edge. Lower if hiss / sibilants are over-driving the mouth."),

        // ── Visemes (multi-shape mouth) ─────────────────────────────────
        with_desc(toggle("Visemes Enabled",
            [&state]{ return state.voice_mouth.visemes_enabled; },
            [&state, voice_analyzer](bool v){
                state.voice_mouth.visemes_enabled = v;
                if (voice_analyzer) voice_analyzer->set_visemes_enabled(v);
            }),
            "Switch the mouth overlay between mouth_open/_small/_smile/_round "
            "based on the analyzer's spectral centroid. Falls back to "
            "mouth_open when off (matches the pre-viseme behaviour)."),
    };

    // Shared updater for the three viseme thresholds.
    auto voice_apply_visemes = [voice_analyzer, &state]() {
        if (voice_analyzer)
            voice_analyzer->set_viseme_thresholds(
                state.voice_mouth.viseme_round_max_hz,
                state.voice_mouth.viseme_open_max_hz,
                state.voice_mouth.viseme_small_max_hz);
    };
    voice_menu.push_back(with_desc(slider("Round → Open", 100.f, 1500.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_round_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_round_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is the OOH (round) viseme; above, AH (open)."));
    voice_menu.push_back(with_desc(slider("Open → Small", 800.f, 2500.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_open_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_open_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is AH; above, the M/N small-open shape."));
    voice_menu.push_back(with_desc(slider("Small → Smile", 1500.f, 4000.f, 25.f, " Hz",
        [&state]{ return state.voice_mouth.viseme_small_max_hz; },
        [&state, voice_apply_visemes](float v){
            state.voice_mouth.viseme_small_max_hz = v;
            voice_apply_visemes();
        }),
        "Centroid below this is the small-open shape; above, EE (smile)."));
    face_display_menu.push_back(
        with_desc(submenu("Voice", std::move(voice_menu)),
                  "Mic-driven mouth_open. FFT-based: speech-band RMS feeds an "
                  "envelope follower whose output drives face::set_audio() each "
                  "audio period (~5 ms)."));

    // ── Accessory LEDs (cheekhubs + fins) ────────────────────────────────────
    // Per-zone pattern / color / breathe rate live in the AccessoryLeds
    // manager's atomic-snapshotted config; the menu's setters write through
    // so the next render tick uses the new values. Brightness is global to
    // the whole chain. Pattern picker exposes Off / Solid / Breathe / Level
    // — Flash is reserved for event-driven overlays (boop hooks) only.
    auto led_zone_menu = [&, leds](accessory::Zone z, std::string label) -> MenuItem {
        // Pattern picker — radio-style leaf_sel set, one per pattern.
        struct PatOpt { const char* label; accessory::Pattern pat; };
        const PatOpt opts[] = {
            { "Off",     accessory::Pattern::Off     },
            { "Solid",   accessory::Pattern::Solid   },
            { "Breathe", accessory::Pattern::Breathe },
            { "Level",   accessory::Pattern::Level   },
        };
        std::vector<MenuItem> pat_items;
        for (const auto& o : opts) {
            const accessory::Pattern p = o.pat;
            pat_items.push_back(leaf_sel(o.label,
                [leds, z, p]{ if (leds) leds->set_zone_pattern(z, p); },
                [leds, z, p]{
                    return leds && leds->zone(z).pattern == p;
                }));
        }

        std::vector<MenuItem> items = {
            with_desc(submenu("Pattern", std::move(pat_items)),
                      "What the zone does each tick. Level uses the mic "
                      "volume; Breathe pulses at its own rate; Solid is a "
                      "steady colour. Boop events flash on top regardless."),
            color_picker("Color",
                [leds, z](uint8_t r, uint8_t g, uint8_t b) {
                    if (leds) leds->set_zone_color(z, r, g, b);
                },
                [leds, z]() -> std::tuple<uint8_t, uint8_t, uint8_t> {
                    if (!leds) return {0, 0, 0};
                    auto zc = leds->zone(z);
                    return {zc.r, zc.g, zc.b};
                }),
            with_desc(slider("Breathe Rate", 0.05f, 5.f, 0.05f, " Hz",
                [leds, z]{ return leds ? leds->zone(z).breathe_hz : 0.5f; },
                [leds, z](float v){ if (leds) leds->set_zone_breathe_hz(z, v); }),
                "How fast the Breathe pattern oscillates. Only meaningful "
                "when Pattern is Breathe."),
            leaf("Test Flash", [leds, z]{ if (leds) leds->trigger_flash(z, 0.35); }),
        };
        return submenu(std::move(label), std::move(items));
    };

    std::vector<MenuItem> led_menu = {
        with_desc(slider("Brightness", 0.f, 255.f, 1.f, "",
            [leds]{ return leds ? static_cast<float>(leds->global_brightness()) : 0.f; },
            [leds](float v){ if (leds) leds->set_global_brightness(static_cast<uint8_t>(v)); }),
            "Master brightness applied to the whole accessory chain at SPI "
            "encode time. WS2812s draw a lot of current at full white — "
            "keep this low (~64) unless you have power injection."),
        led_zone_menu(accessory::Zone::LeftCheekhub,  "Left Cheekhub"),
        led_zone_menu(accessory::Zone::RightCheekhub, "Right Cheekhub"),
        led_zone_menu(accessory::Zone::LeftFin,       "Left Fin"),
        led_zone_menu(accessory::Zone::RightFin,      "Right Fin"),
    };
    face_display_menu.push_back(
        with_desc(submenu("LEDs", std::move(led_menu)),
                  "Accessory WS2812 strip (cheekhubs + fins). Driven via "
                  "SPI MOSI; boops flash the matching zone, mic volume "
                  "drives Level zones."));

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
        { "Auto (BNO055 > MPU9250 > Viture)", AppState::ImuSource::Auto    },
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

    std::vector<MenuItem> compass_menu = {
        toggle("Compass Tape",
            [&state]{ return state.compass_tape; },
            [&state](bool v){ state.compass_tape = v; }),
        with_desc(submenu("IMU Source", std::move(imu_source_menu)),
                  "Which sensor drives the HUD compass. Auto walks "
                  "BNO055 > MPU9250 > Viture and picks the highest-priority "
                  "fresh source each frame; explicit choices force their "
                  "source even if stale."),
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

    // Built-in coordinated themes — reusable so the same quick-apply list can live
    // under HUD (with Effects) and under System → HUD/Menu Presets.
    auto make_builtin_theme_leaves = [apply_theme, leaf]() -> std::vector<MenuItem> {
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
        toggle("Circle Window",
            [&state]{ return state.map_overlay.circle_window; },
            [&state](bool v){ state.map_overlay.circle_window = v; }),
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
        leaf("Expand Map (Pan/Zoom)", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            state.map_overlay.expanded   = true;
            state.map_overlay.view_zoom  = 1.f;
            state.map_overlay.view_pan_x = 0.f;
            state.map_overlay.view_pan_y = 0.f;
        }),
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
    std::vector<MenuItem> mini_map_menu = {
        submenu("Module Controls", std::move(module_controls_menu)),
        submenu("Map Options",     std::move(map_options_menu)),
    };

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

    std::vector<MenuItem> hud_menu = {
        toggle("Flip to Top",
            [hud_cfg]{ return hud_cfg->hud_flip_vertical; },
            [hud_cfg](bool v){ hud_cfg->hud_flip_vertical = v; }),
        submenu("Location",         std::move(location_menu)),
        submenu("Mini-Map Module",  std::move(mini_map_menu)),
        submenu("Info-Panel Module",std::move(info_panel_menu)),
        submenu("Compass",          std::move(compass_menu)),
        submenu("Clock",            std::move(clock_menu)),
        submenu("Color",            std::move(color_options_menu)),
        submenu("Menu Position",    std::move(menu_position_menu)),
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

    // ── HUD / Menu Presets ───────────────────────────────────────────────────────
    // Visual-only presets (HUD colors + menu accent/border/selection/scale). Built-in
    // coordinated themes apply instantly; custom ones are saved/loaded live (no
    // restart, unlike full Profiles). Load/delete lists are dynamic (label_fn reads
    // the manager live). kProfileSlots = max dynamic rows shown for save/load lists.
    constexpr int kProfileSlots = 16;
    std::vector<MenuItem> hud_preset_delete_menu;
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.visible_fn  = [hud_presets, i]{ return hud_presets && i < hud_presets->count(); };
        m.description = "Delete this HUD/menu preset permanently.";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_delete_name = nm;
        };
        hud_preset_delete_menu.push_back(std::move(m));
    }

    std::vector<MenuItem> hud_presets_menu;
    // Built-in coordinated themes (instant apply).
    for (auto& t : make_builtin_theme_leaves())
        hud_presets_menu.push_back(with_desc(std::move(t),
            "Built-in theme: applies a coordinated HUD color + menu style instantly."));
    // Save current look as a named preset (on-screen keyboard).
    hud_presets_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Preset Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->hud_preset_save_name = name;
                });
        }),
        "Save the current HUD colors + menu style (accent, border, selection, UI "
        "scale) as a named preset. Applies live when loaded — no restart."));
    // Dynamic load slots.
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "preset";
        m.label_fn    = [hud_presets, i]{ return hud_presets ? hud_presets->name(i) : std::string(); };
        m.visible_fn  = [hud_presets, i]{ return hud_presets && i < hud_presets->count(); };
        m.description = "Apply this HUD/menu preset (live, no restart).";
        m.action = [state_ptr, hud_presets, i]{
            if (!state_ptr || !hud_presets) return;
            std::string nm = hud_presets->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->hud_preset_load_name = nm;
        };
        hud_presets_menu.push_back(std::move(m));
    }
    hud_presets_menu.push_back(with_desc(submenu("Delete Preset", std::move(hud_preset_delete_menu)),
        "Remove a saved HUD/menu preset permanently."));

    std::vector<MenuItem> system_menu = {
        submenu("Headset & Tracking", std::move(headset_menu)),
        with_desc(submenu("HUD / Menu Presets", std::move(hud_presets_menu)),
                  "Visual presets: built-in themes plus your own saved HUD color + "
                  "menu style combinations. Applied live."),
        with_desc(toggle("Radial Quick Menu",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp
                              && (*menu_sys_pp)->quick_style() == QuickStyle::Radial; },
            [menu_sys_pp](bool v){ if (menu_sys_pp && *menu_sys_pp)
                (*menu_sys_pp)->set_quick_style(v ? QuickStyle::Radial : QuickStyle::List); }),
            "ON: the quick menu is a radial wheel encircling the minimap. OFF: the "
            "legacy compact corner list. The minimap's position (HUD > Map) sets where "
            "the wheel sits."),
        with_desc(slider("Menu Tilt", 0.f, 0.8f, 0.05f, "",
            [menu_sys_pp]{ return menu_sys_pp && *menu_sys_pp ? (*menu_sys_pp)->radial_tilt() : 0.f; },
            [menu_sys_pp](float v){ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->set_radial_tilt(v); }),
            "Helmet-style inward perspective tilt for the radial menu. 0 = flat on "
            "the glass; higher = the wheel curves away at the top like a visor."),
        with_desc(toggle("Fullscreen",
            [&state]{ return state.win_fullscreen.load(); },
            [&state](bool v){ state.win_fullscreen.store(v); state.win_mode_dirty.store(true); }),
            "Borderless fullscreen covering the whole screen. Desktop/dev only — "
            "ignored while the glasses are connected. Applied live."),
        with_desc(toggle("Frameless Window",
            [&state]{ return state.win_frameless.load(); },
            [&state](bool v){ state.win_frameless.store(v); state.win_mode_dirty.store(true); }),
            "Remove the OS window title bar and borders (windowed mode). Desktop/dev "
            "only. Applied live."),
        with_desc(toggle("Legacy HUD",
            [&state]{ return state.legacy_hud; },
            [&state](bool v){ state.legacy_hud = v; }),
            "ON: show the legacy edge/corner HUD (compass tape, health indicators, "
            "face indicator, corner clock/timer, LoRa messages). OFF: show only the "
            "modular HUD — the minimap and info panel."),
        with_desc(toggle("Skip Startup Screen",
            [&state]{ return state.skip_landing; },
            [&state](bool v){ state.skip_landing = v; }),
            "Bypass the profile/continue landing screen at boot and run the current "
            "config directly. Takes effect on the next launch."),
        with_panel(
            submenu("Audio", std::move(audio_menu)),
            "Audio Status",
            [&state, menu_sys_pp]
            (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                const ImU32 accent = (menu_sys_pp && *menu_sys_pp)
                    ? (*menu_sys_pp)->accent_color()
                    : IM_COL32(255, 255, 255, 255);
                const float lh = 18.f;
                ImVec2 p = o;
                auto row = [&](const char* k, const char* v, ImU32 vc) {
                    dl->AddText({ p.x, p.y }, IM_COL32(180, 180, 180, 200), k);
                    dl->AddText({ p.x + 130.f, p.y }, vc, v);
                    p.y += lh;
                };

                bool enabled;
                int  out_idx;
                float cpu;
                int  xruns;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    enabled = state.audio.enabled;
                    out_idx = state.audio.output;
                    cpu     = state.audio.cpu_load;
                    xruns   = state.audio.xrun_count;
                }
                const char* out_name =
                    out_idx == 0 ? "VITURE" :
                    out_idx == 1 ? "Headphones" :
                    out_idx == 2 ? "HDMI" : "?";

                row("State",  enabled ? "Enabled" : "Muted",
                    enabled ? ((accent & 0x00FFFFFFu) | (220u << 24))
                            : IM_COL32(200, 90, 90, 220));
                row("Output", out_name, IM_COL32(230, 230, 230, 230));
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f %%", cpu * 100.f);
                row("CPU load", buf, IM_COL32(230, 230, 230, 230));
                snprintf(buf, sizeof(buf), "%d", xruns);
                row("XRuns", buf,
                    xruns == 0 ? IM_COL32(160, 220, 160, 220)
                               : IM_COL32(220, 160, 90, 220));
            }),
        submenu("Timers and Alarm", std::move(timers_alarm_menu)),
        slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
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
        leaf("Reboot System", [&state] {
            state.quit = true;
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                std::system("reboot");
            }).detach();
        }),
        leaf("Close Program",  [&state]{ state.quit = true; }),
    };

    // ── Profiles ────────────────────────────────────────────────────────────────
    // Save the current setup as a named snapshot, load one (relaunches ProtoHUD),
    // or delete one. The load/delete lists are dynamic: label_fn/visible_fn read the
    // ProfileManager live, so newly-saved profiles appear without rebuilding the menu.
    // (kProfileSlots declared above, near the HUD/Menu Presets section.)
    std::vector<MenuItem> profile_delete_menu;
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type       = MenuItemType::LEAF;
        m.label      = "profile";
        m.label_fn   = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.visible_fn = [profiles, i]{ return profiles && i < profiles->count(); };
        m.description = "Delete this profile permanently.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_delete_name = nm;
        };
        profile_delete_menu.push_back(std::move(m));
    }

    std::vector<MenuItem> profiles_menu;
    profiles_menu.push_back(with_desc(
        leaf("Save Current As...", [menu_sys_pp, state_ptr]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Profile Name", std::string(),
                [state_ptr](const std::string& name){
                    if (!state_ptr) return;
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->profile_save_name = name;
                });
        }),
        "Save every current setting (HUD layout, menu style, camera/vision, "
        "Protoface look) as a named profile you can switch to later."));
    for (int i = 0; i < kProfileSlots; ++i) {
        MenuItem m;
        m.type        = MenuItemType::LEAF;
        m.label       = "profile";
        m.label_fn    = [profiles, i]{ return profiles ? profiles->name(i) : std::string(); };
        m.visible_fn  = [profiles, i]{ return profiles && i < profiles->count(); };
        m.description = "Load this profile. ProtoHUD restarts to apply it.";
        m.action = [state_ptr, profiles, i]{
            if (!state_ptr || !profiles) return;
            std::string nm = profiles->name(i);
            if (nm.empty()) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            state_ptr->profile_load_name = nm;
        };
        profiles_menu.push_back(std::move(m));
    }
    profiles_menu.push_back(with_desc(submenu("Delete Profile", std::move(profile_delete_menu)),
        "Remove a saved profile permanently."));

    // ── Quick (corner / radial) menu ─────────────────────────────────────────────
    // A short, curated set of mid-use actions — separate from the full settings tree
    // above. Some items are always present; an optional "catalog" of extras can be
    // pinned by the user (favorites, persisted in config) and is gated by visible_fn.
    // Submenus here become outer rings in the radial renderer.
    if (quick_out) {
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

        *quick_out = std::move(q);
    }

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
        with_desc(submenu("LoRa",         std::move(lora_menu)),
                  "Long-range radio: team nodes, messages and status."),
        with_desc(submenu("System",       std::move(system_menu)),
                  "Audio output and volume, timers/alarms, status and power."),
        with_desc(submenu("Profiles",     std::move(profiles_menu)),
                  "Save, load and manage full-setup profiles. Loading one restarts "
                  "ProtoHUD with that profile."),
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
    xr_cfg.sbs_height       = jval(jdisp, "sbs_height",       1080);
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
        return EyeSource::CSI;
    };
    EyeSource left_eye_src  = parse_eye_src(jcam.value("left_eye_source",  std::string("csi")));
    EyeSource right_eye_src = parse_eye_src(jcam.value("right_eye_source", std::string("csi")));

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
    }

    // IMU source selector — replaces the old "Viture always wins, MPU is
    // backup" hardcoded priority. "auto" picks the best fresh source per
    // frame (BNO055 > MPU9250 > Viture); explicit choices force that
    // source even if others are connected.
    if (cfg.contains("imu_source")) {
        const std::string s = cfg.value("imu_source", std::string("auto"));
        if      (s == "bno055" || s == "bno")     state.imu_source = AppState::ImuSource::Bno055;
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
                boop_cfg.electrode[i] =
                    static_cast<int8_t>(jval(jz, "electrode", static_cast<int>(boop_cfg.electrode[i])));
                state.boop_zones[i].expression =
                    jz.value("expression", state.boop_zones[i].expression);
                state.boop_zones[i].duration_s =
                    jval(jz, "duration_s", state.boop_zones[i].duration_s);
                state.boop_zones[i].threshold =
                    static_cast<uint8_t>(jval(jz, "threshold", static_cast<int>(state.boop_zones[i].threshold)));
                state.boop_zones[i].enabled =
                    jval(jz, "enabled", state.boop_zones[i].enabled);
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
    if (cfg.contains("protoface")) {
        auto& jpf = cfg["protoface"];
        pf_autostart     = jval(jpf, "autostart", true);
        pf_mode          = jpf.value("mode", std::string("daemon"));
        pf_launch_driver = jval(jpf, "panel_driver", true);
        pf_backend       = jpf.value("backend", std::string("hub75"));
        if (jpf.contains("layout") && jpf["layout"].is_object()) {
            auto& jl = jpf["layout"];
            pf_eye_layout   = jl.value("eye",   pf_eye_layout);
            pf_mouth_layout = jl.value("mouth", pf_mouth_layout);
            pf_nose_layout  = jl.value("nose",  pf_nose_layout);
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
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_xr_imu_us = now_us;
        std::lock_guard<std::mutex> lk(state.mtx);
        state.imu_pose = { roll, pitch, yaw };

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
        const float fs = std::sinf(prev * kDeg2Rad) + kAlpha * (std::sinf(heading * kDeg2Rad) - std::sinf(prev * kDeg2Rad));
        const float fc = std::cosf(prev * kDeg2Rad) + kAlpha * (std::cosf(heading * kDeg2Rad) - std::cosf(prev * kDeg2Rad));
        float filtered = std::atan2f(fs, fc) / kDeg2Rad;
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
    if (!bno055.start() && bno_cfg.enabled)
        std::cerr << "[main] BNO055 9-DOF IMU unavailable\n";

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

    boop_sensor.on_boop([&face_proxy, &state, &accessory_leds, boop_face_stem]
                        (sensor::BoopSensor::Zone z) {
        const auto zi = static_cast<size_t>(z);
        if (zi >= 4) return;
        // Snapshot under the state lock so a menu edit mid-boop can't tear
        // the std::string read.
        bool        enabled;
        std::string fallback_expr;
        double      duration_s;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            enabled       = state.boop_zones[zi].enabled;
            fallback_expr = state.boop_zones[zi].expression;
            duration_s    = state.boop_zones[zi].duration_s;
        }
        if (!enabled) return;
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

        // Accessory LED flash overlay. Snout = both fins, single cheek =
        // matching cheekhub, both cheeks = all four zones flash together
        // (matches the "surprise" reaction's broader visual feedback).
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
    });
    if (boop_cfg.enabled && !boop_sensor.start())
        std::cerr << "[main] boop sensor (MPR121) unavailable\n";
    boop_sensor_ptr = &boop_sensor;   // expose for the menu's live tuning

    if (led_cfg.enabled && !accessory_leds.start())
        std::cerr << "[main] accessory LEDs unavailable — continuing without\n";

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
    // SmartKnob ESP32-S3 DevKitC-1 UART port: CH341 VID=0x1A86, PID=0x7522.
    teensy_port = resolve_serial_port(teensy_port, 0x16C0, 0x0483);
    lora_port   = resolve_serial_port(lora_port,   0x1A86, 0x7523);
    knob_port   = resolve_serial_port(knob_port,   0x1A86, 0x7522);

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
        face::RenderConfig rc = pf_build_render_config(cfg);
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
                const int mx = pf_mirror_x(pf_nose_layout, rc.canvas_w);
                const int face_w = std::min(rc.canvas_w, std::max(8, 2 * mx));
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
            rc, pf_build_panel_output(cfg, rc));
        native_ctrl->start();
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
            pf_launch_panel_driver(bin_dir, rc.canvas_w, rc.canvas_h);
        }
    } else {
        // Auto-start the Protoface daemon on boot (no-op if already running). The
        // reconnect loop in start() then connects once its socket comes up.
        if (pf_autostart) protoface_ctrl.launch();
        protoface_ctrl.start();   // connects async; no-op if socket absent
    }

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

        face::RenderConfig rc = pf_build_render_config(cfg);
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
                const int mx = pf_mirror_x(pf_nose_layout, rc.canvas_w);
                const int face_w = std::min(rc.canvas_w, std::max(8, 2 * mx));
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
        auto new_output = pf_build_panel_output(cfg, rc);
        native_ctrl = std::make_unique<face::NativeFaceController>(
            rc, std::move(new_output));
        active_face = native_ctrl.get();
        native_ctrl->start();

        // panel_driver.py choreography. The Python shim is only needed for
        // HUB75 (it reads /dev/shm frames and pushes them via piomatter);
        // both MAX7219 and RGB matrix backends drive spidev directly. Kill
        // on either of those — safe even if it wasn't running.
        if (new_backend == "max7219" || new_backend == "rgb_matrix") {
            std::system("pkill -f panel_driver.py 2>/dev/null");
        } else if (new_backend == "hub75" && pf_launch_driver) {
            pf_launch_panel_driver(bin_dir, rc.canvas_w, rc.canvas_h);
        }
    };

    // Edit… launcher used by Files > Faces > <slot> > Edit and Mouth Shapes
    // > <slot> > Edit. Only meaningful when the active backend has covered
    // regions (MAX7219 or RGB matrix today) — for HUB75 / daemon mode the
    // menu's visible_fn hides the leaf so this never runs.
    auto edit_face = [&](const std::string& expression) {
        if (!native_ctrl || !menu_ptr) return;
        const int cw = native_ctrl->canvas_width();
        const int ch = native_ctrl->canvas_height();
        // The Chain Layout pickers are the source of truth for the
        // editor's per-zone bounding boxes (Left/Right Eye, Nose, Mouth
        // halves). The helper also returns the canvas mirror axis (nose
        // centre, or canvas_w/2 when there's no nose) so the editor's
        // mirror brush respects the face's actual symmetry line.
        auto zones = pf_compute_face_zones(pf_eye_layout,
                                           pf_mouth_layout,
                                           pf_nose_layout,
                                           cw, ch);
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

        const menu::FaceEditor::Mode mode =
            (pf_backend == "rgb_matrix") ? menu::FaceEditor::Mode::Color
                                         : menu::FaceEditor::Mode::Mono;

        char title[96];
        std::snprintf(title, sizeof(title),
                      "Edit face: %s  (%s)",
                      expression.c_str(), pf_backend.c_str());
        menu_ptr->open_face_editor(
            title, abs_path, cw, ch, std::move(covered), std::move(labels),
            zones.mirror_x,
            mode, {} /* default palette */,
            /* on_commit */ [&face_proxy, &native_ctrl, expression]
                (const cv::Mat& rgba_canvas, const std::string& target_path) {
                // Convert RGBA back to BGRA for cv::imwrite (PNG storage
                // expects native channel order in OpenCV).
                cv::Mat bgra;
                cv::cvtColor(rgba_canvas, bgra, cv::COLOR_RGBA2BGRA);
                std::error_code ec;
                std::filesystem::create_directories(
                    std::filesystem::path(target_path).parent_path(), ec);
                if (!cv::imwrite(target_path, bgra)) {
                    std::fprintf(stderr, "[editor] save failed: %s\n",
                                 target_path.c_str());
                    return;
                }
                // Rebuild the face loader so the new PNG shows up immediately
                // — then pop the saved expression on-face so the user sees
                // their work without leaving the menu. set_face_by_name
                // falls back to neutral gracefully when the name isn't an
                // expression in the loader's set (mouth-shape PNGs).
                if (native_ctrl) native_ctrl->reload_active_face();
                face_proxy.set_face_by_name(expression);
            });
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
    std::string cfg_bg_user_dir;
    if (const char* home = std::getenv("HOME"))
        cfg_bg_user_dir = std::string(home) + "/protohud/backgrounds";
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
                               // In native mode hide the daemon's Start/Restart +
                               // source-switch menu items (null fp_option).
                               native_ctrl ? nullptr
                                           : static_cast<IFaceController*>(&protoface_ctrl),
                               &panel_preview_enabled,
                               &protoface_preview_cfg,
                               &protoface_preview_view,
                               cfg_map_dir,
                               &left_eye_src, &right_eye_src,
                               &profiles, &hud_presets, &quick_items,
                               cfg_gifs_dir,
                               &bg_lib_ptr, cfg_bg_user_dir,
                               &boop_sensor_ptr,
                               audio.voice(),
                               &accessory_leds,
                               swap_backend, &pf_backend,
                               edit_face,
                               &pf_eye_layout, &pf_mouth_layout, &pf_nose_layout));
    menu_ptr = &menu;
    menu.set_quick_items(std::move(quick_items));

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

    knob.on_move([&menu, &hud, &landing, &landing_nav](int8_t dir, int) {
        // if (hud.popup_active())    hud.popup_navigate(dir);  // modal popup disabled
        if      (menu.is_keyboard_open()) menu.osk_step(dir);
        else if (landing.active)        landing_nav(dir);
        else if (menu.is_open())        menu.navigate(dir);
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

    knob.on_button([&menu, &hud, &state, &landing, &landing_select](uint8_t btn, uint8_t ev) {
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
            buttons.on_select([&menu, &hud, &state]() {
                if      (menu.is_open())            menu.select();
                else if (hud.toast_has_focused())   hud.toast_select(state);
                else                                menu.open();   // short press opens menu when idle
            });
            buttons.on_back([&menu]() {
                if (menu.is_open()) menu.back();
            });
        } else {
            std::cerr << "[main] GPIO button init failed\n";
        }
    }

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
        if      (state.map_overlay.expanded) map_pan(+0.06f, 0);
        else if (menu.is_keyboard_open()) menu.osk_move(-1, 0);
        else if (landing.active)          bg_lib.prev();
        else if (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_right([&menu, &hud, &state, &landing, &bg_lib, &map_pan]{
        if      (state.map_overlay.expanded) map_pan(-0.06f, 0);
        else if (menu.is_keyboard_open()) menu.osk_move(+1, 0);
        else if (landing.active)          bg_lib.next();
        else if (hud.toast_has_focused()) hud.toast_navigate(+1);
        else if (menu.is_open())          menu.select();
    });
    // LB/RB: zoom the expanded map, else cycle the landing background, switch
    // deep-menu tabs while it's open, otherwise toggle the PiPs.
    gamepad.on_pip_left ([&menu, &kb_pip_left, &landing, &bg_lib, &state, &map_zoom] {
        if      (state.map_overlay.expanded) map_zoom(-0.4f);
        else if (landing.active)        bg_lib.prev();
        else if (menu.is_deep_open())   menu.prev_tab();
        else                            kb_pip_left  = !kb_pip_left;
    });
    gamepad.on_pip_right([&menu, &kb_pip_right, &landing, &bg_lib, &state, &map_zoom]{
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

    // ── Signal handling: Ctrl+C / SIGTERM → graceful quit + 5s force-kill ──────
    {
        static std::atomic<bool>* g_quit = &state.quit;
        auto handler = [](int) {
            if (g_quit) g_quit->store(true);
            // If cleanup stalls, force-exit after 5 seconds.  Exit 0, not 1:
            // SIGINT/SIGTERM is an explicit quit request (Ctrl+C, `kill`, or the
            // watchdog forwarding a stop), so report a clean exit — otherwise the
            // respawn supervisor (scripts/watchdog.sh) treats the non-zero code as
            // a crash and relaunches the program.
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::cerr << "[signal] cleanup timed out — forcing clean exit\n";
                std::_Exit(0);
            }).detach();
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
            }
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
        cfg["protoface"]["layout"]["eye"]       = pf_eye_layout;
        cfg["protoface"]["layout"]["mouth"]     = pf_mouth_layout;
        cfg["protoface"]["layout"]["nose"]      = pf_nose_layout;
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

        cfg["resolution"]["width"]  = state.camera_resolution.width;
        cfg["resolution"]["height"] = state.camera_resolution.height;
        cfg["resolution"]["fps"]    = state.camera_resolution.fps;

        auto eye_src_str = [](EyeSource s) -> const char* {
            switch (s) {
                case EyeSource::USB1: return "usb1";
                case EyeSource::USB2: return "usb2";
                case EyeSource::USB3: return "usb3";
                default:              return "csi";
            }
        };
        cfg["cameras"]["left_eye_source"]  = eye_src_str(left_eye_src);
        cfg["cameras"]["right_eye_source"] = eye_src_str(right_eye_src);

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

    while (!glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {
        wd_heartbeat.fetch_add(1, std::memory_order_relaxed);

        // Apply a pending window-mode change (Settings > Fullscreen / Frameless).
        if (state.win_mode_dirty.exchange(false))
            xr.apply_window_mode(state.win_fullscreen.load(), state.win_frameless.load());

        // ── Delta time ────────────────────────────────────────────────────────
        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - prev_time);
        prev_time  = now;

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
        } else {
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
            // still work (paint / cancel).
            const bool editor_up = menu.is_face_editor_open();
            if (key_pressed(ImGuiKey_Enter) ||
                (!editor_up && key_pressed(ImGuiKey_RightArrow))) menu.select();
            if (key_pressed(ImGuiKey_Backspace) ||
                (!editor_up && key_pressed(ImGuiKey_LeftArrow)))  menu.back();
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
            // 0: toggle manual/auto focus
            if (edge(0, GLFW_KEY_0) && !menu.is_open() && !face_mod) {
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
            if (edge(4, GLFW_KEY_4) && !face_mod) {
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
            float cam_ar  = (float)snap.camera_resolution.width
                          / snap.camera_resolution.height;
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
                // Beast passthrough — TODO: fullscreen blit once Beast path is ported
                drew = false;
            }
            if (!drew) {
                if (left_eye_src == EyeSource::CSI)
                    drew = snap.cameras_swapped
                        ? cameras.draw_owl_right(snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y)
                        : cameras.draw_owl_left (snap.zoom_left.zoom, snap.zoom_left.center_x, snap.zoom_left.center_y);
                else
                    drew = cameras.draw_tex_fullscreen(usb_tex_for(left_eye_src));
            }

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
                drew = false;
            }
            if (!drew) {
                if (right_eye_src == EyeSource::CSI)
                    drew = snap.cameras_swapped
                        ? cameras.draw_owl_left (snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y)
                        : cameras.draw_owl_right(snap.zoom_right.zoom, snap.zoom_right.center_x, snap.zoom_right.center_y);
                else
                    drew = cameras.draw_tex_fullscreen(usb_tex_for(right_eye_src));
            }

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
        if (snap.capture_request != CaptureRequest::None) {
            do_capture(snap.capture_request, xr, cfg_photo_dir, state);  // resets request
            // Burst: re-arm another stereo shot next frame until the count runs out.
            std::lock_guard<std::mutex> lk(state.mtx);
            if (state.capture_burst > 0) {
                --state.capture_burst;
                state.capture_request = CaptureRequest::Stereo;
            }
        }

        // ── Video recording ───────────────────────────────────────────────────
        // Same clean eye-FBO source as photos; encodes on a worker thread.
        video_recorder.tick(xr, state, cfg_video);

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
        hud.begin_nvg_overlay(xr.display_width(), xr.display_height());
        hud.draw_pip_underlays(tex_usb1, p1, pip_overlay_cfg1,
                               tex_usb2, p2, pip_overlay_cfg2,
                               tex_usb3, p3, pip_overlay_cfg3,
                               xr.display_width(), xr.display_height());
        hud.draw_hud_frame(snap, xr.display_width(), xr.display_height(), fps_overlay_active);
        hud.draw_toasts(state.notifs, xr.display_width(), xr.display_height());
        hud.end_nvg_overlay();

        // ── Phase 2: ImGui overlays (menu, popups) ────────────────────────
        menu.set_glow_enabled(hud.config().glow_enabled);
        if (menu.is_deep_open()) {
            menu.draw_fullscreen(xr.eye_width(), xr.eye_height());
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
            menu.draw_radial(mcx, mcy, half, focus, rotate);
        } else {
            menu.draw(xr.eye_width(), xr.eye_height());
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
                // preview shows a centred face instead of dead space (the
                // canvas is configured wider than the content for HUB75
                // compatibility). Face content runs col 0 to col 2*mirror_x
                // because every zone mirrors around mirror_x; mirror_x
                // matches the editor's mirror axis.
                const int mx     = pf_mirror_x(pf_nose_layout, rgb.cols);
                const int face_w = std::min(rgb.cols, std::max(8, 2 * mx));
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

        if (panel_preview_enabled) {
            const FaceTex ft = pick_face_tex();
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
        if (snap.map_overlay.portrait && !menu.is_open()) {
            const FaceTex ft = pick_face_tex();
            hud.draw_face_portrait(ft.id, ft.w, ft.h, ft.native,
                                   xr.display_width(), xr.display_height(), snap);
        }

        // System status panel (CPU/RAM/WiFi/ping/BT/SSH/perf/serial).
        // Debug panel: normal toggle, plus an option to show it in the expanded-map
        // view. When expanded, it opens to the right of the info sidebar (~310px).
        {
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

        hud.render_menu_overlay();

        // ── Swap ──────────────────────────────────────────────────────────────
        xr.present();
    }

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
