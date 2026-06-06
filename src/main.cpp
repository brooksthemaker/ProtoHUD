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
#include "input/gpio_function.h"
#include "input/gamepad_input.h"
#include "input/wireless_controller.h"
#include "game/game_source.h"
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
// Geometry (panel_w/panel_h/chain/parallel) must match the canvas the renderer
// pushes into /dev/shm, otherwise panel_driver.py's piomatter framebuffer is a
// different shape than the frame it reads and the blit throws on the first
// frame (panels never light). The defaults below mirror panel_driver.py's own
// argparse defaults so existing callers behave unchanged.
static void pf_launch_panel_driver(const std::string& bin_dir,
                                   int canvas_w, int canvas_h,
                                   int panel_w = 64, int panel_h = 32,
                                   int chain = 2, int parallel = 1) {
    std::string drv = bin_dir + "/../scripts/panel_driver.py";
    std::string cw  = std::to_string(canvas_w);
    std::string chh = std::to_string(canvas_h);
    std::string pw  = std::to_string(panel_w);
    std::string ph  = std::to_string(panel_h);
    std::string ch  = std::to_string(chain);
    std::string par = std::to_string(parallel);
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
               static_cast<char*>(nullptr));
        _exit(127);
    }
    std::cout << "[main] launched panel_driver.py pid=" << pid
              << " (" << drv << ", canvas " << cw << "x" << chh
              << ", panel " << pw << "x" << ph
              << ", chain " << ch << ", parallel " << par << ")\n";
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
struct PfSideChains {
    std::vector<std::array<int, 2>> left;
    std::vector<std::array<int, 2>> right;
};
static PfSideChains pf_auto_side_chains(
        const std::string& eye_layout,
        const std::string& mouth_layout,
        const std::string& nose_layout,
        int canvas_h);

// HUB75 panel layout state lives near the top of the file so the early
// pf_build_panel_output / pf_build_render_config can see the type. The
// actual helper definitions live further down (next to the MAX7219
// layout helpers); forward-declare what's needed here.
struct PfHub75Layout {
    // Default size applied to every panel slot. Per-slot overrides in
    // panel_size_per (empty string = "use default") let a build mix
    // sizes — e.g. two 64x32 eye panels plus a 64x64 mouth.
    std::string panel_size      = "64x32";
    std::string arrangement     = "horizontal"; // horizontal / vertical / grid2x2
    int         panel_count     = 1;            // 1..4
    std::string panel_size_per[4] = {"", "", "", ""};
    // Nudge stores each panel's CENTRE as an offset from the canvas centre
    // (in canvas pixels). Default = auto-placed by apply_defaults() per
    // arrangement; e.g. for two 64×32 panels in a horizontal chain the
    // defaults are dx[0] = -32, dx[1] = +32 (P1 left of centre, P2 right).
    int         nudge_dx[4]     = {0, 0, 0, 0};
    int         nudge_dy[4]     = {0, 0, 0, 0};
    // Per-panel orientation flips to match physical mounting/wiring. flip_x
    // mirrors left-right, flip_y top-bottom (both = 180°). Independent of the
    // nudge geometry — applied to each panel's composited region.
    bool        flip_x[4]       = {false, false, false, false};
    bool        flip_y[4]       = {false, false, false, false};
    // First-run flag — until apply_defaults has run we treat all-zero
    // nudges as "uninitialised" and populate them.
    bool        defaults_applied = false;
};
static std::vector<face::ShmPusherOutput::Panel>
pf_hub75_panels(const PfHub75Layout& L);
static void pf_hub75_canvas(const PfHub75Layout& L, int& cw, int& ch);
static void pf_hub75_apply_defaults(PfHub75Layout& L);
static void pf_hub75_driver_geometry(const PfHub75Layout& L,
                                     int& panel_w, int& panel_h,
                                     int& chain, int& parallel);

// ── Custom multi-colour gradient material ───────────────────────────────────
// Editor state for Protoface > Material Color > Custom Gradient. Up to 6 colour
// stops laid out along the face, smoothly blended or hard-banded, optionally
// scrolling so the colours flow behind the face. Serialised to
// cfg["protoface"]["gradient"]; rendered via face::GradientMaterial (the
// "gradient:…" material spec built by pf_gradient_spec below).
struct PfGradient {
    int count = 3;                                   // active stops, 2..6
    std::array<std::array<int, 3>, 6> colors {{
        {{0, 220, 180}}, {{0, 100, 255}}, {{180, 30, 220}},
        {{255, 80, 0}},  {{30, 220, 60}}, {{255, 220, 0}},
    }};
    bool        smooth    = true;                    // blend vs hard bands
    std::string direction = "horizontal";            // horizontal | vertical
    int         speed     = 0;                        // px/s, 0 = static
};

// Build the "gradient:<dir>:<mode>:<speed>:RRGGBB-…" spec face::load_material
// parses. Clamped to 2..6 stops.
static std::string pf_gradient_spec(const PfGradient& g) {
    std::string s = "gradient:";
    s += (g.direction == "vertical") ? "v" : "h";
    s += ':';
    s += g.smooth ? 's' : 'b';
    s += ':';
    s += std::to_string(g.speed);
    s += ':';
    const int n = std::clamp(g.count, 2, 6);
    char buf[8];
    for (int i = 0; i < n; ++i) {
        if (i) s += '-';
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X",
                      g.colors[i][0] & 0xFF, g.colors[i][1] & 0xFF,
                      g.colors[i][2] & 0xFF);
        s += buf;
    }
    return s;
}

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
static std::vector<face::ShmPusherOutput::Panel>
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
static void pf_hub75_apply_defaults(PfHub75Layout& L) {
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
static void pf_hub75_canvas(const PfHub75Layout& L, int& cw, int& ch) {
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

// One layer in the Protoface Effects > Layered Builder. State lives in a
// file-scope struct so kMaxLayers can be a static constexpr (forbidden on a
// local class). The menu's static LayeredEffectState instance persists for
// the program's lifetime.
struct LayerCfg {
    std::string effect = "none";   // "none" disables the slot
    int   count = 20;              // particle count / density
    int   r = 255, g = 255, b = 255;
    float speed_min = 5.f;
    float speed_max = 15.f;
    // Direction of motion, degrees (0 = right, 90 = down, 180 = left,
    // 270 = up). Honoured by snow / rain / embers / confetti; stationary
    // and radial effects ignore it and the slider is hidden in their UI.
    // -1 means "use the effect's historical default."
    float direction_deg = -1.f;
    // Liquid fill fraction for the "water" effect (0..1). Ignored by others.
    float level = 0.4f;
    // Liquid viscosity for "water" (0 = thin/snappy, 1 = thick/sluggish).
    float viscosity = 0.3f;
    // "water" extras: pitch shifts the fill level (look down → liquid rises);
    // bubbles count + style ("rise" bubbles in liquid / "drip" droplets above).
    float pitch_fill = 0.0f;
    int   bubbles = 0;
    std::string bubble_mode = "rise";
    // "lightning" extras: arc mode (crackling arcs vs falling bolts) + fork density.
    bool  arc = false;
    float branches = 0.35f;
    std::string blend = "add";     // "add" | "normal" | "multiply" | "screen"
    // Motion reactivity (opt-in): drives the layer's direction from head
    // movement. "none" | "heading" (lock to compass) | "yaw" (drift when
    // turning) | "tilt" (skew like gravity when rolling the head).
    std::string direction_from = "none";
    // Density reactivity (opt-in): scales particle count from a live signal.
    // "none" | "audio" (mic level) | "yaw_rate" | "accel".
    std::string intensity_from = "none";
};
struct LayeredEffectState {
    static constexpr int kMaxLayers = 5;
    LayerCfg layers[kMaxLayers];
};

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
    {"squint",    "Squint"},
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

// ── Face version helpers ──────────────────────────────────────────────────────
// Saved versions of an expression's PNG live in hidden sibling dirs so the
// FaceLoader (which scans top-level *.png) never lists them as expressions:
//   <folder>/.versions/<expr>/<name>.png   named, kept until deleted
//   <folder>/.history/<expr>/<ts>.png      auto-backups, ring-buffered
namespace fvers {
namespace ffs = std::filesystem;
inline ffs::path vdir(const std::string& expr_png, const char* kind) {
    ffs::path p(expr_png);
    return p.parent_path() / kind / p.stem();
}
inline std::string stamp() {
    char b[32]; time_t t = time(nullptr);
    strftime(b, sizeof(b), "%Y%m%d-%H%M%S", localtime(&t));
    static int seq = 0;
    char s[44]; std::snprintf(s, sizeof(s), "%s-%03d", b, (seq++) % 1000);
    return s;
}
// Copy the live PNG into .history before it's overwritten; prune to `keep` newest.
inline void backup_current(const std::string& expr_png, int keep) {
    std::error_code ec;
    if (!ffs::exists(expr_png, ec)) return;
    ffs::path d = vdir(expr_png, ".history");
    ffs::create_directories(d, ec);
    ffs::copy_file(expr_png, d / (stamp() + ".png"),
                   ffs::copy_options::overwrite_existing, ec);
    std::vector<ffs::path> f;
    for (auto& e : ffs::directory_iterator(d, ec))
        if (e.path().extension() == ".png") f.push_back(e.path());
    std::sort(f.begin(), f.end());   // names are timestamps → chronological
    for (int i = 0; i + keep < static_cast<int>(f.size()); ++i) ffs::remove(f[i], ec);
}
inline bool save_named(const std::string& expr_png, std::string name) {
    std::error_code ec;
    if (!ffs::exists(expr_png, ec)) return false;
    for (auto& c : name) if (c == '/' || c == '\\' || c == ':') c = '_';
    if (name.empty()) return false;
    ffs::path d = vdir(expr_png, ".versions");
    ffs::create_directories(d, ec);
    ffs::copy_file(expr_png, d / (name + ".png"),
                   ffs::copy_options::overwrite_existing, ec);
    return !ec;
}
struct Entry { ffs::path path; std::string label; bool named = false; };
inline std::vector<Entry> list(const std::string& expr_png) {
    std::vector<Entry> out; std::error_code ec;
    std::vector<ffs::path> named;
    for (auto& e : ffs::directory_iterator(vdir(expr_png, ".versions"), ec))
        if (e.path().extension() == ".png") named.push_back(e.path());
    std::sort(named.begin(), named.end());
    for (auto& p : named) out.push_back({p, p.stem().string(), true});
    std::vector<ffs::path> hist;
    for (auto& e : ffs::directory_iterator(vdir(expr_png, ".history"), ec))
        if (e.path().extension() == ".png") hist.push_back(e.path());
    std::sort(hist.rbegin(), hist.rend());   // newest first
    for (auto& p : hist) out.push_back({p, p.stem().string(), false});
    return out;
}
} // namespace fvers

// Serialize the captured-QR running list to <qr_dir>/index.json. Caller must
// hold state.mtx (it reads the live log). The folders themselves hold the link
// + raw image; this index is the de-dupe list and the menu's source of truth.
static void qr_write_index(const std::string& qr_dir, const QrCaptureLog& log) {
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

static std::vector<MenuItem> build_menu(
        IFaceController* teensy, XRDisplay* xr, CameraManager* cameras,
        LoRaRadio* lora, SmartKnob* knob, AudioEngine* audio, AppState& state,
        AndroidMirror* android_mirror, bool* android_overlay,
        OverlayConfig* pip_cfg1, OverlayConfig* pip_cfg2, OverlayConfig* pip_cfg3,
        bool* pip_cam1_overlay, bool* pip_cam2_overlay, bool* pip_cam3_overlay,
        OverlayConfig* android_cfg,
        HudColors* hud_col, HudConfig* hud_cfg, MenuSystem** menu_sys_pp,
        Mpu9250* mpu9250, Bno055* bno055,
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
        // Multi-cam quad layout: both CSI cameras (rotated, side-by-side) on
        // the top half, two USB cameras side-by-side on the bottom, the same
        // composite in both eyes. Toggle + which-two-USB pickers.
        bool*      multicam_layout = nullptr,
        EyeSource* multicam_usb_a  = nullptr,
        EyeSource* multicam_usb_b  = nullptr,
        EyeSource* multicam_top_a  = nullptr,
        EyeSource* multicam_top_b  = nullptr,
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
        sys::FanController* fans = nullptr,
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
        std::string* pf_nose_layout_p  = nullptr,
        // HUB75 panel layout (pickers + nudges). Owned by main, mutated by
        // the menu; main rebuilds the renderer when the user backs out of
        // the HUB75 Layout submenu (Phase 3) — for now changes take effect
        // on the next backend hot-swap.
        PfHub75Layout* pf_hub75_p = nullptr,
        // Named HUB75 layouts — Save As / Load / Rename / Delete management.
        // pf_hub75_p above is the *working copy* of the active entry; these
        // pointers expose the map + selection so the menu can swap which
        // layout is being edited. pf_layout_changed runs after every
        // mutation (active swap, save-as, rename, delete) so the controller
        // can pick up the new active-layout name for face-folder stamping.
        std::map<std::string, PfHub75Layout>* pf_hub75_layouts_p = nullptr,
        std::string* pf_hub75_active_p = nullptr,
        std::function<void()> pf_layout_changed = nullptr,
        // Face animation tunables — pointers + a "push live" callback that
        // forwards the current values into native_ctrl after a slider/toggle
        // change. Caller owns the slots and the persistence to config.json.
        bool*   pf_blink_enabled_p = nullptr,
        double* pf_blink_min_p     = nullptr,
        double* pf_blink_max_p     = nullptr,
        double* pf_blink_dur_p     = nullptr,
        double* pf_expr_fade_p     = nullptr,
        // Editor "V (Preview to panels)" hold time. No live-push callback —
        // the value is read each time the editor opens, so changes take
        // effect on the next Edit… launch.
        double* pf_preview_duration_p = nullptr,
        std::function<void()> pf_anim_push = nullptr,
        // Pushes an arbitrary particle-system spec (a {"layers":[...]} dict
        // or single-effect spec) directly to the native renderer, bypassing
        // the effect_id mapping. Used by the Layered Effects builder so the
        // user can compose multi-layer particle configs at runtime.
        std::function<void(const nlohmann::json&)> pf_set_effect_json = nullptr,
        // Toggle expression-coupled effects (mood preset follows the face) and
        // the config-backed flag the menu reads. Null on non-native backends.
        std::function<void(bool)> pf_set_expr_effects = nullptr,
        bool* pf_expr_effects_p = nullptr,
        // Live-preview tick: main calls this each frame; when Live Preview is on
        // it re-applies the builder spec on change. Installed inside build_menu.
        std::shared_ptr<std::function<void()>> pf_live_tick = nullptr,
        // The live cfg JSON object owned by main(). Used by the GPIO
        // Visualizer (pin-claim scan, I²C peripherals, user notes,
        // rail-current estimate) and the Layered Effects builder
        // (Save / Load slots persisted under
        // cfg["protoface"]["custom_effects"]).
        nlohmann::json* cfg_root = nullptr,
        // USB camera live-preview wiring: GL texture handles sampled by the camera
        // image-setting context panels, plus a per-frame "preview request" the
        // panels set (1/2/3) so the render loop opens the stream, hides the
        // on-screen PiP, and shows the feed in the context pane while adjusting.
        GLuint* tex_usb1 = nullptr, GLuint* tex_usb2 = nullptr, GLuint* tex_usb3 = nullptr,
        int*    usb_preview_req = nullptr,
        // Custom Gradient material editor (Protoface > Material Color). The
        // working copy lives in main(); pf_set_material pushes the built
        // "gradient:…" spec to the live renderer for instant preview.
        PfGradient* pf_gradient_p = nullptr,
        std::function<void(const std::string&)> pf_set_material = nullptr,
        // Restart the native HUB75 panel pusher (scripts/panel_driver.py) to
        // recover the face feed after a GPIO conflict, without restarting all of
        // ProtoHUD. No-op outside native HUB75 mode.
        std::function<void()> pf_restart_renderer = nullptr,
        // Configurable GPIO switch map: array of assignable pin slots + count,
        // and the master enable flag. Edited in the GPIO Buttons menu; applied
        // on the next launch (the poller is built from these at startup).
        input::GpioPinCfg* gpio_pins_p = nullptr,
        int gpio_slot_count = 0,
        bool* gpio_inputs_enabled_p = nullptr,
        // Rebuilds the GPIO poller from the live slots so menu edits apply
        // without a relaunch. Shared so main can install it after the poller
        // (which the menu is built before) exists.
        std::shared_ptr<std::function<void()>> gpio_reload = nullptr,
        integrations::KdeConnectBridge* kdc_p = nullptr,
        std::vector<std::string>* kdc_ignore_p = nullptr,
        std::vector<std::string>* kdc_msg_p = nullptr,
        // Optional button/switch coprocessor (input::CoprocInputs). enabled flag
        // + a live status string getter + a reload hook, mirroring the GPIO
        // poller's shared-after-construction pattern. All null when the build
        // has no coprocessor support wired.
        bool* coproc_enabled_p = nullptr,
        std::shared_ptr<std::function<void()>> coproc_reload = nullptr,
        std::shared_ptr<std::function<std::string()>> coproc_status = nullptr)
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

    // Per-expression Versions submenu: Save Version… (named) + a list of saved
    // versions (named + auto-backups) with a thumbnail panel; Make Current /
    // Delete each. Copy-based — Make Current backs up the live PNG, copies the
    // version in via the controller (which reloads), so the renderer is unchanged.
    struct VerThumb { GLuint tex = 0; std::string loaded;
                      std::filesystem::file_time_type mt{}; cv::Mat img; };
    auto make_versions_submenu = [&, teensy, menu_sys_pp](const std::string& expr) -> MenuItem {
        auto entries = std::make_shared<std::vector<fvers::Entry>>();
        auto focus   = std::make_shared<int>(-1);
        auto thumb   = std::make_shared<VerThumb>();
        auto refresh = [entries, expr, teensy]{
            *entries = fvers::list(teensy->face_image_path(expr)); };

        std::vector<MenuItem> items;
        items.push_back(with_desc(leaf("Save Version\xE2\x80\xA6",
            [menu_sys_pp, expr, teensy, refresh, focus]{
                *focus = -1;
                if (!menu_sys_pp || !*menu_sys_pp) return;
                (*menu_sys_pp)->open_keyboard("Version name", "",
                    [expr, teensy, refresh](const std::string& nm){
                        fvers::save_named(teensy->face_image_path(expr), nm);
                        refresh();
                    });
            }), "Save the current art as a named version you can restore later."));
        for (int i = 0; i < 24; ++i) {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "version";
            row.label_fn = [entries, i]{
                if (i >= static_cast<int>(entries->size())) return std::string();
                const auto& e = (*entries)[i];
                return e.named ? e.label : ("auto: " + e.label);
            };
            row.visible_fn   = [entries, i]{ return i < static_cast<int>(entries->size()); };
            row.on_highlight = [focus, i]{ *focus = i; };
            MenuItem mk = leaf("Make Current",
                [entries, i, expr, teensy, refresh]{
                    if (i >= static_cast<int>(entries->size())) return;
                    const std::string live = teensy->face_image_path(expr);
                    fvers::backup_current(live, 15);                       // undo the switch
                    teensy->import_face_image(expr, (*entries)[i].path.string());  // copy + reload
                    teensy->set_face_by_name(expr);
                    refresh();
                });
            MenuItem del = leaf("Delete", [entries, i, refresh]{
                if (i >= static_cast<int>(entries->size())) return;
                std::error_code ec; std::filesystem::remove((*entries)[i].path, ec);
                refresh();
            });
            row.children = { std::move(mk), std::move(del) };
            items.push_back(std::move(row));
        }
        MenuItem sub = submenu("Versions", std::move(items));
        sub.action = refresh;   // SUBMENU on-enter hook: rescan disk
        sub.context_panel_title = "Version Preview";
        sub.context_panel_draw  = [thumb, focus, entries](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            std::string path;
            if (*focus >= 0 && *focus < static_cast<int>(entries->size()))
                path = (*entries)[*focus].path.string();
            const float pw = std::min(sz.x * 0.9f, (sz.y - 22.f) * 2.0f), ph = pw * 0.5f;
            const float px = o.x + (sz.x - pw) * 0.5f, py = o.y + (sz.y - ph) * 0.5f - 6.f;
            dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10, 16, 22, 190));
            if (path.empty()) {
                const char* m = "Scroll to a version";
                const ImVec2 ts = ImGui::CalcTextSize(m);
                dl->AddText({px + pw * 0.5f - ts.x * 0.5f, py + ph * 0.5f - ts.y * 0.5f},
                            IM_COL32(180, 190, 200, 200), m);
                return;
            }
            std::error_code ec; auto mt = std::filesystem::last_write_time(path, ec);
            if (path != thumb->loaded || mt != thumb->mt) {
                thumb->img = face::load_png_rgba(path, 256, 128);
                thumb->loaded = path; thumb->mt = mt;
            }
            if (!thumb->img.empty() && thumb->img.isContinuous()) {
                if (!thumb->tex) {
                    glGenTextures(1, &thumb->tex);
                    glBindTexture(GL_TEXTURE_2D, thumb->tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, thumb->tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumb->img.cols, thumb->img.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, thumb->img.data);
                glBindTexture(GL_TEXTURE_2D, 0);
                dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(thumb->tex)),
                             {px, py}, {px + pw, py + ph});
            }
        };
        return sub;
    };

    auto face_slot_row = [&, face_preview, edit_face, make_versions_submenu](int slot_idx) -> MenuItem {
        const std::string expr  = kFaceSlots[slot_idx].expression;
        const std::string label = kFaceSlots[slot_idx].label;

        MenuItem m;
        m.type  = MenuItemType::SUBMENU;
        m.label = label;

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
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

        // Copy from another face slot — useful as a starting point ("clone
        // 'happy' into 'wink' then tweak the eye"). Listed slots are gated
        // per-row to hide empties and self; the submenu itself hides when
        // there's nothing copyable. native_ctrl reload + on-screen set so
        // the new art shows immediately, matching the editor's commit path.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kFaceSlotCount; ++src_idx) {
            if (src_idx == slot_idx) continue;
            const std::string src_expr  = kFaceSlots[src_idx].expression;
            const std::string src_label = kFaceSlots[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another face's PNG into this slot as a "
                                "starting point. The source slot keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kFaceSlotCount; ++j) {
                if (kFaceSlots[j].expression == expr) continue;
                if (teensy->face_image_exists(kFaceSlots[j].expression)) return true;
            }
            return false;
        };

        // Edit launches the pixel editor on this slot's PNG. Visible only
        // when the active backend exposes covered LED regions — keeps the
        // option hidden in HUB75 / daemon modes where the editor would
        // have nothing meaningful to draw against.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem versions = make_versions_submenu(expr);
        versions.description = "Saved versions of this face (named + auto-backups) "
                               "with thumbnails — Make Current to restore one.";
        versions.visible_fn = bound_now;

        m.children = { std::move(play), std::move(edit_it), std::move(versions),
                       std::move(replace), std::move(copy_from),
                       std::move(clear), std::move(imp) };
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

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
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

        // Copy from another mouth-shape slot — useful when one viseme is
        // a small tweak on another (e.g. small → smile). Same pattern as
        // the expression Copy from... above.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kMouthShapeCount; ++src_idx) {
            if (src_idx == idx) continue;
            const std::string src_expr  = kMouthShapes[src_idx].file_stem;
            const std::string src_label = kMouthShapes[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another viseme's PNG into this slot as a "
                                "starting point. The source slot keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kMouthShapeCount; ++j) {
                if (kMouthShapes[j].file_stem == expr) continue;
                if (teensy->face_image_exists(kMouthShapes[j].file_stem)) return true;
            }
            return false;
        };

        // Edit the mouth-shape PNG with the pixel editor (mono on MAX7219,
        // color on RGB matrix). Same visibility gate as the expression
        // slots — hidden in HUB75 / daemon modes.
        MenuItem edit_it = leaf("Edit...",
            [edit_face, expr]{ if (edit_face) edit_face(expr); });
        edit_it.visible_fn = have_led_regions;

        MenuItem versions = make_versions_submenu(expr);
        versions.visible_fn = bound_now;

        m.children = { std::move(edit_it), std::move(versions), std::move(replace),
                       std::move(copy_from), std::move(clear), std::move(imp) };
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

        // Slot label: "(empty)" when no PNG; "[Other Layout]" when the
        // saved PNG was stamped with a layout name that doesn't match the
        // currently-active one. Untagged (legacy) faces show plain.
        m.label_fn = [teensy, expr, label, pf_hub75_active_p]() -> std::string {
            if (!teensy->face_image_exists(expr)) return label + " (empty)";
            const std::string tag = teensy->face_image_layout(expr);
            if (tag.empty() || !pf_hub75_active_p || tag == *pf_hub75_active_p)
                return label;
            return label + "  [" + tag + "]";
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

        // Copy from another boop reaction slot — same pattern as the
        // expression / mouth slot rows.
        std::vector<MenuItem> copy_children;
        for (int src_idx = 0; src_idx < kBoopFaceSlotCount; ++src_idx) {
            if (src_idx == idx) continue;
            const std::string src_expr  = kBoopFaceSlots[src_idx].file_stem;
            const std::string src_label = kBoopFaceSlots[src_idx].label;
            MenuItem ci = leaf(src_label,
                [teensy, src_expr, expr]{
                    const std::string src = teensy->face_image_path(src_expr);
                    if (!src.empty()) teensy->import_face_image(expr, src);
                });
            ci.visible_fn = [teensy, src_expr]{ return teensy->face_image_exists(src_expr); };
            copy_children.push_back(std::move(ci));
        }
        MenuItem copy_from = submenu("Copy from...", std::move(copy_children));
        copy_from.description = "Copy another boop reaction's PNG into this "
                                "slot as a starting point. The source slot "
                                "keeps its art.";
        copy_from.visible_fn = [teensy, expr]{
            for (int j = 0; j < kBoopFaceSlotCount; ++j) {
                if (kBoopFaceSlots[j].file_stem == expr) continue;
                if (teensy->face_image_exists(kBoopFaceSlots[j].file_stem)) return true;
            }
            return false;
        };

        MenuItem versions = make_versions_submenu(expr);
        versions.visible_fn = bound_now;

        m.children = { std::move(play), std::move(edit_it), std::move(versions),
                       std::move(replace), std::move(copy_from),
                       std::move(clear), std::move(imp) };
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

        return std::vector<MenuItem>{
            submenu("Focus",          std::move(fm)),
            submenu("White Balance",  std::move(wbm)),
            submenu("Rotation",       std::move(rm)),
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

    // ── Multi-Cam quad layout ────────────────────────────────────────────────
    // CSI left/right (rotated, per the Rotation submenu) fill the top half
    // side-by-side; the two selected USB cameras fill the bottom half. The
    // same composite is mirrored into both eyes. Build the "which two USB"
    // pickers as 3-item radio lists; A and B may not be the same camera.
    auto make_mc_usb_menu = [&](EyeSource* slot, EyeSource* other) -> std::vector<MenuItem> {
        if (!slot) return {};
        auto row = [&](const char* label, EyeSource v) {
            return leaf_sel(label,
                [slot, other, v]{
                    *slot = v;
                    if (other && *other == v) {   // keep the two slots distinct
                        *other = (v == EyeSource::USB1) ? EyeSource::USB2 : EyeSource::USB1;
                    }
                },
                [slot, v]{ return *slot == v; });
        };
        return { row("USB Cam 1", EyeSource::USB1),
                 row("USB Cam 2", EyeSource::USB2),
                 row("USB Cam 3", EyeSource::USB3) };
    };

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
                submenu("Top-Left Camera", make_mc_src_menu(multicam_top_a)),
                "Which camera fills the top-left quadrant (default Left CSI)."));
        if (multicam_top_b)
            multicam_menu.push_back(with_desc(
                submenu("Top-Right Camera", make_mc_src_menu(multicam_top_b)),
                "Which camera fills the top-right quadrant (default Right CSI)."));
        if (multicam_usb_a)
            multicam_menu.push_back(with_desc(
                submenu("Bottom-Left Camera", make_mc_usb_menu(multicam_usb_a, multicam_usb_b)),
                "Which USB camera fills the bottom-left quadrant."));
        if (multicam_usb_b)
            multicam_menu.push_back(with_desc(
                submenu("Bottom-Right Camera", make_mc_usb_menu(multicam_usb_b, multicam_usb_a)),
                "Which USB camera fills the bottom-right quadrant."));
    }

    std::vector<MenuItem> main_cameras_menu = {
        submenu("Left Eye Source",  make_eye_source_menu(left_eye_src)),
        submenu("Right Eye Source", make_eye_source_menu(right_eye_src)),
        with_desc(submenu("Multi-Cam Layout", std::move(multicam_menu)),
            "CSI cameras side-by-side on the top half (rotated per each "
            "camera's Rotation setting), two USB cameras side-by-side on the "
            "bottom. Same view in both eyes. Set the CSI rotation to 90\xc2\xb0 "
            "under Left/Right Camera > Rotation for the intended portrait fit."),
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
        with_desc([&]{
            // Recover a CSI sensor that came up dark/wedged at boot without a
            // full reboot: tears down + re-enumerates + restarts both cameras.
            MenuItem m; m.type = MenuItemType::LEAF; m.label = "Reinitialize CSI Cameras";
            m.label_fn = [cameras]{
                std::string s = "Reinitialize CSI  [L:";
                s += cameras->owl_left_ok()  ? "ok" : "\xE2\x80\x94";
                s += " R:"; s += cameras->owl_right_ok() ? "ok" : "\xE2\x80\x94";
                return s + "]";
            };
            m.action = [cameras, &state]{
                const bool ok = cameras->reinit_owls();
                const bool lok = cameras->owl_left_ok(), rok = cameras->owl_right_ok();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App;
                n.title = ok ? "CSI cameras reinitialized" : "CSI reinit: no camera found";
                char b[64]; snprintf(b, sizeof(b), "Left %s  \xC2\xB7  Right %s",
                                     lok ? "OK" : "\xE2\x80\x94", rok ? "OK" : "\xE2\x80\x94");
                n.body = b; n.auto_dismiss_s = 5.f;
                state.notifs.push(std::move(n));
            };
            return m;
        }(),
            "Re-enumerate and restart the CSI (OWLsight) cameras \xE2\x80\x94 recovers an "
            "eye that came up dark/wedged at boot, without rebooting. Briefly blacks "
            "both feeds while it re-acquires."),
        submenu("Low-Light Mode",   std::move(nv_menu)),
        submenu("Autofocus Both",   std::move(af_both_menu)),
        submenu("Capture Photo",    std::move(capture_menu)),
        submenu("Record Video",     std::move(video_menu)),
        submenu("QR Scan",          std::move(qr_menu)),
    };

    // ── Headset controls ──────────────────────────────────────────────────────
    auto xr_gaze = std::make_shared<bool>(false);   // local mirror of the mode state
    auto xr_3d   = std::make_shared<bool>(false);
    std::vector<MenuItem> headset_menu = {
        with_desc(slider("Electrochromic Transparency", 0.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_dimming); },
            [xr, &state](float v){
                state.xr_dimming = static_cast<int>(v);
                if (xr) xr->set_dimming(static_cast<int>(v));
            }),
            "Darken the glasses' electrochromic film (0 = clear see-through, 9 = "
            "fully dimmed) so the display stands out against bright surroundings."),
        with_desc(slider("HUD Brightness", 1.f, 9.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_hud_brightness); },
            [xr, &state](float v){
                state.xr_hud_brightness = static_cast<int>(v);
                if (xr) xr->set_hud_brightness(static_cast<int>(v));
            }),
            "Brightness of the glasses' on-board HUD layer."),
        with_desc(slider("Backlight Brightness", 1.f, 7.f, 1.f, "",
            [&state]{ return static_cast<float>(state.xr_brightness); },
            [xr, &state](float v){
                state.xr_brightness = static_cast<int>(v);
                if (xr) xr->set_brightness(static_cast<int>(v));
            }),
            "Display panel / backlight brightness."),
        with_desc(leaf("Recenter Display", [xr]{ if (xr) xr->recenter_tracking(); }),
            "Re-center the head-tracked view so straight-ahead is forward right now."),
        with_desc(toggle("Gaze Lock (0-DoF Mode)",
            [xr_gaze]{ return *xr_gaze; },
            [xr, xr_gaze](bool v){ *xr_gaze = v; if (xr) xr->toggle_gaze_lock(); }),
            "Lock the image to the glasses (0-DoF) instead of head-tracking it \xe2\x80\x94 "
            "the HUD stays put as you look around."),
        with_desc(toggle("3D Side-by-Side",
            [xr_3d]{ return *xr_3d; },
            [xr, xr_3d](bool v){ *xr_3d = v; if (xr) xr->set_3d_mode(v); }),
            "Switch the glasses panel to 3D side-by-side stereo mode."),
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
    //
    // The Effects submenu hosts both the canned single-effect presets (via
    // teensy->set_effect(effect_id)) AND a Layered Builder where the user can
    // compose up to five particle layers, tweak each one's parameters, save
    // the composition to a slot, and export it to a file.
    // LayerCfg / LayeredEffectState are file-scope (above build_menu) — C++
    // forbids static-constexpr members on local classes, and we need
    // kMaxLayers to size the layer array.
    // Effects whose motion respects direction_deg — used to hide the
    // Direction slider for stationary / radial effects (sparkle / rings /
    // fireflies). Clouds participate too: when direction_deg is set their
    // clumps travel along the unit vector instead of the random horizontal
    // drift the effect uses by default.
    auto effect_is_directional = [](const std::string& e) {
        return e == "snow" || e == "rain" || e == "embers"
            || e == "confetti" || e == "clouds"
            || e == "lightning" || e == "meteor" || e == "bubbles";
    };
    static LayeredEffectState pf_layered;
    LayeredEffectState* pflz = &pf_layered;   // static address — safe to capture

    auto build_layered_spec = [pflz]() -> nlohmann::json {
        nlohmann::json out;
        out["layers"] = nlohmann::json::array();
        for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i) {
            const auto& L = pflz->layers[i];
            if (L.effect == "none") continue;
            nlohmann::json layer;
            layer["effect"]   = L.effect;
            layer["count"]    = L.count;
            layer["colors"]   = nlohmann::json::array({
                nlohmann::json::array({L.r, L.g, L.b})});
            layer["speed_min"]= L.speed_min;
            layer["speed_max"]= L.speed_max;
            layer["blend"]    = L.blend;
            // Only serialise direction when the user has overridden the
            // effect's default (>= 0). Otherwise omit so the particle
            // class falls back to its historical default angle.
            if (L.direction_deg >= 0.f)
                layer["direction_deg"] = L.direction_deg;
            if (L.direction_from != "none")
                layer["direction_from"] = L.direction_from;
            if (L.intensity_from != "none")
                layer["intensity_from"] = L.intensity_from;
            if (L.effect == "water") {
                layer["level"]      = L.level;
                layer["viscosity"]  = L.viscosity;
                layer["pitch_fill"] = L.pitch_fill;
                if (L.bubbles > 0) {
                    layer["bubbles"]     = L.bubbles;
                    layer["bubble_mode"] = L.bubble_mode;
                }
            }
            if (L.effect == "lightning") {
                layer["branches"] = L.branches;
                if (L.arc) layer["arc"] = true;
            }
            out["layers"].push_back(layer);
        }
        return out;
    };

    auto load_layered_spec = [pflz](const nlohmann::json& spec) {
        // Reset all layers, then walk the loaded "layers" array filling slots.
        for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
            pflz->layers[i] = LayerCfg{};
        if (!spec.contains("layers") || !spec["layers"].is_array()) return;
        int i = 0;
        for (const auto& jl : spec["layers"]) {
            if (i >= LayeredEffectState::kMaxLayers) break;
            LayerCfg& L = pflz->layers[i++];
            L.effect = jl.value("effect", std::string("none"));
            L.count  = jl.value("count", 20);
            if (jl.contains("colors") && jl["colors"].is_array()
                && !jl["colors"].empty() && jl["colors"][0].is_array()
                && jl["colors"][0].size() == 3) {
                L.r = jl["colors"][0][0].get<int>();
                L.g = jl["colors"][0][1].get<int>();
                L.b = jl["colors"][0][2].get<int>();
            }
            L.speed_min = jl.value("speed_min", 5.f);
            L.speed_max = jl.value("speed_max", 15.f);
            L.blend     = jl.value("blend", std::string("add"));
            L.direction_deg = jl.value("direction_deg", -1.f);
            L.direction_from = jl.value("direction_from", std::string("none"));
            L.intensity_from = jl.value("intensity_from", std::string("none"));
            L.level = jl.value("level", 0.4f);
            L.viscosity = jl.value("viscosity", 0.3f);
            L.pitch_fill = jl.value("pitch_fill", 0.0f);
            L.bubbles = jl.value("bubbles", 0);
            L.bubble_mode = jl.value("bubble_mode", std::string("rise"));
            L.arc = jl.value("arc", false);
            L.branches = jl.value("branches", 0.35f);
        }
    };

    // Live Preview — when on, edits in the builder/effect-settings apply
    // continuously. ParticleSystem::set_effect updates params in place when the
    // layer structure is unchanged, so tweaks don't reset the particle sim.
    auto pf_live = std::make_shared<bool>(false);
    auto live_apply = [pf_live, build_layered_spec, pf_set_effect_json]{
        if (*pf_live && pf_set_effect_json) pf_set_effect_json(build_layered_spec());
    };
    // main calls *pf_live_tick each frame; it re-applies only when the spec
    // actually changed, so dragging a slider previews smoothly (the particle
    // sim updates in place) without spamming rebuilds.
    if (pf_live_tick) {
        auto last = std::make_shared<nlohmann::json>();
        *pf_live_tick = [pf_live, build_layered_spec, pf_set_effect_json, last]{
            if (!*pf_live || !pf_set_effect_json) return;
            nlohmann::json s = build_layered_spec();
            if (s != *last) { pf_set_effect_json(s); *last = s; }
        };
    }

    // Effects users can pick per layer. "none" is the sentinel for "disable
    // this slot." The strings line up with the names ParticleSystem's factory
    // recognises (see particles.cpp::make_effect).
    static const char* const kLayerEffects[] = {
        "none", "sparkle", "embers", "rain", "snow",
        "confetti", "rings", "fireflies", "clouds",
        "lightning", "meteor", "bubbles", "fireworks", "vortex", "water",
    };
    static const char* const kBlendModes[] = {
        "add", "normal", "multiply", "screen",
    };

    auto build_layer_menu = [&, pflz](int idx) -> MenuItem {
        LayerCfg* L = &pflz->layers[idx];

        std::vector<MenuItem> effect_items;
        for (const char* name : kLayerEffects) {
            effect_items.push_back(leaf_sel(name,
                [L, name]{ L->effect = name; },
                [L, name]{ return L->effect == name; }));
        }

        std::vector<MenuItem> blend_items;
        for (const char* name : kBlendModes) {
            blend_items.push_back(leaf_sel(name,
                [L, name]{ L->blend = name; },
                [L, name]{ return L->blend == name; }));
        }

        // Move Up / Move Down — swap with the neighbouring layer. Hidden at
        // the ends so the user doesn't shuffle into a no-op.
        MenuItem move_up = leaf("Move Up", [pflz, idx]{
            if (idx > 0) std::swap(pflz->layers[idx], pflz->layers[idx - 1]);
        });
        move_up.visible_fn = [idx]{ return idx > 0; };
        MenuItem move_dn = leaf("Move Down", [pflz, idx]{
            if (idx + 1 < LayeredEffectState::kMaxLayers)
                std::swap(pflz->layers[idx], pflz->layers[idx + 1]);
        });
        move_dn.visible_fn = [idx]{ return idx + 1 < LayeredEffectState::kMaxLayers; };

        MenuItem clear = leaf("Clear Layer", [L]{ *L = LayerCfg{}; });

        std::vector<MenuItem> items = {
            submenu("Effect",     std::move(effect_items)),
            slider("Count",      0.f, 100.f,  1.f, "",
                [L]{ return static_cast<float>(L->count); },
                [L](float v){ L->count = static_cast<int>(v); }),
            // Color — Figma-style picker. The submenu carries Hue / Sat /
            // Value sliders for encoder navigation and a preset list; the
            // attached context panel draws a clickable SV square, a hue
            // strip, the live swatch, and the hex string. Both interactions
            // share L->r/g/b: each slider edit recomputes RGB from the live
            // HSV, and each mouse click in the panel does the same.
            ([&]{
                // HSV ↔ RGB helpers — small and fast, no need for OpenCV here.
                auto rgb2hsv = [](int ir, int ig, int ib,
                                  float& h, float& s, float& v) {
                    const float r = ir / 255.f, g = ig / 255.f, b = ib / 255.f;
                    const float mx = std::max({r, g, b});
                    const float mn = std::min({r, g, b});
                    const float d  = mx - mn;
                    v = mx;
                    s = mx > 0.f ? (d / mx) : 0.f;
                    if (d < 1e-6f)        h = 0.f;
                    else if (mx == r)     h = 60.f * std::fmod((g - b) / d + 6.f, 6.f);
                    else if (mx == g)     h = 60.f * ((b - r) / d + 2.f);
                    else                  h = 60.f * ((r - g) / d + 4.f);
                };
                auto hsv2rgb = [](float h, float s, float v,
                                  int& r, int& g, int& b) {
                    h = std::fmod(h, 360.f); if (h < 0.f) h += 360.f;
                    const float c  = v * s;
                    const float hp = h / 60.f;
                    const float x  = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
                    float rf = 0, gf = 0, bf = 0;
                    if      (hp < 1) { rf = c; gf = x; }
                    else if (hp < 2) { rf = x; gf = c; }
                    else if (hp < 3) { gf = c; bf = x; }
                    else if (hp < 4) { gf = x; bf = c; }
                    else if (hp < 5) { rf = x; bf = c; }
                    else             { rf = c; bf = x; }
                    const float m = v - c;
                    r = std::clamp(static_cast<int>(std::round((rf + m) * 255.f)), 0, 255);
                    g = std::clamp(static_cast<int>(std::round((gf + m) * 255.f)), 0, 255);
                    b = std::clamp(static_cast<int>(std::round((bf + m) * 255.f)), 0, 255);
                };

                // Sliders read/write HSV by round-tripping through L->r/g/b
                // (single source of truth). Round-trip precision is <1/255
                // per channel — imperceptible.
                auto get_h = [L, rgb2hsv]{
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v); return h;
                };
                auto get_s = [L, rgb2hsv]{
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v); return s * 100.f;
                };
                auto get_v = [L, rgb2hsv]{
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v); return v * 100.f;
                };
                auto set_h = [L, rgb2hsv, hsv2rgb](float new_h){
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v);
                    int nr, ng, nb; hsv2rgb(new_h, s, v, nr, ng, nb);
                    L->r = nr; L->g = ng; L->b = nb;
                };
                auto set_s = [L, rgb2hsv, hsv2rgb](float new_s){
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v);
                    int nr, ng, nb; hsv2rgb(h, new_s / 100.f, v, nr, ng, nb);
                    L->r = nr; L->g = ng; L->b = nb;
                };
                auto set_v = [L, rgb2hsv, hsv2rgb](float new_v){
                    float h, s, v; rgb2hsv(L->r, L->g, L->b, h, s, v);
                    int nr, ng, nb; hsv2rgb(h, s, new_v / 100.f, nr, ng, nb);
                    L->r = nr; L->g = ng; L->b = nb;
                };

                // Presets
                struct Preset { const char* name; int r, g, b; };
                static constexpr Preset kPresets[] = {
                    {"White",   255, 255, 255}, {"Red",     255,  40,  40},
                    {"Orange",  255, 140,  40}, {"Yellow",  255, 230,  60},
                    {"Green",    40, 220,  80}, {"Cyan",     60, 220, 220},
                    {"Blue",     60, 130, 255}, {"Magenta", 220,  80, 220},
                    {"Pink",    255, 160, 200}, {"Warm",    255, 180,  90},
                };
                std::vector<MenuItem> preset_items;
                for (const auto& p : kPresets) {
                    preset_items.push_back(leaf(p.name, [L, p]{
                        L->r = p.r; L->g = p.g; L->b = p.b;
                    }));
                }

                std::vector<MenuItem> color_items = {
                    slider("Hue",        0.f, 360.f, 1.f, "\xc2\xb0",
                           get_h, set_h),
                    slider("Saturation", 0.f, 100.f, 1.f, "%", get_s, set_s),
                    slider("Value",      0.f, 100.f, 1.f, "%", get_v, set_v),
                    submenu("Presets", std::move(preset_items)),
                };

                // Context-panel draw: SV square (clickable), hue strip,
                // swatch, and hex readout. The square shows white at TL,
                // pure hue at TR, black at the bottom — same gradient
                // ImGui's built-in ColorPicker draws.
                auto picker_draw =
                    [L, rgb2hsv, hsv2rgb](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                    ImFont* font = ImGui::GetFont();
                    const float fs = ImGui::GetFontSize();

                    // Title.
                    dl->AddText(font, fs * 1.0f, {o.x, o.y},
                                IM_COL32(230, 235, 240, 255), "Color");

                    // Live HSV from the layer's RGB.
                    float H, S, V;
                    rgb2hsv(L->r, L->g, L->b, H, S, V);

                    // Layout: SV square + hue strip + swatch + hex below
                    const float pad = 8.f;
                    const float top = o.y + fs * 1.4f;
                    const float strip_h = 14.f;
                    const float swatch_h = 30.f;
                    const float sv_h = std::max(80.f,
                        std::min(sz.x - pad * 2.f,
                                 sz.y - (top - o.y) - strip_h - swatch_h - fs - pad * 4.f));
                    const float sv_w = sv_h;
                    const float sv_x = o.x + pad;
                    const float sv_y = top;

                    // Pure hue colour for the square's top-right corner.
                    int hr, hg, hb;
                    hsv2rgb(H, 1.f, 1.f, hr, hg, hb);
                    const ImU32 white = IM_COL32(255, 255, 255, 255);
                    const ImU32 black = IM_COL32(0, 0, 0, 255);
                    const ImU32 hue_col = IM_COL32(hr, hg, hb, 255);

                    // Saturation × Value square.
                    dl->AddRectFilledMultiColor(
                        {sv_x, sv_y}, {sv_x + sv_w, sv_y + sv_h},
                        white, hue_col, black, black);
                    dl->AddRect({sv_x, sv_y}, {sv_x + sv_w, sv_y + sv_h},
                                IM_COL32(80, 90, 100, 255), 0.f, 0, 1.f);
                    // SV marker — small circle at (S, 1-V) on the square.
                    const float mx = sv_x + S * sv_w;
                    const float my = sv_y + (1.f - V) * sv_h;
                    dl->AddCircle({mx, my}, 5.f, IM_COL32(0, 0, 0, 230), 0, 2.5f);
                    dl->AddCircle({mx, my}, 5.f, IM_COL32(255, 255, 255, 230), 0, 1.f);

                    // Hue strip — 12 segments of bilinearly-shaded rainbow.
                    const float hs_x = sv_x;
                    const float hs_y = sv_y + sv_h + 8.f;
                    const float hs_w = sv_w;
                    constexpr int kSegs = 12;
                    for (int i = 0; i < kSegs; ++i) {
                        int r0, g0, b0, r1, g1, b1;
                        hsv2rgb(360.f * i       / kSegs, 1.f, 1.f, r0, g0, b0);
                        hsv2rgb(360.f * (i + 1) / kSegs, 1.f, 1.f, r1, g1, b1);
                        const float x0 = hs_x + hs_w * (i)     / kSegs;
                        const float x1 = hs_x + hs_w * (i + 1) / kSegs;
                        dl->AddRectFilledMultiColor(
                            {x0, hs_y}, {x1, hs_y + strip_h},
                            IM_COL32(r0, g0, b0, 255), IM_COL32(r1, g1, b1, 255),
                            IM_COL32(r1, g1, b1, 255), IM_COL32(r0, g0, b0, 255));
                    }
                    dl->AddRect({hs_x, hs_y}, {hs_x + hs_w, hs_y + strip_h},
                                IM_COL32(80, 90, 100, 255), 0.f, 0, 1.f);
                    // Hue marker (vertical line at H / 360).
                    const float hmx = hs_x + (H / 360.f) * hs_w;
                    dl->AddLine({hmx, hs_y - 2.f}, {hmx, hs_y + strip_h + 2.f},
                                IM_COL32(0, 0, 0, 230), 3.f);
                    dl->AddLine({hmx, hs_y - 2.f}, {hmx, hs_y + strip_h + 2.f},
                                IM_COL32(255, 255, 255, 230), 1.f);

                    // Swatch + hex below.
                    const float sw_y = hs_y + strip_h + 8.f;
                    const float sw_w = sv_w * 0.45f;
                    dl->AddRectFilled({sv_x, sw_y}, {sv_x + sw_w, sw_y + swatch_h},
                                      IM_COL32(L->r, L->g, L->b, 255), 4.f);
                    dl->AddRect({sv_x, sw_y}, {sv_x + sw_w, sw_y + swatch_h},
                                IM_COL32(80, 90, 100, 255), 4.f, 0, 1.f);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                                  L->r, L->g, L->b);
                    dl->AddText(font, fs * 1.0f,
                                {sv_x + sw_w + 12.f, sw_y + (swatch_h - fs) * 0.5f},
                                IM_COL32(230, 235, 240, 255), buf);

                    // Mouse interaction — clicking inside the SV square
                    // pins saturation+value; clicking inside the hue strip
                    // pins hue. Both update L->r/g/b live.
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const ImVec2 mp = ImGui::GetMousePos();
                        if (mp.x >= sv_x && mp.x < sv_x + sv_w &&
                            mp.y >= sv_y && mp.y < sv_y + sv_h) {
                            const float new_s = (mp.x - sv_x) / sv_w;
                            const float new_v = 1.f - (mp.y - sv_y) / sv_h;
                            int nr, ng, nb;
                            hsv2rgb(H, std::clamp(new_s, 0.f, 1.f),
                                      std::clamp(new_v, 0.f, 1.f), nr, ng, nb);
                            L->r = nr; L->g = ng; L->b = nb;
                        }
                        if (mp.x >= hs_x && mp.x < hs_x + hs_w &&
                            mp.y >= hs_y && mp.y < hs_y + strip_h) {
                            const float new_h = std::clamp(
                                (mp.x - hs_x) / hs_w, 0.f, 1.f) * 360.f;
                            int nr, ng, nb;
                            hsv2rgb(new_h, S, V, nr, ng, nb);
                            L->r = nr; L->g = ng; L->b = nb;
                        }
                    }
                };

                return with_panel(submenu("Color", std::move(color_items)),
                                  "Color Picker", picker_draw);
            })(),
            slider("Density",    1.f, 120.f,  1.f, "",
                [L]{ return static_cast<float>(L->count); },
                [L](float v){ L->count = static_cast<int>(v); }),
            slider("Speed Min",  0.f, 100.f,  1.f, "",
                [L]{ return L->speed_min; },
                [L](float v){ L->speed_min = v; if (L->speed_max < v) L->speed_max = v; }),
            slider("Speed Max",  0.f, 100.f,  1.f, "",
                [L]{ return L->speed_max; },
                [L](float v){ L->speed_max = v; if (L->speed_min > v) L->speed_min = v; }),
            // Direction (only for effects whose motion respects an angle).
            // The slider clicks in 10° increments through the full 360°;
            // visible_fn hides the row when the picked effect ignores it.
            ([&]{
                MenuItem dir = slider("Direction", 0.f, 360.f, 10.f, "\xc2\xb0",
                    [L]{ return L->direction_deg < 0.f ? 90.f : L->direction_deg; },
                    [L](float v){ L->direction_deg = v; });
                dir.visible_fn = [L, effect_is_directional]{
                    return effect_is_directional(L->effect);
                };
                return dir;
            })(),
            // Fill Level — only meaningful for the "water" liquid effect.
            ([&]{
                MenuItem lvl = slider("Fill Level", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->level * 100.f; },
                    [L](float v){ L->level = v / 100.f; });
                lvl.visible_fn = [L]{ return L->effect == "water"; };
                return lvl;
            })(),
            // Viscosity — only for "water": higher = slower, resists sloshing.
            ([&]{
                MenuItem vis = slider("Viscosity", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->viscosity * 100.f; },
                    [L](float v){ L->viscosity = v / 100.f; });
                vis.visible_fn = [L]{ return L->effect == "water"; };
                return vis;
            })(),
            // Pitch Fill — water only: head pitch shifts the liquid level.
            ([&]{
                MenuItem pf = slider("Pitch Fill", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->pitch_fill * 100.f; },
                    [L](float v){ L->pitch_fill = v / 100.f; });
                pf.visible_fn = [L]{ return L->effect == "water"; };
                return pf;
            })(),
            // Bubbles / droplets — water only.
            ([&]{
                MenuItem bub = slider("Bubbles", 0.f, 30.f, 1.f, "",
                    [L]{ return static_cast<float>(L->bubbles); },
                    [L](float v){ L->bubbles = static_cast<int>(v); });
                bub.visible_fn = [L]{ return L->effect == "water"; };
                return bub;
            })(),
            ([&]{
                std::vector<MenuItem> bm = {
                    leaf_sel("Rise (bubbles in liquid)", [L]{ L->bubble_mode = "rise"; },
                                                         [L]{ return L->bubble_mode == "rise"; }),
                    leaf_sel("Drip (droplets above)",    [L]{ L->bubble_mode = "drip"; },
                                                         [L]{ return L->bubble_mode == "drip"; }),
                };
                MenuItem m = submenu("Bubble Style", std::move(bm));
                m.label_fn   = [L]{ return std::string("Bubble Style: ") + L->bubble_mode; };
                m.visible_fn = [L]{ return L->effect == "water" && L->bubbles > 0; };
                return m;
            })(),
            // Lightning extras — arc mode + fork density (lightning only).
            ([&]{
                MenuItem t = toggle("Arc Mode",
                    [L]{ return L->arc; }, [L](bool v){ L->arc = v; });
                t.visible_fn = [L]{ return L->effect == "lightning"; };
                return t;
            })(),
            ([&]{
                MenuItem br = slider("Branches", 0.f, 100.f, 5.f, "%",
                    [L]{ return L->branches * 100.f; },
                    [L](float v){ L->branches = v / 100.f; });
                br.visible_fn = [L]{ return L->effect == "lightning"; };
                return br;
            })(),
            // Motion reactivity — couple this layer's direction to head movement.
            ([&]{
                static const char* const kMotionSrc[] = {"none", "heading", "yaw", "tilt"};
                std::vector<MenuItem> msrc;
                for (const char* s : kMotionSrc)
                    msrc.push_back(leaf_sel(s,
                        [L, s]{ L->direction_from = s; },
                        [L, s]{ return L->direction_from == s; }));
                MenuItem m = with_desc(submenu("Motion Reactive", std::move(msrc)),
                    "Drive this layer's direction from the IMU: heading locks it "
                    "to the compass, yaw makes it drift as you turn your head, "
                    "tilt skews it like gravity when you roll. Apply Now to push.");
                m.label_fn = [L]{ return std::string("Motion: ") + L->direction_from; };
                return m;
            })(),
            // Density reactivity — scale particle count from audio or motion.
            ([&]{
                static const char* const kIntSrc[] = {"none", "audio", "yaw_rate", "accel"};
                std::vector<MenuItem> isrc;
                for (const char* s : kIntSrc)
                    isrc.push_back(leaf_sel(s,
                        [L, s]{ L->intensity_from = s; },
                        [L, s]{ return L->intensity_from == s; }));
                MenuItem m = with_desc(submenu("Density Reactive", std::move(isrc)),
                    "Scale this layer's particle count from a live signal: audio "
                    "(mic level — pulses with sound), yaw_rate or accel (head "
                    "movement). Apply Now to push.");
                m.label_fn = [L]{ return std::string("Density: ") + L->intensity_from; };
                return m;
            })(),
            submenu("Blend Mode", std::move(blend_items)),
            std::move(move_up),
            std::move(move_dn),
            std::move(clear),
        };
        char name_buf[32];
        std::snprintf(name_buf, sizeof(name_buf), "Layer %d", idx + 1);
        MenuItem m = submenu(name_buf, std::move(items));
        // Dynamic label so the parent menu shows the active effect at a glance.
        m.label_fn = [L, idx]{
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Layer %d: %s", idx + 1,
                          L->effect.c_str());
            return std::string(buf);
        };
        return m;
    };

    std::vector<MenuItem> layered_items;
    for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
        layered_items.push_back(build_layer_menu(i));

    layered_items.push_back(leaf("Apply Now",
        [build_layered_spec, pf_set_effect_json]{
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        }));
    layered_items.push_back(with_desc(toggle("Live Preview",
        [pf_live]{ return *pf_live; },
        [pf_live, live_apply](bool v){ *pf_live = v; if (v) live_apply(); }),
        "Apply edits to the panels continuously as you tweak layers, instead of "
        "hitting Apply Now. The sim updates in place (no reset) while the layer "
        "structure is unchanged."));

    // (Built-in Presets / Randomize / Expression Effects are assembled as their
    //  own top-level Effects pages below — Premade / Random / the toggle.)

    // Save / Load — three numbered quick-save slots PLUS user-named
    // presets entered via the on-screen keyboard. Everything lands in
    // cfg["protoface"]["custom_effects"][key]; the existing mutate_cfg
    // path persists to disk on shutdown.

    // Ensures cfg[key1][key2]... resolves to an object so subsequent
    // operator[] writes/reads can't trip type_error.305/306. nlohmann's
    // operator[] auto-creates an empty object on a null node, but a
    // hand-edited string or array at any of these paths would throw —
    // overwrite with an empty object first to keep the menu robust.
    auto ensure_obj_path = [](json& root,
                              std::initializer_list<const char*> keys) -> json& {
        json* cur = &root;
        for (const char* k : keys) {
            if (!cur->is_object()) *cur = json::object();
            if (!cur->contains(k) || !(*cur)[k].is_object()) (*cur)[k] = json::object();
            cur = &(*cur)[k];
        }
        return *cur;
    };

    // "Save As..." opens the OSK to name the preset; commit writes
    // the live builder spec under cfg["protoface"]["custom_effects"][name].
    layered_items.push_back(leaf("Save As...",
        [cfg_root, menu_sys_pp, build_layered_spec, ensure_obj_path]{
            if (!menu_sys_pp || !*menu_sys_pp || !cfg_root) return;
            (*menu_sys_pp)->open_keyboard(
                "Preset Name", std::string(),
                [cfg_root, build_layered_spec, ensure_obj_path](const std::string& name){
                    if (!cfg_root || name.empty()) return;
                    ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] =
                        build_layered_spec();
                });
        }));

    std::vector<MenuItem> save_items, load_items;
    for (int s = 1; s <= 3; ++s) {
        char nm[16]; std::snprintf(nm, sizeof(nm), "Slot %d", s);
        save_items.push_back(leaf(nm, [cfg_root, s, build_layered_spec, ensure_obj_path]{
            if (!cfg_root) return;
            char key[16]; std::snprintf(key, sizeof(key), "slot_%d", s);
            ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[key] =
                build_layered_spec();
        }));
    }
    layered_items.push_back(submenu("Save to Slot", std::move(save_items)));

    // Load — walks every key under cfg["protoface"]["custom_effects"]
    // (sorted alphabetically) and exposes up to kMaxLoadEntries dynamic
    // rows. Each row's label tracks its slot's name via label_fn so a
    // freshly Saved-As preset shows up immediately on next draw, and a
    // visible_fn hides empty slots so the user only sees real entries.
    constexpr int kMaxLoadEntries = 16;
    auto preset_name_at = [cfg_root](int idx) -> std::string {
        if (!cfg_root || !cfg_root->contains("protoface")
            || !(*cfg_root)["protoface"].is_object()) return {};
        const auto& jpf = (*cfg_root)["protoface"];
        if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object())
            return {};
        std::vector<std::string> keys;
        for (auto& [k, _] : jpf["custom_effects"].items()) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        return (idx >= 0 && idx < static_cast<int>(keys.size()))
                ? keys[static_cast<size_t>(idx)] : std::string{};
    };
    for (int i = 0; i < kMaxLoadEntries; ++i) {
        MenuItem row;
        row.type = MenuItemType::LEAF;
        row.label = "preset";   // overridden by label_fn each draw
        row.label_fn   = [preset_name_at, i]{ return preset_name_at(i); };
        row.visible_fn = [preset_name_at, i]{ return !preset_name_at(i).empty(); };
        row.action = [cfg_root, preset_name_at, i, load_layered_spec]{
            const std::string nm = preset_name_at(i);
            if (!cfg_root || nm.empty()) return;
            if (!(*cfg_root)["protoface"].is_object()) return;
            const auto& jpf = (*cfg_root)["protoface"];
            if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object()) return;
            const auto& ce = jpf["custom_effects"];
            if (ce.contains(nm)) load_layered_spec(ce[nm]);
        };
        load_items.push_back(std::move(row));
    }
    layered_items.push_back(submenu("Load Preset", std::move(load_items)));

    // Delete — same dynamic listing as Load. Removing a preset key drops
    // it from cfg; the next mutate_cfg writeback won't carry it.
    std::vector<MenuItem> delete_items;
    for (int i = 0; i < kMaxLoadEntries; ++i) {
        MenuItem row;
        row.type = MenuItemType::LEAF;
        row.label = "preset";
        row.label_fn   = [preset_name_at, i]{ return preset_name_at(i); };
        row.visible_fn = [preset_name_at, i]{ return !preset_name_at(i).empty(); };
        row.action = [cfg_root, preset_name_at, i]{
            const std::string nm = preset_name_at(i);
            if (!cfg_root || nm.empty()) return;
            if (!(*cfg_root)["protoface"].is_object()) return;
            auto& jpf = (*cfg_root)["protoface"];
            if (!jpf.contains("custom_effects") || !jpf["custom_effects"].is_object()) return;
            jpf["custom_effects"].erase(nm);
        };
        delete_items.push_back(std::move(row));
    }
    layered_items.push_back(with_desc(
        submenu("Delete Preset", std::move(delete_items)),
        "Remove a saved preset permanently. The slot disappears from cfg "
        "on next save."));

    // Export the live composition to /tmp/protohud_layered_effect.json so
    // the user can copy it elsewhere or paste it into another cfg.
    layered_items.push_back(leaf("Export to File", [build_layered_spec]{
        const std::string path = "/tmp/protohud_layered_effect.json";
        std::ofstream f(path);
        if (!f) {
            std::fprintf(stderr, "[effects] cannot open %s\n", path.c_str());
            return;
        }
        f << build_layered_spec().dump(2) << "\n";
        std::cerr << "[effects] exported layered spec to " << path << "\n";
    }));

    MenuItem pf_layered_item = with_desc(
        submenu("Custom", std::move(layered_items)),
        "Compose up to five particle layers and apply the stack live. Each "
        "layer is independent: pick an effect, tweak count / colour / speed / "
        "blend, reorder with Move Up / Move Down, clear to disable.\n"
        "  • Apply Now pushes the composition to the renderer.\n"
        "  • Save As... names the current build and stores it.\n"
        "  • Save to Slot writes one of three numbered quick-save slots.\n"
        "  • Load Preset lists every named or slot save (alphabetical).\n"
        "  • Delete Preset removes a saved entry.\n"
        "  • Export to File dumps the live spec to "
        "/tmp/protohud_layered_effect.json.\n"
        "All presets persist under cfg[\"protoface\"][\"custom_effects\"].");

    // ── Effects first page: Single / Premade / Custom / Random ───────────────
    // 1) SINGLE EFFECTS — one primitive at a time. Select applies it (on layer 0,
    //    clearing the rest); Ctrl+Select opens that effect's full settings.
    std::vector<MenuItem> single_items;
    for (const char* nm : kLayerEffects) {
        if (std::string(nm) == "none") continue;
        const std::string name = nm;
        MenuItem it = leaf(name, [pflz, name, build_layered_spec, pf_set_effect_json]{
            for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i) pflz->layers[i] = LayerCfg{};
            pflz->layers[0].effect = name;
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        });
        it.description = "Select: apply this effect.  Ctrl+Select: open its settings.";
        // Ctrl+Select: switch layer 0 to this effect, then open the layer editor.
        it.secondary_action = [pflz, name]{
            pflz->layers[0] = LayerCfg{}; pflz->layers[0].effect = name;
            for (int i = 1; i < LayeredEffectState::kMaxLayers; ++i) pflz->layers[i] = LayerCfg{};
        };
        it.secondary_children = build_layer_menu(0).children;   // full per-layer settings
        single_items.push_back(std::move(it));
    }
    MenuItem single_item = with_desc(submenu("Single Effects", std::move(single_items)),
        "One effect at a time. Select applies it; Ctrl+Select opens its settings "
        "(colour, density, speed, and effect-specific options).");

    // 2) PREMADE EFFECTS — curated combos; the side panel shows the recipe.
    struct Premade { const char* name; const char* combo; };
    static const Premade kPremades[] = {
        {"gentle_snow","snow — light drift"}, {"heavy_snow","snow — dense"},
        {"campfire","embers"}, {"galaxy","sparkle — multicolour"},
        {"party","confetti"}, {"radar","expanding rings"},
        {"fire","embers + embers + sparkle"}, {"aurora","fireflies + sparkle"},
        {"blizzard","driven snow x2"}, {"sonar","rings + fireflies"},
        {"celebration","confetti + sparkle"}, {"plasma","blue embers x2 + ring"},
        {"thunderstorm","rain + branched lightning"}, {"arc","crackling electric arcs"},
        {"meteor_shower","meteors + sparkle"},
        {"fireworks","fireworks bursts"}, {"bubbles","rising bubbles"},
        {"vortex","comet vortex (cool) + sparkle"},
        {"vortex_ember","comet vortex (fire palette)"},
        {"vortex_toxic","comet vortex (toxic green)"},
        {"vortex_rose","comet vortex (pink/violet)"},
        {"vortex_rainbow","comet vortex (rainbow)"},
        {"nebula","clouds x2 + sparkle"},
        {"water","liquid — cyan, bubbles"}, {"lava","liquid — lava, thick"},
        {"toxic","liquid — green, bubbles"}, {"ocean","liquid — teal"},
        {"plasma_fluid","liquid — magenta"}, {"mercury","liquid — silver, thick"},
    };
    auto premade_combo = std::make_shared<std::string>();
    std::vector<MenuItem> premade_items;
    for (const auto& pm : kPremades) {
        const std::string name = pm.name, combo = pm.combo;
        MenuItem it = leaf(name, [name, pf_set_effect_json]{
            nlohmann::json spec; spec["preset"] = name;
            if (pf_set_effect_json) pf_set_effect_json(spec);
        });
        it.on_highlight = [premade_combo, name, combo]{ *premade_combo = name + "\n" + combo; };
        it.description  = "Select: apply.  Ctrl+Select: save a copy to Custom.";
        it.secondary_action = [cfg_root, name, ensure_obj_path]{
            if (!cfg_root) return;
            nlohmann::json spec; spec["preset"] = name;
            ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] = spec;
        };
        premade_items.push_back(std::move(it));
    }
    *premade_combo = std::string(kPremades[0].name) + "\n" + kPremades[0].combo;  // seed panel
    MenuItem premade_item = submenu("Premade Effects", std::move(premade_items));
    premade_item.context_panel_title = "Recipe";
    premade_item.context_panel_draw =
        [premade_combo](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            (void)sz;
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const std::string& s = *premade_combo;
            float y = o.y + 6.f;
            size_t start = 0;
            while (start <= s.size()) {
                const size_t nl = s.find('\n', start);
                const std::string line = s.substr(start,
                    nl == std::string::npos ? std::string::npos : nl - start);
                dl->AddText(font, fs, ImVec2(o.x + 6.f, y),
                            IM_COL32(220, 225, 235, 255), line.c_str());
                y += fs + 4.f;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        };
    premade_item = with_desc(std::move(premade_item),
        "Curated multi-layer presets. The side panel shows what each is made of. "
        "Select applies; Ctrl+Select saves a copy to Custom.");

    // 3) CUSTOM is the layered builder (pf_layered_item, renamed above).
    // 4) RANDOM — generate a fresh mix; optionally save it to Custom.
    std::vector<MenuItem> random_items;
    random_items.push_back(with_desc(leaf("Randomize (Surprise Me)",
        [pflz, build_layered_spec, pf_set_effect_json]{
            static std::mt19937 rng(std::random_device{}());
            static const char* const fx[] = {
                "sparkle","embers","rain","snow","confetti","rings","fireflies",
                "clouds","lightning","meteor","bubbles","fireworks","vortex"};
            static const int pal[][3] = {
                {0,220,180},{255,80,80},{80,180,255},{255,200,40},
                {180,80,255},{80,255,160},{255,120,20},{255,255,255}};
            const int nfx  = static_cast<int>(sizeof(fx)/sizeof(fx[0]));
            const int npal = static_cast<int>(sizeof(pal)/sizeof(pal[0]));
            for (int i = 0; i < LayeredEffectState::kMaxLayers; ++i)
                pflz->layers[i] = LayerCfg{};
            std::uniform_int_distribution<int> nlayers(1,3), pf(0,nfx-1),
                pp(0,npal-1), cnt(10,60), spd(2,30);
            const int N = nlayers(rng);
            for (int i = 0; i < N; ++i) {
                LayerCfg& L = pflz->layers[i];
                L.effect = fx[pf(rng)];
                const int* c = pal[pp(rng)];
                L.r = c[0]; L.g = c[1]; L.b = c[2];
                L.count = cnt(rng);
                L.speed_min = static_cast<float>(spd(rng));
                L.speed_max = L.speed_min + static_cast<float>(spd(rng));
                L.blend = "add";
            }
            if (pf_set_effect_json) pf_set_effect_json(build_layered_spec());
        }),
        "Roll a fresh 1–3 layer mix and apply it. Tweak it in Custom, or save it "
        "below."));
    random_items.push_back(leaf("Save Current to Custom...",
        [cfg_root, menu_sys_pp, build_layered_spec, ensure_obj_path]{
            if (!menu_sys_pp || !*menu_sys_pp || !cfg_root) return;
            (*menu_sys_pp)->open_keyboard("Preset Name", std::string(),
                [cfg_root, build_layered_spec, ensure_obj_path](const std::string& name){
                    if (!cfg_root || name.empty()) return;
                    ensure_obj_path(*cfg_root, {"protoface", "custom_effects"})[name] =
                        build_layered_spec();
                });
        }));
    MenuItem random_item = with_desc(submenu("Random", std::move(random_items)),
        "Discover new looks. Randomize applies a fresh mix; Save Current to Custom "
        "keeps the one you like.");

    std::vector<MenuItem> pf_effects;
    pf_effects.push_back(std::move(single_item));
    pf_effects.push_back(std::move(premade_item));
    pf_effects.push_back(std::move(pf_layered_item));   // "Custom"
    pf_effects.push_back(std::move(random_item));
    if (pf_expr_effects_p && pf_set_expr_effects)
        pf_effects.push_back(with_desc(toggle("Expression Effects",
            [pf_expr_effects_p]{ return *pf_expr_effects_p; },
            [pf_expr_effects_p, pf_set_expr_effects, cfg_root](bool v){
                *pf_expr_effects_p = v; pf_set_expr_effects(v);
                if (cfg_root) (*cfg_root)["protoface"]["expression_effects"] = v;
            }),
            "Auto-swap the particle effect to a mood preset as the face changes "
            "(angry\xe2\x86\x92""fire, happy\xe2\x86\x92""celebration, "
            "sad\xe2\x86\x92""rain, shocked\xe2\x86\x92""galaxy)."));
    {
        // Legacy Teensy/ProtoTracer single-effect ids (only meaningful on the
        // Teensy face backend) — tucked into their own page.
        const char* pf_effect_names[] = {
            "None","Sparkle","Embers","Rain","Snow","Confetti","Rings","Fireflies",
            "Fire","Aurora","Blizzard","Sonar","Plasma","Celebration","Galaxy","Party",
            "Clouds","Nebula",
        };
        const uint8_t pf_effect_count =
            static_cast<uint8_t>(sizeof(pf_effect_names) / sizeof(pf_effect_names[0]));
        std::vector<MenuItem> teensy_fx;
        for (uint8_t id = 0; id < pf_effect_count; id++)
            teensy_fx.push_back(leaf_sel(pf_effect_names[id],
                [teensy, id, &state]{
                    teensy->set_effect(id);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.effect_id = id;
                },
                [&state, id]{ return state.face.effect_id == id; }));
        pf_effects.push_back(with_desc(submenu("Teensy Face Effects", std::move(teensy_fx)),
            "Single-effect ids for the Teensy/ProtoTracer face backend (no effect "
            "on the native Protoface particle renderer)."));
    }

    // Material Color — the single colour/material picker (Face Color was folded
    // in here). Named solids + multi-colour patterns/gradients, plus a Custom
    // Color (solid) picker and the Custom Gradient editor below.
    std::vector<MenuItem> pf_palette;
    {
        struct PFMat { const char* label; uint8_t idx; };
        const PFMat pf_mats[] = {
            // Solids
            { "Teal",    0 }, { "Yellow", 1 }, { "Orange", 2 }, { "White", 3 },
            { "Green",   4 }, { "Purple", 5 }, { "Red",    6 }, { "Blue",  7 },
            { "Black",  11 },
            // Multi-colour patterns / gradients (8-10 are PNG patterns; 12+ are
            // built-in GradientMaterial presets — see preset_material()).
            { "Rainbow", 8 }, { "Cool",    9 }, { "Warm",  10 },
            { "Sunset", 12 }, { "Ocean",  13 }, { "Forest",14 }, { "Fire",  15 },
            { "Aurora", 16 }, { "Lava",   17 }, { "Galaxy",18 }, { "Pastel",19 },
            { "Candy",  20 }, { "Toxic",  21 },
        };
        for (const auto& m : pf_mats)
            pf_palette.push_back(leaf_sel(m.label,
                [teensy, idx = m.idx, &state]{
                    teensy->set_menu_item(8, idx);
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.face.material_color = idx;
                },
                [&state, idx = m.idx]{ return state.face.material_color == idx; }));

        // Custom solid colour (moved here from the removed Face Color menu) —
        // an arbitrary flat colour via the RGB/hex picker, applied as a
        // SolidMaterial like the named solids above.
        pf_palette.push_back(color_picker("Custom Color",
            [teensy, &state](uint8_t r, uint8_t g, uint8_t b){
                teensy->set_color(r, g, b);
                // Remember the choice so the picker re-opens on it (get_color
                // below reads these back — otherwise it always shows the default).
                std::lock_guard<std::mutex> lk(state.mtx);
                state.face.r = r; state.face.g = g; state.face.b = b;
            },
            [&state]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                std::lock_guard<std::mutex> lk(state.mtx);
                return { state.face.r, state.face.g, state.face.b };
            }));
    }

    // ── Custom Gradient — multi-colour material, optionally scrolling ─────────
    // Appended to the Material Color list. Every edit previews live via
    // pf_set_material (native renderer only); persisted to cfg["protoface"].
    if (pf_gradient_p) {
        auto* G = pf_gradient_p;
        auto apply_grad = [G, pf_set_material]{
            if (pf_set_material) pf_set_material(pf_gradient_spec(*G));
        };
        std::vector<MenuItem> grad_items;

        std::vector<MenuItem> gcount_items;
        for (int n = 2; n <= 6; ++n)
            gcount_items.push_back(leaf_sel(std::to_string(n) + " colors",
                [G, n, apply_grad]{ G->count = n; apply_grad(); },
                [G, n]{ return G->count == n; }));
        grad_items.push_back(submenu("Colors", std::move(gcount_items)));

        for (int i = 0; i < 6; ++i) {
            MenuItem cp = color_picker(std::string("Color ") + std::to_string(i + 1),
                [G, i, apply_grad](uint8_t r, uint8_t g, uint8_t b){
                    G->colors[i] = {r, g, b}; apply_grad();
                },
                [G, i]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                    return { static_cast<uint8_t>(G->colors[i][0]),
                             static_cast<uint8_t>(G->colors[i][1]),
                             static_cast<uint8_t>(G->colors[i][2]) };
                });
            cp.visible_fn = [G, i]{ return i < G->count; };
            grad_items.push_back(std::move(cp));
        }

        grad_items.push_back(toggle("Smooth Blend",
            [G]{ return G->smooth; },
            [G, apply_grad](bool v){ G->smooth = v; apply_grad(); }));

        std::vector<MenuItem> gdir_items = {
            leaf_sel("Horizontal",
                [G, apply_grad]{ G->direction = "horizontal"; apply_grad(); },
                [G]{ return G->direction != "vertical"; }),
            leaf_sel("Vertical",
                [G, apply_grad]{ G->direction = "vertical"; apply_grad(); },
                [G]{ return G->direction == "vertical"; }),
        };
        grad_items.push_back(submenu("Direction", std::move(gdir_items)));

        grad_items.push_back(slider("Speed", -60.f, 60.f, 1.f, " px/s",
            [G]{ return static_cast<float>(G->speed); },
            [G, apply_grad](float v){ G->speed = static_cast<int>(v); apply_grad(); }));

        grad_items.push_back(leaf("Use This Gradient", apply_grad));

        pf_palette.push_back(with_desc(
            submenu("Custom Gradient", std::move(grad_items)),
            "Multi-colour gradient painted behind the face. Pick 2–6 colours, "
            "Smooth or banded, a flow Direction and a Speed to scroll them "
            "behind the mask (0 = static). Previews live; persisted to "
            "cfg[\"protoface\"][\"gradient\"]."));
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

    // Context-panel preview for a chain-layout submenu (Eyes / Mouth /
    // Nose). Renders a labelled grid sized to the active layout, with
    // bold yellow borders drawn between each 8x8 LED module so the user
    // can see at a glance how many panels they're configuring. The "none"
    // pick shows a "Disabled" placeholder instead of a grid.
    auto chain_layout_preview = [](const std::string& zone_name,
                                   std::string* layout_ptr) -> MenuContextPanelDraw {
        return [zone_name, layout_ptr](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            // Header: zone name + pixel dimensions for the active layout.
            const ImU32 head_col = IM_COL32(230, 235, 240, 255);
            const ImU32 sub_col  = IM_COL32(170, 185, 200, 200);
            const ImU32 frame_bg = IM_COL32(20, 24, 32, 255);
            const ImU32 cell_col = IM_COL32(255, 255, 255, 30);
            const ImU32 mod_col  = IM_COL32(255, 220, 60, 255);
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            const std::string layout = layout_ptr ? *layout_ptr : std::string();
            int rows = 0, cols = 0;
            if (layout.size() == 3 && layout[1] == 'x' &&
                layout[0] >= '0' && layout[0] <= '9' &&
                layout[2] >= '0' && layout[2] <= '9') {
                rows = layout[0] - '0';
                cols = layout[2] - '0';
            }

            char title[64];
            std::snprintf(title, sizeof(title), "%s", zone_name.c_str());
            dl->AddText(font, fs * 1.4f, {o.x, o.y}, head_col, title);

            char sub[96];
            if (layout == "none" || rows == 0 || cols == 0) {
                std::snprintf(sub, sizeof(sub), "Disabled");
            } else {
                std::snprintf(sub, sizeof(sub),
                              "%d row × %d col   (%d × %d px)",
                              rows, cols, cols * 8, rows * 8);
            }
            dl->AddText(font, fs * 0.95f, {o.x, o.y + fs * 1.5f}, sub_col, sub);

            if (rows == 0 || cols == 0) return;

            // Fit the schematic into the remaining context-pane space, with
            // 8px padding on each side and a top margin below the header.
            const float top = o.y + fs * 3.2f;
            const float pad = 8.f;
            const float avail_w = std::max(20.f, sz.x - pad * 2.f);
            const float avail_h = std::max(20.f, o.y + sz.y - top - pad);
            const int   px_w    = cols * 8;
            const int   px_h    = rows * 8;
            const float cell    = std::max(1.f,
                std::floor(std::min(avail_w / px_w, avail_h / px_h)));
            const float grid_w  = cell * px_w;
            const float grid_h  = cell * px_h;
            const float gx      = o.x + (sz.x - grid_w) * 0.5f;
            const float gy      = top + (avail_h - grid_h) * 0.5f;

            // Background fill.
            dl->AddRectFilled({gx, gy}, {gx + grid_w, gy + grid_h}, frame_bg);

            // Faint per-pixel grid — only when cells are big enough to read.
            if (cell >= 6.f) {
                for (int x = 1; x < px_w; ++x) {
                    const float xx = gx + x * cell;
                    dl->AddLine({xx, gy}, {xx, gy + grid_h}, cell_col, 1.f);
                }
                for (int y = 1; y < px_h; ++y) {
                    const float yy = gy + y * cell;
                    dl->AddLine({gx, yy}, {gx + grid_w, yy}, cell_col, 1.f);
                }
            }

            // Bold module borders every 8 cells — the visual cue the user
            // asked for: "borders drawn between each 8x8 matrix."
            for (int c = 0; c <= cols; ++c) {
                const float xx = gx + c * 8.f * cell;
                dl->AddLine({xx, gy}, {xx, gy + grid_h}, mod_col, 2.f);
            }
            for (int r = 0; r <= rows; ++r) {
                const float yy = gy + r * 8.f * cell;
                dl->AddLine({gx, yy}, {gx + grid_w, yy}, mod_col, 2.f);
            }
        };
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
        with_desc(with_panel(submenu("Eyes",  std::move(eye_items)),
                             "Eyes",  chain_layout_preview("Eyes",  pf_eye_layout_p)),
                  "Panels per eye. The right eye is mirrored on the same row "
                  "so both eyes match. Used by the face editor to outline "
                  "Left Eye / Right Eye zones."),
        with_desc(with_panel(submenu("Mouth", std::move(mouth_items)),
                             "Mouth", chain_layout_preview("Mouth", pf_mouth_layout_p)),
                  "Mouth panels (anchor 4,25). Used by the face editor to "
                  "outline the Mouth zone."),
        with_desc(with_panel(submenu("Nose",  std::move(nose_items)),
                             "Nose",  chain_layout_preview("Nose",  pf_nose_layout_p)),
                  "Nose panels (single row, centred). \"None\" omits the "
                  "nose zone entirely."),
    };
    MenuItem pf_chain_layout_item = with_desc(
        submenu("Chain Layout", std::move(pf_chain_layout_items)),
        "Pick panels per zone — drives the bounding boxes the face editor "
        "highlights so you can see which canvas pixels each panel will "
        "display. Persisted to config.json.");
    pf_chain_layout_item.visible_fn = visible_for_native;

    // ── HUB75 Panel Layout (presets + per-panel pixel nudge) ─────────────────
    // Visible only on the hub75 backend. Picks panel size + count + arrangement
    // and exposes a per-panel Nudge X/Y so the user can shift any single panel
    // ±32 px to compensate for tiny mounting offsets between physical panels.
    MenuItem pf_hub75_layout_item;
    if (pf_hub75_p) {
        auto* H = pf_hub75_p;
        // String picker that also reapplies the auto-placed centre offsets
        // when the global Default Panel Size / Arrangement changes — the
        // canvas resizes around the new geometry, so the existing nudges
        // would point at the wrong canvas centres.
        auto hub_pick_str = [&leaf_sel, H](const char* lbl, std::string* slot, const char* v) {
            return leaf_sel(lbl,
                [slot, v, H]{ if (slot) { *slot = v; pf_hub75_apply_defaults(*H); } },
                [slot, v]{ return slot && *slot == v; });
        };
        auto hub_pick_int = [&leaf_sel, H](const char* lbl, int* slot, int v) {
            return leaf_sel(lbl,
                [slot, v, H]{ if (slot) { *slot = v; pf_hub75_apply_defaults(*H); } },
                [slot, v]{ return slot && *slot == v; });
        };
        std::vector<MenuItem> size_items = {
            hub_pick_str("32x16",  &H->panel_size, "32x16"),
            hub_pick_str("64x32",  &H->panel_size, "64x32"),
            hub_pick_str("64x64",  &H->panel_size, "64x64"),
            hub_pick_str("96x48",  &H->panel_size, "96x48"),
            hub_pick_str("128x32", &H->panel_size, "128x32"),
            hub_pick_str("128x64", &H->panel_size, "128x64"),
        };
        std::vector<MenuItem> count_items = {
            hub_pick_int("1 panel",  &H->panel_count, 1),
            hub_pick_int("2 panels", &H->panel_count, 2),
            hub_pick_int("3 panels", &H->panel_count, 3),
            hub_pick_int("4 panels", &H->panel_count, 4),
        };
        std::vector<MenuItem> arr_items = {
            hub_pick_str("Horizontal Chain", &H->arrangement, "horizontal"),
            hub_pick_str("Vertical Stack",   &H->arrangement, "vertical"),
            hub_pick_str("2x2 Grid",         &H->arrangement, "grid2x2"),
        };

        // Per-panel submenus carrying that slot's Size override + Nudge.
        // "Use Default" tracks the global Panel Size picker; the six other
        // entries pin this slot to that physical size regardless of the
        // global. Visible only when the panel index is within the active
        // count.
        // Per-slot Size pickers also reapply defaults — sizing a panel up
        // can grow the canvas, which shifts what "centre" means.
        auto size_pick = [&leaf_sel, H](const char* lbl, int i, const char* v) {
            return leaf_sel(lbl,
                [H, i, v]{ H->panel_size_per[i] = v; pf_hub75_apply_defaults(*H); },
                [H, i, v]{ return H->panel_size_per[i] == v; });
        };
        // Slider range: ±canvas/2 so each panel can be moved to either
        // edge of the canvas (centre-offset model). Capped at ±512 for
        // very large layouts so the encoder stays usable.
        auto nudge_x_max = [H]() -> float {
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            return static_cast<float>(std::min(512, std::max(64, cw / 2)));
        };
        auto nudge_y_max = [H]() -> float {
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            return static_cast<float>(std::min(512, std::max(64, ch / 2)));
        };
        std::vector<MenuItem> nudge_items;
        for (int i = 0; i < 4; ++i) {
            std::vector<MenuItem> size_items = {
                size_pick("Use Default", i, ""),
                size_pick("32x16",       i, "32x16"),
                size_pick("64x32",       i, "64x32"),
                size_pick("64x64",       i, "64x64"),
                size_pick("96x48",       i, "96x48"),
                size_pick("128x32",      i, "128x32"),
                size_pick("128x64",      i, "128x64"),
            };
            const float xmax = nudge_x_max();
            const float ymax = nudge_y_max();
            std::vector<MenuItem> axis_items = {
                submenu("Size", std::move(size_items)),
                slider("Offset X (from centre)", -xmax, xmax, 1.f, " px",
                    [H, i]{ return static_cast<float>(H->nudge_dx[i]); },
                    [H, i](float v){ H->nudge_dx[i] = static_cast<int>(v); }),
                slider("Offset Y (from centre)", -ymax, ymax, 1.f, " px",
                    [H, i]{ return static_cast<float>(H->nudge_dy[i]); },
                    [H, i](float v){ H->nudge_dy[i] = static_cast<int>(v); }),
                // Per-panel orientation. Pushed to the live renderer via
                // pf_layout_changed so the flip is visible on the panels
                // immediately (geometry edits above still need a restart/
                // backend round-trip; flips don't).
                toggle("Flip Horizontal",
                    [H, i]{ return H->flip_x[i]; },
                    [H, i, pf_layout_changed](bool v){
                        H->flip_x[i] = v;
                        if (pf_layout_changed) pf_layout_changed();
                    }),
                toggle("Flip Vertical",
                    [H, i]{ return H->flip_y[i]; },
                    [H, i, pf_layout_changed](bool v){
                        H->flip_y[i] = v;
                        if (pf_layout_changed) pf_layout_changed();
                    }),
                leaf("Reset to Default Position",
                     [H, i]{
                         // Reset just this slot by reapplying defaults to a
                         // scratch copy and copying the slot's values back.
                         PfHub75Layout tmp = *H;
                         pf_hub75_apply_defaults(tmp);
                         H->nudge_dx[i] = tmp.nudge_dx[i];
                         H->nudge_dy[i] = tmp.nudge_dy[i];
                     }),
            };
            char nm[32]; std::snprintf(nm, sizeof(nm), "Panel %d", i + 1);
            MenuItem m = submenu(nm, std::move(axis_items));
            // Dynamic label so the parent shows each slot's effective size.
            m.label_fn = [H, i]{
                const std::string& s = H->panel_size_per[i].empty()
                                       ? H->panel_size : H->panel_size_per[i];
                return std::string("Panel ") + std::to_string(i + 1)
                       + " (" + s + ")";
            };
            m.visible_fn = [H, i]{ return i < H->panel_count; };
            nudge_items.push_back(std::move(m));
        }

        // Schematic preview — draws all panels at their canvas positions
        // (after nudge) so the user sees the whole-set arrangement update
        // live as they tweak picker rows or nudge sliders.
        auto hub_preview = [H](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            dl->AddText(font, fs * 1.1f, {o.x, o.y},
                        IM_COL32(230, 235, 240, 255), "HUB75 Panel Layout");
            const auto panels = pf_hub75_panels(*H);
            int cw = 0, ch = 0; pf_hub75_canvas(*H, cw, ch);
            char sub[96];
            std::snprintf(sub, sizeof(sub),
                          "%d × %s   %s   canvas %dx%d",
                          H->panel_count, H->panel_size.c_str(),
                          H->arrangement.c_str(), cw, ch);
            dl->AddText(font, fs * 0.85f, {o.x, o.y + fs * 1.2f},
                        IM_COL32(170, 180, 190, 220), sub);

            const float top = o.y + fs * 2.6f;
            const float pad = 8.f;
            const float avail_w = std::max(40.f, sz.x - pad * 2.f);
            const float avail_h = std::max(40.f, o.y + sz.y - top - pad);
            if (cw <= 0 || ch <= 0) return;
            const float scale = std::min(avail_w / cw, avail_h / ch);
            const float gw = cw * scale, gh = ch * scale;
            const float ox = o.x + (sz.x - gw) * 0.5f;
            const float oy = top + (avail_h - gh) * 0.5f;
            const ImU32 bg   = IM_COL32(20, 24, 32, 255);
            const ImU32 pcol = IM_COL32(255, 220, 60, 255);
            const ImU32 ptxt = IM_COL32(20, 24, 28, 255);
            dl->AddRectFilled({ox, oy}, {ox + gw, oy + gh}, bg);
            for (size_t i = 0; i < panels.size(); ++i) {
                const auto& r = panels[i].rect;
                const float x0 = ox + r.x * scale;
                const float y0 = oy + r.y * scale;
                const float x1 = x0 + r.width  * scale;
                const float y1 = y0 + r.height * scale;
                dl->AddRectFilled({x0, y0}, {x1, y1},
                                  IM_COL32(255, 220, 60, 60));
                dl->AddRect({x0, y0}, {x1, y1}, pcol, 2.f, 0, 2.f);
                char lab[16];
                std::snprintf(lab, sizeof(lab), "P%zu", i + 1);
                const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, lab);
                dl->AddText(font, fs,
                    {x0 + ((x1 - x0) - ts.x) * 0.5f,
                     y0 + ((y1 - y0) - ts.y) * 0.5f},
                    ptxt, lab);
            }
        };

        std::vector<MenuItem> hub_items;

        // ── Named-layout management (Save As / Load / Rename / Delete) ───────
        // Shown only when the host wired through the named-layout pointers.
        // Operations work on the live `*H` so the user's in-progress edits to
        // the active layout are preserved when switching.
        if (pf_hub75_layouts_p && pf_hub75_active_p) {
            auto* M = pf_hub75_layouts_p;
            auto* A = pf_hub75_active_p;
            auto layout_changed = pf_layout_changed;
            // Helper: ith name in the map's iteration order (stable for std::map).
            auto nth_name = [M](int i) -> std::string {
                if (i < 0) return {};
                int k = 0;
                for (const auto& [n, _] : *M) { if (k++ == i) return n; }
                return {};
            };

            // Save As — copy the current working layout into a new named slot
            // and switch the working copy onto it.
            hub_items.push_back(with_desc(
                leaf("Save As...", [H, M, A, layout_changed, menu_sys_pp]{
                    if (!menu_sys_pp || !*menu_sys_pp) return;
                    (*menu_sys_pp)->open_keyboard("Layout Name", *A,
                        [H, M, A, layout_changed](const std::string& name){
                            if (name.empty()) return;
                            (*M)[*A]   = *H;          // checkpoint current edits
                            (*M)[name] = *H;          // copy into new slot
                            *A = name;
                            if (layout_changed) layout_changed();
                        });
                }),
                "Save the panels + nudges you've configured here as a new "
                "named layout. Subsequent edits go to the new layout; switch "
                "back via Load Layout."));

            // Load — pre-allocated 16 slot rows; each one's label_fn pulls the
            // i-th map entry and the action loads it. Hidden when i is past
            // the current map size. Matches the profile-slots pattern.
            constexpr int kLayoutSlots = 16;
            std::vector<MenuItem> load_items;
            for (int i = 0; i < kLayoutSlots; ++i) {
                MenuItem li;
                li.type  = MenuItemType::LEAF;
                li.label = "layout";
                li.label_fn = [nth_name, A, i]{
                    const std::string nm = nth_name(i);
                    if (nm.empty()) return std::string();
                    return (*A == nm) ? (nm + "  *") : nm;
                };
                li.visible_fn = [M, i]{ return i < static_cast<int>(M->size()); };
                li.action = [H, M, A, layout_changed, nth_name, i]{
                    const std::string nm = nth_name(i);
                    if (nm.empty() || *A == nm) return;
                    (*M)[*A] = *H;
                    *A = nm;
                    *H = (*M)[nm];
                    if (layout_changed) layout_changed();
                };
                load_items.push_back(std::move(li));
            }
            MenuItem load_sub = submenu("Load Layout", std::move(load_items));
            load_sub.label_fn = [A]{ return std::string("Load Layout  (") + *A + ")"; };
            load_sub.description =
                "Switch to a different saved layout. The current layout's "
                "edits are checkpointed first. * marks the active one.";
            hub_items.push_back(std::move(load_sub));

            // Rename — replace the active layout's key without copying data.
            hub_items.push_back(with_desc(
                leaf("Rename Active...", [H, M, A, layout_changed, menu_sys_pp]{
                    if (!menu_sys_pp || !*menu_sys_pp) return;
                    const std::string old_name = *A;
                    (*menu_sys_pp)->open_keyboard("Layout Name", old_name,
                        [H, M, A, layout_changed, old_name](const std::string& name){
                            if (name.empty() || name == old_name) return;
                            (*M)[old_name] = *H;
                            (*M)[name] = (*M)[old_name];
                            M->erase(old_name);
                            *A = name;
                            if (layout_changed) layout_changed();
                        });
                }),
                "Rename the active layout. Face PNGs stamped with the old "
                "name will read as a mismatch until you re-import them."));

            // Delete — only when there's at least one other layout to fall back to.
            MenuItem del = leaf("Delete Active",
                [H, M, A, layout_changed]{
                    if (M->size() <= 1) return;
                    M->erase(*A);
                    *A = M->begin()->first;
                    *H = M->begin()->second;
                    if (layout_changed) layout_changed();
                });
            del.label_fn = [A]{ return std::string("Delete \"") + *A + "\""; };
            del.visible_fn = [M]{ return M->size() > 1; };
            del.description = "Remove this layout. The first remaining "
                              "saved layout becomes active.";
            hub_items.push_back(std::move(del));
        }

        hub_items.push_back(
            with_desc(submenu("Default Panel Size", std::move(size_items)),
                      "Size applied to every panel slot whose Panel N > Size "
                      "is set to \"Use Default\". Per-slot overrides let a "
                      "build mix sizes."));
        hub_items.push_back(submenu("Panel Count", std::move(count_items)));
        hub_items.push_back(submenu("Arrangement", std::move(arr_items)));
        for (auto& it : nudge_items) hub_items.push_back(std::move(it));
        hub_items.push_back(with_desc(
            leaf("Reset All Positions",
                 [H]{ pf_hub75_apply_defaults(*H); }),
            "Re-seed every panel's Offset X/Y to the auto-placed positions "
            "for the current size + count + arrangement."));

        pf_hub75_layout_item = with_desc(
            with_panel(submenu("HUB75 Layout", std::move(hub_items)),
                       "HUB75 Panel Layout", hub_preview),
            "Panel size + count + arrangement for the HUB75 backend. "
            "Each panel exposes a Nudge X/Y slider (±32 px integer) so you "
            "can align the editor canvas to physical mounting offsets, plus "
            "Flip Horizontal / Vertical toggles to match how each panel is "
            "mounted/wired (these apply to the panels live). "
            "Save As / Load lets you keep multiple named layouts (e.g. one "
            "per helmet build); faces are stamped with the active layout "
            "name on import. Persisted to cfg[\"protoface\"][\"hub75_layouts\"].");
        if (pf_hub75_active_p) {
            auto* A = pf_hub75_active_p;
            pf_hub75_layout_item.label_fn = [A]{
                return std::string("HUB75 Layout  (") + *A + ")";
            };
        }
        pf_hub75_layout_item.visible_fn = [pf_backend_p]{
            return pf_backend_p && *pf_backend_p == "hub75";
        };
    }

    std::vector<MenuItem> pf_hardware_menu = {
        with_desc(submenu("Backend", std::move(pf_backend_items)),
                  "What LED hardware Protoface paints. Switching tears down "
                  "the running renderer and brings up a new one with the new "
                  "backend; the HUD keeps running through the transition. "
                  "Persists to config.json so the next launch starts here."),
    };
    if (pf_restart_renderer) {
        MenuItem rr = with_desc(leaf("Restart Face Renderer", [pf_restart_renderer, state_ptr]{
            pf_restart_renderer();
            if (!state_ptr) return;
            std::lock_guard<std::mutex> lk(state_ptr->mtx);
            Notification n; n.type = NotifType::App;
            n.title = "Face renderer restarted";
            n.body  = "Relaunched the HUB75 panel driver";
            n.auto_dismiss_s = 4.f;
            state_ptr->notifs.push(std::move(n));
        }),
            "Kill + relaunch the HUB75 panel pusher to recover the face feed "
            "(e.g. after the live GPIO read stole a panel pin), without "
            "restarting ProtoHUD.");
        rr.visible_fn = [pf_backend_p]{ return !pf_backend_p || *pf_backend_p == "hub75"; };
        pf_hardware_menu.push_back(std::move(rr));
    }

    // The chain layout config used to live under Hardware, but it's really a
    // face-authoring concern (it shapes the editor's bbox guides), so it now
    // sits inside Face Options alongside the per-expression slots.
    face_files_menu.push_back(std::move(pf_chain_layout_item));
    if (pf_hub75_p) face_files_menu.push_back(std::move(pf_hub75_layout_item));

    // ── Size & Position — per-face placement transform ───────────────────────
    // Writes fit/scale/offset into the active face folder's config.json so a
    // face drawn for one panel size scales + sits correctly on another, then
    // reloads the loaders live. Honored by FaceLoader (see face_loader.cpp).
    {
        struct FaceXform { std::string folder, fit = "stretch"; double scale = 1.0; int ox = 0, oy = 0; };
        auto fx = std::make_shared<FaceXform>();
        auto fx_folder = [teensy]() -> std::string {
            const std::string p = teensy ? teensy->face_image_path("neutral") : std::string();
            if (p.empty()) return {};
            return std::filesystem::path(p).parent_path().string();
        };
        auto fx_load = [fx, fx_folder]{
            *fx = FaceXform{};
            fx->folder = fx_folder();
            if (fx->folder.empty()) return;
            std::ifstream f(fx->folder + "/config.json");
            if (!f) return;
            json j; try { f >> j; } catch (...) { return; }
            if (j.contains("fit") && j["fit"].is_string())          fx->fit   = j["fit"].get<std::string>();
            if (j.contains("scale") && j["scale"].is_number())      fx->scale = j["scale"].get<double>();
            if (j.contains("offset_x") && j["offset_x"].is_number())fx->ox    = (int)std::lround(j["offset_x"].get<double>());
            if (j.contains("offset_y") && j["offset_y"].is_number())fx->oy    = (int)std::lround(j["offset_y"].get<double>());
        };
        auto fx_save = [fx, teensy]{
            if (fx->folder.empty()) return;
            const std::string cp = fx->folder + "/config.json";
            json j = json::object();
            { std::ifstream f(cp); if (f) { try { f >> j; } catch (...) { j = json::object(); } } }
            if (!j.is_object()) j = json::object();
            j["fit"] = fx->fit; j["scale"] = fx->scale;
            j["offset_x"] = fx->ox; j["offset_y"] = fx->oy;
            const std::string tmp = cp + ".tmp";
            { std::ofstream f(tmp); if (f) f << j.dump(2); }
            std::error_code ec; std::filesystem::rename(tmp, cp, ec);
            if (teensy) teensy->reload_faces();      // live preview
        };
        std::vector<MenuItem> fit_items = {
            leaf_sel("Stretch (fill)", [fx, fx_save]{ fx->fit = "stretch"; fx_save(); },
                                       [fx]{ return fx->fit == "stretch"; }),
            leaf_sel("Contain (fit)",  [fx, fx_save]{ fx->fit = "contain"; fx_save(); },
                                       [fx]{ return fx->fit == "contain"; }),
            leaf_sel("Cover (crop)",   [fx, fx_save]{ fx->fit = "cover";   fx_save(); },
                                       [fx]{ return fx->fit == "cover"; }),
        };
        std::vector<MenuItem> xform_items = {
            with_desc(submenu("Fit Mode", std::move(fit_items)),
                "How the face fills the panel when its drawn size differs: Stretch "
                "fills both axes (may distort); Contain keeps aspect + letterboxes; "
                "Cover keeps aspect + crops to fill."),
            with_desc(slider("Scale", 0.25f, 3.0f, 0.05f, "x",
                [fx]{ return (float)fx->scale; },
                [fx, fx_save](float v){ fx->scale = v; fx_save(); }),
                "Extra uniform zoom on top of the fit. 1.0 = none."),
            with_desc(slider("Offset X", -128.f, 128.f, 1.f, "px",
                [fx]{ return (float)fx->ox; },
                [fx, fx_save](float v){ fx->ox = (int)std::lround(v); fx_save(); }),
                "Shift the face left/right (panel pixels)."),
            with_desc(slider("Offset Y", -128.f, 128.f, 1.f, "px",
                [fx]{ return (float)fx->oy; },
                [fx, fx_save](float v){ fx->oy = (int)std::lround(v); fx_save(); }),
                "Shift the face up/down (panel pixels)."),
            with_desc(leaf("Reset", [fx, fx_save]{
                fx->fit = "stretch"; fx->scale = 1.0; fx->ox = 0; fx->oy = 0; fx_save(); }),
                "Back to fill-the-panel with no zoom or shift."),
        };
        MenuItem xform_sub = with_desc(submenu("Size & Position", std::move(xform_items)),
            "Scale and shift the current face so art drawn for a different panel "
            "size fits this one. Applies live and is saved into the face folder, so "
            "it travels with the face. Affects every expression in the face.");
        xform_sub.action     = fx_load;   // on-enter: read current values from disk
        xform_sub.visible_fn = [fx_folder]{ return !fx_folder().empty(); };
        face_files_menu.push_back(std::move(xform_sub));
    }

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
        gated(submenu("Material Color", std::move(pf_palette)),  visible_for_hub75),
        // Face PNGs (per-expression slots, mouth shapes, boop reactions)
        // live here under Protoface rather than the generic Files menu —
        // they're meaningful per-backend, and the editor only makes sense
        // when the active backend supports it (MAX7219 / RGB matrix).
        with_desc(with_panel(submenu("Face Options", std::move(face_files_menu)),
                             "Face Preview", draw_face_preview),
                  "Per-expression face PNGs, mouth/boop slots, the in-HUD "
                  "pixel editor, and the chain layout pickers that shape "
                  "its bounding boxes. Edit... opens whenever the active "
                  "backend (Hardware > Backend) has an editor capability "
                  "— today: MAX7219 and RGB matrix; HUB75 stays "
                  "import-only. Files are stored in "
                  "faces/<active>[_<backend>]/ so each panel technology "
                  "keeps its own art."),
        // Face Animations — blink timing + expression fade. Mutates the live
        // FaceState on every panel via the pf_anim_push callback wired by
        // main; persists to cfg["protoface"]["animation"].
        ([&]() -> MenuItem {
            std::vector<MenuItem> blink_items;
            if (pf_blink_enabled_p) {
                blink_items.push_back(toggle("Enable Blinking",
                    [pf_blink_enabled_p]{ return *pf_blink_enabled_p; },
                    [pf_blink_enabled_p, pf_anim_push](bool v){
                        *pf_blink_enabled_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_min_p) {
                blink_items.push_back(slider("Min Interval", 1.0f, 15.0f, 0.5f, "s",
                    [pf_blink_min_p]{ return static_cast<float>(*pf_blink_min_p); },
                    [pf_blink_min_p, pf_blink_max_p, pf_anim_push](float v){
                        *pf_blink_min_p = v;
                        if (pf_blink_max_p && *pf_blink_max_p < v) *pf_blink_max_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_max_p) {
                blink_items.push_back(slider("Max Interval", 1.0f, 30.0f, 0.5f, "s",
                    [pf_blink_max_p]{ return static_cast<float>(*pf_blink_max_p); },
                    [pf_blink_min_p, pf_blink_max_p, pf_anim_push](float v){
                        *pf_blink_max_p = v;
                        if (pf_blink_min_p && *pf_blink_min_p > v) *pf_blink_min_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_blink_dur_p) {
                blink_items.push_back(slider("Duration", 0.05f, 0.5f, 0.01f, "s",
                    [pf_blink_dur_p]{ return static_cast<float>(*pf_blink_dur_p); },
                    [pf_blink_dur_p, pf_anim_push](float v){
                        *pf_blink_dur_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            std::vector<MenuItem> anim_items;
            if (!blink_items.empty()) {
                anim_items.push_back(with_desc(
                    submenu("Blink", std::move(blink_items)),
                    "Eyes-closed overlay timing. Min/Max set the random "
                    "interval between blinks; Duration is the full "
                    "close→open arc. Disable to hold eyes open."));
            }
            if (pf_expr_fade_p) {
                anim_items.push_back(slider("Expression Fade", 0.0f, 2.0f, 0.05f, "s",
                    [pf_expr_fade_p]{ return static_cast<float>(*pf_expr_fade_p); },
                    [pf_expr_fade_p, pf_anim_push](float v){
                        *pf_expr_fade_p = v;
                        if (pf_anim_push) pf_anim_push();
                    }));
            }
            if (pf_preview_duration_p) {
                anim_items.push_back(with_desc(
                    slider("Editor Preview Hold", 5.0f, 60.0f, 1.0f, "s",
                        [pf_preview_duration_p]{
                            return static_cast<float>(*pf_preview_duration_p);
                        },
                        [pf_preview_duration_p](float v){
                            *pf_preview_duration_p = v;
                        }),
                    "How long the V (Preview to panels) key in the face "
                    "editor shows the in-progress canvas on the physical "
                    "panels before auto-restoring the saved face."));
            }
            if (anim_items.empty()) {
                MenuItem empty;
                empty.visible_fn = []{ return false; };
                return empty;
            }
            return with_desc(submenu("Animations", std::move(anim_items)),
                "Idle-face animation tuning: blink cadence, blink duration, "
                "and the crossfade time between expressions. Applies to the "
                "native (MAX7219 / RGB matrix) renderer.");
        })(),
        gated(with_panel(submenu("GIFs", std::move(pf_gifs)),
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
    // Protoface first (the primary renderer), then the source radios and the
    // ProtoTracer submenu.
    std::vector<MenuItem> face_display_menu;
    if (!protoface_inner_menu.empty())
        face_display_menu.push_back(submenu("Protoface", std::move(protoface_inner_menu)));
    if (active_face_pp && teensy_option && fp_option) {
        face_display_menu.push_back(leaf_sel("Source: Teensy (ProtoTracer)",
            [active_face_pp, teensy_option]{ *active_face_pp = teensy_option; },
            [active_face_pp, teensy_option]{ return *active_face_pp == teensy_option; }));
        face_display_menu.push_back(leaf_sel("Source: Protoface",
            [active_face_pp, fp_option]{ *active_face_pp = fp_option; },
            [active_face_pp, fp_option]{ return *active_face_pp == fp_option; }));
    }
    face_display_menu.push_back(submenu("ProtoTracer", std::move(prototracer_inner_menu)));

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
            // MPR121 electrode → zone mapping (the I²C cap-touch input). Applies
            // on the next launch — the sensor reads it at start(). Hidden for
            // BothCheeks (derived from the two cheek electrodes).
            [&]{
                MenuItem m = with_desc(slider("Electrode (MPR121)", -1.f, 11.f, 1.f, "",
                    [&state, idx]{
                        std::lock_guard<std::mutex> lk(state.mtx);
                        return static_cast<float>(state.boop_zones[idx].electrode);
                    },
                    [&state, idx](float v){
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.boop_zones[idx].electrode = static_cast<int>(v);
                    }),
                    "Which MPR121 capacitive electrode (0-11) drives this zone; "
                    "-1 disables it. Takes effect on the next launch.");
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

        // ── Animated Eyes — rapid-boop easter egg ────────────────────────────
        // After Trigger Count fast boops on this zone, a procedural eye
        // animation plays instead of the normal reaction. The built-in
        // animations are re-skinnable (rate / scale / colour / hold time).
        {
            auto& bz = state;   // capture helper alias
            std::vector<MenuItem> anim_items;
            for (int a = 0; a < face::eye_anim_count(); ++a)
                anim_items.push_back(leaf_sel(face::eye_anim_name(static_cast<face::EyeAnim>(a)),
                    [&bz, idx, a]{
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.anim = a;
                    },
                    [&bz, idx, a]{
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        return bz.boop_zones[idx].eye_trigger.anim == a;
                    }));

            std::vector<MenuItem> eye_items = {
                toggle("Enabled",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return bz.boop_zones[idx].eye_trigger.enabled; },
                    [&bz, idx](bool v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.enabled = v; }),
                submenu("Animation", std::move(anim_items)),
                with_desc(slider("Trigger Count", 2.f, 10.f, 1.f, " boops",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.count); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.count = static_cast<int>(v); }),
                    "How many fast boops on this zone trigger the eyes."),
                with_desc(slider("Window", 1.f, 8.f, 0.5f, " s",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.window_s); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.window_s = static_cast<double>(v); }),
                    "Consecutive boops must land within this window or the "
                    "counter resets."),
                slider("Speed", 0.2f, 4.f, 0.1f, "x",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.speed); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.speed = static_cast<double>(v); }),
                slider("Size", 0.4f, 2.5f, 0.1f, "x",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.size); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.size = static_cast<double>(v); }),
                slider("Duration", 1.f, 8.f, 0.5f, " s",
                    [&bz, idx]{ std::lock_guard<std::mutex> lk(bz.mtx);
                        return static_cast<float>(bz.boop_zones[idx].eye_trigger.duration_s); },
                    [&bz, idx](float v){ std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.duration_s = static_cast<double>(v); }),
                color_picker("Color",
                    [&bz, idx](uint8_t r, uint8_t g, uint8_t b){
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        bz.boop_zones[idx].eye_trigger.r = r;
                        bz.boop_zones[idx].eye_trigger.g = g;
                        bz.boop_zones[idx].eye_trigger.b = b; },
                    [&bz, idx]() -> std::tuple<uint8_t,uint8_t,uint8_t> {
                        std::lock_guard<std::mutex> lk(bz.mtx);
                        const auto& e = bz.boop_zones[idx].eye_trigger;
                        return { e.r, e.g, e.b }; }),
                leaf("Test Eyes",
                    [teensy, &bz, idx]{
                        EyeTriggerConfig e;
                        { std::lock_guard<std::mutex> lk(bz.mtx);
                          e = bz.boop_zones[idx].eye_trigger; }
                        teensy->play_eye_animation(e.anim, e.speed, e.size,
                                                   e.r, e.g, e.b, e.duration_s);
                    }),
            };
            items.push_back(with_desc(submenu("Animated Eyes", std::move(eye_items)),
                "Boop this zone Trigger-Count times within Window seconds and a "
                "procedural eye animation plays instead of the normal reaction. "
                "Tune the built-in animations with Speed / Size / Color."));
        }

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

    // ── Light Sensor (BH1750) → squint reaction ────────────────────────────
    // Edge-detects dark→bright transitions (helmet stepping into sunlight)
    // and fires the configured expression for the chosen duration. The
    // expression name lines up with a Files > Faces slot so the user can
    // author it in the editor.
    std::vector<MenuItem> light_menu = {
        toggle("Enabled",
            [&state]{ return state.light_squint.enabled; },
            [&state](bool v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.enabled = v;
            }),
        slider("Dark Threshold", 1.f, 1000.f, 5.f, " lx",
            [&state]{ return state.light_squint.dark_threshold_lux; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.dark_threshold_lux = v;
                if (state.light_squint.bright_threshold_lux <= v)
                    state.light_squint.bright_threshold_lux = v + 10.f;
            }),
        slider("Bright Threshold", 50.f, 20000.f, 50.f, " lx",
            [&state]{ return state.light_squint.bright_threshold_lux; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.bright_threshold_lux = v;
                if (state.light_squint.dark_threshold_lux >= v)
                    state.light_squint.dark_threshold_lux = std::max(1.f, v - 10.f);
            }),
        slider("Transition Window", 0.2f, 10.f, 0.1f, " s",
            [&state]{ return state.light_squint.transition_window_s; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.transition_window_s = v;
            }),
        slider("Hold Duration", 0.2f, 5.f, 0.1f, " s",
            [&state]{ return static_cast<float>(state.light_squint.duration_s); },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.duration_s = v;
            }),
        slider("Cooldown", 0.5f, 30.f, 0.5f, " s",
            [&state]{ return state.light_squint.cooldown_s; },
            [&state](float v){
                std::lock_guard<std::mutex> lk(state.mtx);
                state.light_squint.cooldown_s = v;
            }),
    };
    face_display_menu.push_back(
        with_desc(submenu("Light Sensor", std::move(light_menu)),
                  "Triggers a face reaction when the wearer steps from a dim "
                  "area into a bright one. Reads a BH1750 over I²C "
                  "(/dev/i2c-1, addr 0x23). The expression name (default "
                  "\"squint\") maps to a Files > Faces slot — author the PNG "
                  "there. Cooldown gates back-to-back triggers under "
                  "flickering light."));

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
        [&]() -> MenuItem {
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
        }(),
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

    // Software gets its contents (Updates, Demo Mode, Profiles & Backup) appended
    // below once those menus are built; the old "Check for Updates" / "Pull &&
    // Rebuild" shell shortcuts were removed in favour of the in-HUD Updater.
    std::vector<MenuItem> software_menu;

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

    // ── GPIO Visualizer ──────────────────────────────────────────────────────
    // Renders the Pi 5 / CM5 40-pin header in the context pane as a vertical
    // 2x20 grid, colour-coded per Pi Foundation pinout. Each pin has a list
    // of claimants (the things in cfg that use it); pins with one claimant
    // get a green outline, pins with two or more get a red "conflict"
    // outline, idle pins stay grey. Hover any pin to see its claimants,
    // bus speed (SPI), I²C address list, and any user note assigned to it.
    struct GpioVizState {
        bool show_pin_num    = true;
        bool show_primary    = true;
        bool show_secondary  = true;
        bool show_tertiary   = false;
        bool show_inactive   = true;
        bool show_user_notes = true;   // 4th column when cfg["gpio_user_notes"] is set
        bool show_live_state = false;  // small H/L badge from /dev/gpiochip0 readings
        int  filter_kind     = -1;     // -1 = no filter, else PinKind enum value
    };
    static GpioVizState gpio_viz;

    // (BCM) → list of human-readable claimants (e.g. "MAX7219 left.spi",
    // "BNO055 IMU", "HUB75 R1"). Pins with >1 claimant are conflicts.
    struct PinClaims {
        std::map<int, std::vector<std::string>> claimants;
        std::map<int, int>                      spi_speed_hz;   // BCM → bus speed (for MOSI lines)
    };
    auto gpio_pin_claims = [cfg_root, gpio_pins_p, gpio_slot_count]() -> PinClaims {
        PinClaims pc;
        if (!cfg_root) return pc;
        const json& cfg = *cfg_root;
        auto add = [&pc](int bcm, std::string who) {
            if (bcm >= 0) pc.claimants[bcm].push_back(std::move(who));
        };
        // Configurable GPIO switch map — claim each assigned pin, labelled with
        // its (short-press, else long-press) function. Reads the live slots so
        // unsaved menu edits show up immediately.
        if (gpio_pins_p) {
            for (int i = 0; i < gpio_slot_count; ++i) {
                const auto& s = gpio_pins_p[i];
                const bool used = s.short_fn != input::GpioFunc::None ||
                                  s.long_fn  != input::GpioFunc::None;
                if (s.gpio >= 0 && used) {
                    const auto fn = (s.short_fn != input::GpioFunc::None) ? s.short_fn : s.long_fn;
                    add(s.gpio, std::string("GPIO: ") + input::gpio_func_name(fn));
                }
            }
        }

        // I²C peripherals — each enabled sensor on /dev/i2c-1 claims SDA1/SCL1.
        auto add_i2c = [&](const char* key, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            add(2, label);
            add(3, label);
        };
        add_i2c("bno055",       "BNO055 IMU");
        add_i2c("mpu9250",      "MPU9250 IMU");
        add_i2c("boop",         "MPR121 boop");
        add_i2c("light_sensor", "BH1750 light");

        // SPI chains — every chain on /dev/spidev0.x claims SPI0 pins,
        // /dev/spidev1.x claims SPI1 pins. Record per-bus speed for the
        // hover tooltip and pick up GPIO-transport CS pins.
        auto walk_chains = [&](const json& jchains, const char* tag) {
            if (!jchains.is_array()) return;
            for (const auto& jc : jchains) {
                if (!jc.is_object()) continue;
                const std::string dev = jc.value("spi_device", std::string());
                const std::string name = jc.value("name", std::string("chain"));
                const std::string who  = std::string(tag) + " " + name;
                const int speed = jc.value("speed_hz", 1'000'000);
                if (dev.find("/dev/spidev0") == 0) {
                    add(10, who + " (MOSI)"); add(9,  who + " (MISO)");
                    add(11, who + " (SCLK)"); add(8,  who + " (CE0)");
                    add(7,  who + " (CE1)");
                    pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed);
                }
                if (dev.find("/dev/spidev1") == 0) {
                    add(20, who + " (MOSI)"); add(19, who + " (MISO)");
                    add(21, who + " (SCLK)"); add(16, who + " (CE2)");
                    add(17, who + " (CE1)");
                    pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed);
                }
                add(jc.value("gpio_cs_pin", -1), who + " (GPIO CS)");
            }
        };
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            if (jpf.contains("max7219") && jpf["max7219"].is_object()) {
                const auto& jm = jpf["max7219"];
                walk_chains(jm.contains("chains") ? jm["chains"] : json::array(),
                            "MAX7219");
                add(jm.value("gpio_din_pin", -1), "MAX7219 GPIO bus DIN");
                add(jm.value("gpio_clk_pin", -1), "MAX7219 GPIO bus CLK");
            }
            if (jpf.contains("rgb_matrix") && jpf["rgb_matrix"].is_object()
                && jpf["rgb_matrix"].contains("chains"))
                walk_chains(jpf["rgb_matrix"]["chains"], "RGB matrix");

            // HUB75 + piomatter — uses the Adafruit RGB HAT pinout by default
            // (the most common shipping config). We tag these so a HUB75 build
            // shows the full pin claim, conflicts included.
            const std::string be = jpf.value("backend", std::string("hub75"));
            if (be == "hub75") {
                struct HubPin { int bcm; const char* name; };
                static constexpr HubPin kHub75[] = {
                    { 5,"R1"},{13,"G1"},{ 6,"B1"},{12,"R2"},{16,"G2"},{23,"B2"},
                    {22,"A" },{26,"B" },{27,"C" },{20,"D" },{24,"E" },
                    { 4,"OE"},{17,"CLK"},{21,"STB"},
                };
                for (const auto& h : kHub75)
                    add(h.bcm, std::string("HUB75 ") + h.name);
            }
        }
        // Accessory LEDs (WS2812) — single MOSI line on their chosen spidev.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            const std::string dev = jal.value("spi_device", std::string());
            const int speed = jal.value("speed_hz", 2'400'000);
            if (dev.find("/dev/spidev0") == 0) { add(10, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[10] = std::max(pc.spi_speed_hz[10], speed); }
            if (dev.find("/dev/spidev1") == 0) { add(20, "Accessory LEDs (MOSI)"); pc.spi_speed_hz[20] = std::max(pc.spi_speed_hz[20], speed); }
        }
        return pc;
    };

    // I²C peripheral list for the SDA/SCL hover tooltip. (addr, label).
    auto i2c_peripherals = [cfg_root]() -> std::vector<std::pair<int, std::string>> {
        std::vector<std::pair<int, std::string>> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        auto add_if = [&](const char* key, int default_addr, const char* label) {
            if (!cfg.contains(key) || !cfg[key].is_object()) return;
            const auto& jk = cfg[key];
            if (!jk.value("enabled", false)) return;
            if (jk.value("i2c_bus", std::string("/dev/i2c-1")) != "/dev/i2c-1") return;
            const int addr = jk.value("i2c_addr", default_addr);
            out.push_back({addr, label});
        };
        add_if("bno055",       0x28, "BNO055 IMU");
        add_if("mpu9250",      0x68, "MPU9250 IMU");
        add_if("boop",         0x5A, "MPR121 boop");
        add_if("light_sensor", 0x23, "BH1750 light");
        return out;
    };

    // User notes — free-form labels per BCM line via cfg["gpio_user_notes"]
    // ("17": "My LED strip"). Returned as a copy so the draw lambda doesn't
    // hold a json& back into main()'s cfg longer than necessary.
    auto user_notes = [cfg_root]() -> std::map<int, std::string> {
        std::map<int, std::string> out;
        if (!cfg_root) return out;
        const json& cfg = *cfg_root;
        if (!cfg.contains("gpio_user_notes") || !cfg["gpio_user_notes"].is_object())
            return out;
        for (const auto& [k, v] : cfg["gpio_user_notes"].items()) {
            try {
                const int bcm = std::stoi(k);
                if (v.is_string()) out[bcm] = v.get<std::string>();
            } catch (...) { /* skip non-int keys */ }
        }
        return out;
    };

    // Per-rail current estimate (mA) from known device draws. Anything not
    // in the table doesn't contribute — better silently zero than wildly
    // off-by-orders.
    auto rail_currents_mA = [cfg_root]() -> std::pair<int, int> {   // {3v3, 5v}
        int rail3 = 0, rail5 = 0;
        if (!cfg_root) return {rail3, rail5};
        const json& cfg = *cfg_root;
        auto enabled = [&cfg](const char* key) {
            return cfg.contains(key) && cfg[key].is_object()
                && cfg[key].value("enabled", false);
        };
        if (enabled("bno055"))        rail3 +=  12;
        if (enabled("mpu9250"))       rail3 +=   4;
        if (enabled("boop"))          rail3 +=  30;
        if (enabled("light_sensor"))  rail3 +=   1;
        // Accessory LEDs (WS2812) — assume ~20mA/LED at moderate brightness.
        if (cfg.contains("accessory_leds") && cfg["accessory_leds"].is_object()) {
            const auto& jal = cfg["accessory_leds"];
            int total = 0;
            if (jal.contains("zones") && jal["zones"].is_array())
                for (const auto& z : jal["zones"]) total += z.value("count", 0);
            rail5 += total * 20;
        }
        // MAX7219 chains — ~80 mA per module at full brightness.
        if (cfg.contains("protoface") && cfg["protoface"].is_object()) {
            const auto& jpf = cfg["protoface"];
            const std::string be = jpf.value("backend", std::string("hub75"));
            int max7219_mods = 0;
            if (be == "max7219" && jpf.contains("max7219") && jpf["max7219"].is_object()
                && jpf["max7219"].contains("chains") && jpf["max7219"]["chains"].is_array()) {
                for (const auto& jc : jpf["max7219"]["chains"]) {
                    if (!jc.is_object()) continue;
                    if (jc.contains("module_positions") && jc["module_positions"].is_array())
                        max7219_mods += static_cast<int>(jc["module_positions"].size());
                    else
                        max7219_mods += jc.value("cols_chips", 1) * jc.value("rows_chips", 1);
                }
            }
            rail5 += max7219_mods * 80;
        }
        return {rail3, rail5};
    };

    // The MenuContextPanelDraw lambda holds all of build_menu's GPIO helpers by
    // value (each captures cfg by reference, owned by main() which outlives
    // the menu system). gpio_viz has static storage so we capture its address.
    GpioVizState* gpvz_draw = &gpio_viz;
    auto draw_gpio_visualizer =
        [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
         gpvz_draw]() -> MenuContextPanelDraw {
        return [gpio_pin_claims, i2c_peripherals, user_notes, rail_currents_mA,
                gpvz_draw]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            const PinClaims claims = gpio_pin_claims();
            const auto i2c         = i2c_peripherals();
            const auto notes       = user_notes();
            const auto [ma3, ma5]  = rail_currents_mA();
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            // Live-state reader — lazily opens /dev/gpiochip0 the first time
            // the toggle is enabled. Lines already claimed by another consumer
            // (SPI/I²C driver, piomatter, MAX7219 chain) silently fail to
            // claim, and read() returns -1 for those. Static so the fds
            // persist across draws.
            static sys::GpioInputReader reader;
            static bool reader_attempted = false;
            if (gpvz_draw->show_live_state && !reader_attempted) {
                // Only request lines NOT already claimed by another subsystem.
                // Requesting a pin piomatter is driving (HUB75 panels) steals it
                // and blanks the face — so skip anything in the claims map.
                std::vector<int> wanted;
                for (int i = 0; i < 28; ++i)
                    if (claims.claimants.find(i) == claims.claimants.end())
                        wanted.push_back(i);  // free BCM line
                reader.open(wanted);
                reader_attempted = true;
            }

            // Light rounded card + white border behind the visualizer, then
            // inset the working area so content never touches the edges.
            {
                const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
                const ImU32 card_border = IM_COL32(255, 255, 255, 255);
                dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
                dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            }
            o.x += 8.f; o.y += 6.f; sz.x -= 16.f; sz.y -= 12.f;
            const ImU32 text_dark = IM_COL32(22, 26, 32, 255);   // body text on the light card
            const ImU32 text_dim  = IM_COL32(78, 86, 96, 255);   // secondary text

            // Layout: a dedicated text column on the left (title, rail
            // estimate, hovered-pin details — whatever used to live in the
            // floating tooltip) and the 2x20 grid pushed to the right so
            // the two never overlap. Text column width is a fraction of the
            // pane that scales sensibly between docked + expanded views.
            const float label_fs   = std::max(9.f, fs * 0.72f);
            const float info_w_min = 180.f;
            const float info_w     = std::clamp(sz.x * 0.34f, info_w_min,
                                                 sz.x * 0.55f);
            const float info_x     = o.x;
            const float grid_pane_x = o.x + info_w + 8.f;
            const float grid_pane_w = std::max(120.f, sz.x - info_w - 8.f);

            // Left text column: title + rail-current estimate (top), then a
            // "Pin info" block that follows the hovered pin.
            dl->AddText(font, fs * 1.05f, {info_x, o.y},
                        text_dark, "GPIO Header");
            dl->AddText(font, fs * 0.7f, {info_x, o.y + fs * 1.05f},
                        text_dim, "Pi 5 / CM5 — 40-pin");
            char rail_buf[96];
            std::snprintf(rail_buf, sizeof(rail_buf),
                          "3.3V ~%d mA   5V ~%d mA", ma3, ma5);
            const ImU32 rail_col = (ma5 > 2500 || ma3 > 500)
                ? IM_COL32(200, 110,  20, 255) : text_dim;
            dl->AddText(font, fs * 0.75f, {info_x, o.y + fs * 1.85f},
                        text_dim, "Est. rail draw:");
            dl->AddText(font, fs * 0.85f, {info_x, o.y + fs * 2.65f},
                        rail_col, rail_buf);

            const float info_block_top = o.y + fs * 4.0f;

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, sz.y - 2.f * vmargin);
            const float row_h   = std::max(14.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = o.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;

            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 3.f;
            const float pin_right_x = center_x + 3.f;

            const ImU32 outline_active   = IM_COL32( 40, 170,  60, 255);
            const ImU32 outline_conflict = IM_COL32(220,  50,  35, 255);
            const ImU32 outline_inactive = IM_COL32( 95, 105, 115, 220);
            const ImU32 live_high_col    = IM_COL32( 40, 170,  60, 255);
            const ImU32 live_low_col     = IM_COL32(110, 120, 130, 230);

            // Hover state for the tooltip.
            const ImVec2 mp = ImGui::GetMousePos();
            int           hovered_bcm = -1;
            sys::PinKind  hovered_kind = sys::PinKind::Gpio;
            std::string   hovered_primary;
            ImVec2        hover_pos{};

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            auto note_for = [&notes](int bcm) -> const char* {
                if (bcm < 0) return nullptr;
                auto it = notes.find(bcm);
                return (it == notes.end()) ? nullptr : it->second.c_str();
            };
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                std::string fn = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (p.tertiary && p.tertiary[0]) { if (!fn.empty()) fn += " / "; fn += p.tertiary; }
                c2[idx] = fn;
                std::string cl;
                if (p.bcm >= 0) {
                    auto c = claims.claimants.find(p.bcm);
                    if (c != claims.claimants.end() && !c->second.empty()) cl = c->second.front();
                }
                const char* note = gpvz_draw->show_user_notes ? note_for(p.bcm) : nullptr;
                if (!cl.empty()) {
                    c3[idx]    = cl;
                    c3col[idx] = (cl.rfind("GPIO: ", 0) == 0) ? IM_COL32(120, 170, 235, 255)  // GPIO map
                                                              : IM_COL32(235, 180,  90, 255); // hardware
                } else if (note) {
                    c3[idx]    = note;
                    c3col[idx] = IM_COL32(150, 195, 235, 255);                                 // user note
                }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float side_w_l = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col, bool dim) {
                if (t.empty() || w < 6.f) return;
                ImU32 bg = desc_bg;
                if (dim) { bg = (bg & 0x00FFFFFFu) | (110u << 24); col = (col & 0x00FFFFFFu) | (110u << 24); }
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left, bool dim) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col, dim);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col, dim); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx], dim);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col, dim);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col, dim);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx], dim);
                }
            };

            for (int row = 0; row < 20; ++row) {
                const sys::GpioPin& pl = sys::kPi40Pins[row * 2];     // odd phys (left col)
                const sys::GpioPin& pr = sys::kPi40Pins[row * 2 + 1]; // even phys (right col)
                const float y = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;

                auto draw_pin = [&](float x, const sys::GpioPin& p, bool right_side) {
                    const bool dim_by_filter = gpvz_draw->filter_kind >= 0 &&
                        static_cast<int>(p.kind) != gpvz_draw->filter_kind;
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (dim_by_filter)
                        fill = (fill & 0x00FFFFFFu) | (90u << 24);

                    auto it = (p.bcm >= 0) ? claims.claimants.find(p.bcm)
                                           : claims.claimants.end();
                    const bool is_active   = it != claims.claimants.end();
                    const bool is_conflict = is_active && it->second.size() > 1;
                    const ImU32 outline = is_conflict ? outline_conflict
                                       : is_active   ? outline_active
                                       : (gpvz_draw->show_inactive
                                          ? outline_inactive : 0);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    if (outline)
                        dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz},
                                    outline, 3.f, 0,
                                    is_conflict ? 3.f : (is_active ? 2.5f : 1.5f));
                    if (gpvz_draw->show_pin_num) {
                        char buf[6];
                        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                        const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, buf);
                        const float tx = x + (pin_sz - ts.x) * 0.5f;
                        const float ty = cy + (pin_sz - ts.y) * 0.5f;
                        // Bold white with an 8-direction black outline.
                        const ImU32 blk = IM_COL32(0, 0, 0, 235);
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -1; dy <= 1; ++dy)
                                if (dx || dy) dl->AddText(font, label_fs, {tx + dx, ty + dy}, blk, buf);
                        dl->AddText(font, label_fs, {tx, ty}, IM_COL32(255, 255, 255, 255), buf);
                    }
                    // Live state badge — small H/L dot inside the square's
                    // corner. Skipped for non-GPIO pins (power / GND / ID).
                    if (gpvz_draw->show_live_state && p.bcm >= 0) {
                        const int v = reader.read(p.bcm);
                        if (v >= 0) {
                            const ImU32 col = v ? live_high_col : live_low_col;
                            dl->AddCircleFilled({x + pin_sz - 4.f, cy + 4.f}, 2.5f, col);
                        }
                    }
                    // Hover detection (over square OR its description columns).
                    const float side_w = w1 + (w2 > 0.f ? w2 + colgap : 0.f)
                                            + (w3 > 0.f ? w3 + colgap : 0.f) + 6.f;
                    const float hit_l = right_side ? x : (x - side_w);
                    const float hit_r = right_side ? (x + pin_sz + side_w)
                                                   : (x + pin_sz);
                    if (mp.x >= hit_l && mp.x < hit_r &&
                        mp.y >= cy && mp.y < cy + pin_sz) {
                        hovered_bcm     = p.bcm;
                        hovered_kind    = p.kind;
                        hovered_primary = p.primary;
                        hover_pos       = {x + pin_sz * 0.5f, cy + pin_sz};
                    }
                };
                draw_pin(pin_left_x,  pl, false);
                draw_pin(pin_right_x, pr, true);

                const bool dim_l = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pl.kind) != gpvz_draw->filter_kind;
                const bool dim_r = gpvz_draw->filter_kind >= 0 &&
                    static_cast<int>(pr.kind) != gpvz_draw->filter_kind;

                // Three description columns on each side.
                draw_cols(row * 2,     cy, true,  dim_l);
                draw_cols(row * 2 + 1, cy, false, dim_r);
            }

            // Pin info — used to be a floating tooltip near the cursor;
            // now lives in the left text column so it never overlaps the
            // grid and shows even without active hover. Default copy
            // points the user at the grid.
            const bool   have_hover = hovered_bcm >= 0 || !hovered_primary.empty();
            const ImU32  hint_col   = text_dim;
            const ImU32  body_col   = text_dark;
            const float  info_lh    = label_fs + 2.f;
            float        info_y     = info_block_top;
            // Section header — coloured by conflict state when applicable.
            auto cit = claims.claimants.find(hovered_bcm);
            const bool conflict = (cit != claims.claimants.end()
                                   && cit->second.size() > 1);
            const ImU32 head_col = conflict ? outline_conflict
                                            : IM_COL32(40, 95, 165, 255);
            dl->AddText(font, fs * 0.8f, {info_x, info_y},
                        head_col, have_hover ? "Pin Info" : "Hover");
            info_y += fs * 1.0f;
            if (!have_hover) {
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "Hover a pin to see its");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "claimants, bus speed,");
                info_y += info_lh;
                dl->AddText(font, label_fs, {info_x, info_y},
                            hint_col, "I\xc2\xb2""C peripherals, etc.");
            } else {
                std::vector<std::string> lines;
                lines.push_back(hovered_primary.empty() ? std::string("(unknown)")
                                                       : hovered_primary);
                if (cit != claims.claimants.end()) {
                    lines.push_back(conflict ? "CONFLICT — claimants:"
                                             : "Used by:");
                    for (const auto& w : cit->second) lines.push_back("  " + w);
                } else if (hovered_bcm >= 0) {
                    lines.push_back("(idle)");
                }
                auto sit = claims.spi_speed_hz.find(hovered_bcm);
                if (sit != claims.spi_speed_hz.end()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Bus speed: %.2f MHz",
                                  sit->second / 1e6);
                    lines.push_back(buf);
                }
                if (hovered_kind == sys::PinKind::I2c) {
                    lines.push_back("");
                    lines.push_back("I\xc2\xb2""C /dev/i2c-1");
                    if (i2c.empty()) lines.push_back("  (none enabled)");
                    else for (const auto& [addr, name] : i2c) {
                        char buf[80];
                        std::snprintf(buf, sizeof(buf), "  0x%02X  %s",
                                      addr, name.c_str());
                        lines.push_back(buf);
                    }
                }
                auto nit = notes.find(hovered_bcm);
                if (nit != notes.end()) {
                    lines.push_back("");
                    lines.push_back("Note: " + nit->second);
                }
                // Render top-down, stop if we'd overrun the pane.
                const float info_max_y = o.y + sz.y - info_lh;
                for (const auto& l : lines) {
                    if (info_y > info_max_y) break;
                    dl->AddText(font, label_fs, {info_x, info_y},
                                body_col, l.c_str());
                    info_y += info_lh;
                }
            }
            // Suppress the now-unused float-tooltip anchor (kept hover_pos
            // around in case we ever bring back an alt tooltip).
            (void)hover_pos;
        };
    };

    // Pinout export — dumps the current header view to a text file the user
    // can paste into a build journal. Path is /tmp/protohud_pinout.txt for
    // simplicity (no menu picker for the location).
    auto export_pinout = [gpio_pin_claims, i2c_peripherals,
                          user_notes, rail_currents_mA]() {
        const PinClaims pc       = gpio_pin_claims();
        const auto      i2c      = i2c_peripherals();
        const auto      notes    = user_notes();
        const auto [ma3, ma5]    = rail_currents_mA();
        const std::string path   = "/tmp/protohud_pinout.txt";
        std::ofstream f(path);
        if (!f) {
            std::fprintf(stderr, "[gpio] could not open %s for write\n", path.c_str());
            return;
        }
        f << "ProtoHUD GPIO header snapshot\n";
        f << "Estimated rail draw: 3.3V ~" << ma3 << " mA   5V ~" << ma5 << " mA\n\n";
        f << "Phys  BCM  Name      Alt1        Alt2       Used by / notes\n";
        f << "----  ---  --------  ----------  ---------  ----------------------------\n";
        for (const auto& p : sys::kPi40Pins) {
            char line[256];
            std::snprintf(line, sizeof(line), "%4d  %3d  %-8s  %-10s  %-9s  ",
                          static_cast<int>(p.physical),
                          static_cast<int>(p.bcm),
                          p.primary, p.secondary, p.tertiary);
            f << line;
            auto it = (p.bcm >= 0) ? pc.claimants.find(p.bcm) : pc.claimants.end();
            if (it != pc.claimants.end()) {
                if (it->second.size() > 1) f << "[CONFLICT] ";
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i) f << " | ";
                    f << it->second[i];
                }
            }
            auto nit = notes.find(p.bcm);
            if (nit != notes.end()) {
                if (it != pc.claimants.end()) f << "  ";
                f << "note=\"" << nit->second << "\"";
            }
            f << "\n";
        }
        if (!i2c.empty()) {
            f << "\nI2C bus /dev/i2c-1:\n";
            for (const auto& [addr, name] : i2c) {
                char ab[80];
                std::snprintf(ab, sizeof(ab), "  0x%02X  %s\n", addr, name.c_str());
                f << ab;
            }
        }
        f.close();
        std::cerr << "[gpio] pinout exported to " << path << "\n";
    };

    GpioVizState* gpvz = &gpio_viz;   // static storage — capture by pointer is safe

    // Filter chip — single-select radio. -1 = no filter (everything in colour).
    auto filter_radio = [gpvz, &leaf_sel](const char* lbl, int v) -> MenuItem {
        return leaf_sel(lbl,
            [gpvz, v]{ gpvz->filter_kind = v; },
            [gpvz, v]{ return gpvz->filter_kind == v; });
    };
    std::vector<MenuItem> filter_menu = {
        filter_radio("None",   -1),
        filter_radio("GPIO",    static_cast<int>(sys::PinKind::Gpio)),
        filter_radio("I\xc2\xb2""C",  static_cast<int>(sys::PinKind::I2c)),
        filter_radio("SPI",     static_cast<int>(sys::PinKind::Spi)),
        filter_radio("UART",    static_cast<int>(sys::PinKind::Uart)),
        filter_radio("PWM",     static_cast<int>(sys::PinKind::Pwm)),
        filter_radio("Clock",   static_cast<int>(sys::PinKind::Gpclk)),
        filter_radio("3.3V",    static_cast<int>(sys::PinKind::Power3V3)),
        filter_radio("5V",      static_cast<int>(sys::PinKind::Power5V)),
        filter_radio("Ground",  static_cast<int>(sys::PinKind::Ground)),
    };

    std::vector<MenuItem> gpio_viz_menu = {
        toggle("Show Pin Numbers",
            [gpvz]{ return gpvz->show_pin_num; },
            [gpvz](bool v){ gpvz->show_pin_num = v; }),
        toggle("Show Primary Function",
            [gpvz]{ return gpvz->show_primary; },
            [gpvz](bool v){ gpvz->show_primary = v; }),
        toggle("Show Secondary Function",
            [gpvz]{ return gpvz->show_secondary; },
            [gpvz](bool v){ gpvz->show_secondary = v; }),
        toggle("Show Tertiary Function",
            [gpvz]{ return gpvz->show_tertiary; },
            [gpvz](bool v){ gpvz->show_tertiary = v; }),
        toggle("Show User Notes",
            [gpvz]{ return gpvz->show_user_notes; },
            [gpvz](bool v){ gpvz->show_user_notes = v; }),
        toggle("Outline Inactive Pins",
            [gpvz]{ return gpvz->show_inactive; },
            [gpvz](bool v){ gpvz->show_inactive = v; }),
        toggle("Show Live State (H/L)",
            [gpvz]{ return gpvz->show_live_state; },
            [gpvz](bool v){ gpvz->show_live_state = v; }),
        with_desc(submenu("Filter by Function", std::move(filter_menu)),
                  "Dim every pin whose function family doesn't match the "
                  "selected filter — quick way to spot all SPI lines or "
                  "all PWM-capable pins."),
        leaf("Export Pinout...", export_pinout),
    };
    MenuItem gpio_viz_item = with_panel(
        submenu("GPIO Visualizer", std::move(gpio_viz_menu)),
        "Pi 40-pin Header", draw_gpio_visualizer());
    gpio_viz_item.description =
        "Pi Foundation–style 2x20 view of the GPIO header.\n"
        "  • Squares are colour-coded by function family.\n"
        "  • Green outline = pin claimed by one thing in config.json. "
        "Red outline = conflict (multiple claimants).\n"
        "  • Hover a pin for its claimants, SPI bus speed, I\xc2\xb2""C addresses, "
        "and any user note assigned to it.\n"
        "  • Live State badge (toggle) reads /dev/gpiochip0 for pins not "
        "already claimed by another driver.\n"
        "  • User notes live at cfg[\"gpio_user_notes\"][\"<bcm>\"] = "
        "\"label\" and surface as a 4th column.\n"
        "  • Export Pinout… dumps the current view to /tmp/protohud_pinout.txt.\n"
        "  • Power-rail estimate (top of pane) sums known device current draws.\n"
        "  • Filter by Function dims everything not matching the selected family.";

    // ── GPIO pin picker (visual header page) ──────────────────────────────────
    // Helpers shared by the picker page + the slot list:
    //   gpio_hw_claimants  — non-GPIO peripherals holding a pin (HUB75/SPI/I²C…).
    //   gpio_other_slot    — another in-use slot bound to a pin (-1 = none).
    //   gpio_pin_selectable— a real GPIO line not held by hardware. Pins already
    //                        used by another *slot* stay selectable (override
    //                        allowed); the clash is flagged afterwards.
    //   gpio_slot_conflict — a slot whose pin collides with another slot or with
    //                        hardware; the slot list paints it red.
    auto gpio_hw_claimants = [gpio_pin_claims](int bcm) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (bcm < 0) return out;
        PinClaims pc = gpio_pin_claims();
        auto it = pc.claimants.find(bcm);
        if (it != pc.claimants.end())
            for (const auto& who : it->second)
                if (who.rfind("GPIO: ", 0) != 0) out.push_back(who);   // skip GPIO-map self-claims
        return out;
    };
    auto gpio_other_slot = [gpio_pins_p, gpio_slot_count](int bcm, int self_slot) -> int {
        if (bcm < 0 || !gpio_pins_p) return -1;
        for (int j = 0; j < gpio_slot_count; ++j) {
            if (j == self_slot) continue;
            const auto& s = gpio_pins_p[j];
            const bool used = s.short_fn != input::GpioFunc::None ||
                              s.long_fn  != input::GpioFunc::None;
            if (used && s.gpio == bcm) return j;
        }
        return -1;
    };
    auto gpio_pin_selectable = [gpio_hw_claimants](int bcm) -> bool {
        return bcm >= 0 && gpio_hw_claimants(bcm).empty();
    };
    auto gpio_slot_conflict = [gpio_pins_p, gpio_other_slot, gpio_hw_claimants](int i) -> bool {
        if (!gpio_pins_p) return false;
        const auto& s = gpio_pins_p[i];
        const bool used = s.short_fn != input::GpioFunc::None ||
                          s.long_fn  != input::GpioFunc::None;
        if (!used || s.gpio < 0) return false;
        return gpio_other_slot(s.gpio, i) >= 0 || !gpio_hw_claimants(s.gpio).empty();
    };

    // Context-panel renderer for the picker page. Mirrors the GPIO Visualizer's
    // look — a left info column plus a centred 2x20 header grid — on a light
    // rounded card with a white border. Each pin carries an easy-to-read label:
    // its claimant ("HUB75 R1"), "Slot N" if another slot uses it, else its
    // GPIO/alt-function name. Marking: green outline = this slot's pin, blue =
    // another slot, amber = hardware peripheral, dark = the cursor.
    auto draw_pin_picker_panel =
        [gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
        (std::shared_ptr<int> focus, int self_slot,
         input::GpioPinCfg* slot_p) -> MenuContextPanelDraw {
        return [focus, self_slot, slot_p, gpio_hw_claimants, gpio_other_slot, gpio_pin_selectable]
               (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs       = ImGui::GetFontSize();
            const float label_fs = std::max(9.f, fs * 0.72f);
            const int   fbcm     = focus  ? *focus      : -1;
            const int   assigned = slot_p ? slot_p->gpio : -1;

            // Light rounded card + white border behind the picker content.
            const ImU32 card_bg     = IM_COL32(214, 221, 228, 252);
            const ImU32 card_border = IM_COL32(255, 255, 255, 255);
            const ImU32 text_dark   = IM_COL32( 22,  26,  32, 255);
            const ImU32 text_dim    = IM_COL32( 78,  86,  96, 255);
            dl->AddRectFilled(o, {o.x + sz.x, o.y + sz.y}, card_bg, 10.f);
            dl->AddRect(o, {o.x + sz.x, o.y + sz.y}, card_border, 10.f, 0, 2.f);
            const float cpad = 9.f;
            const ImVec2 co{o.x + cpad, o.y + cpad};
            const ImVec2 cs{sz.x - cpad * 2.f, sz.y - cpad * 2.f};

            // Per-pin state + claimant label, computed once.
            bool selectable[40]; int otherslot[40]; bool hardware[40];
            std::string claim[40];
            for (int idx = 0; idx < 40; ++idx) {
                const int b = sys::kPi40Pins[idx].bcm;
                auto hw = (b >= 0) ? gpio_hw_claimants(b) : std::vector<std::string>{};
                hardware[idx]   = !hw.empty();
                selectable[idx] = b >= 0 && !hardware[idx];
                otherslot[idx]  = (b >= 0) ? gpio_other_slot(b, self_slot) : -1;
                claim[idx]      = hw.empty() ? std::string() : hw.front();
            }

            const float info_w      = std::clamp(cs.x * 0.30f, 110.f, cs.x * 0.5f);
            const float info_x      = co.x;
            const float grid_pane_x = co.x + info_w + 6.f;
            const float grid_pane_w = std::max(120.f, cs.x - info_w - 6.f);

            // Vertically centre the 20-row grid with equal top/bottom margin,
            // zooming the rows up to a shared max so both panels match.
            const float vmargin = 12.f;
            const float usable  = std::max(80.f, cs.y - 2.f * vmargin);
            const float row_h   = std::max(13.f, std::min(34.f, usable / 20.f));
            const float grid_h  = row_h * 20.f;
            const float top     = co.y + vmargin + std::max(0.f, (usable - grid_h) * 0.5f);
            const float pin_sz  = row_h * 0.80f;
            const float center_x    = grid_pane_x + grid_pane_w * 0.5f;
            const float pin_left_x  = center_x - pin_sz - 2.f;
            const float pin_right_x = center_x + 2.f;
            const float side_w_l    = (pin_left_x - 4.f) - grid_pane_x;
            const float side_w_r    = (grid_pane_x + grid_pane_w) - (pin_right_x + pin_sz + 4.f);

            const ImU32 out_focus    = IM_COL32( 15,  18,  24, 255);   // dark = cursor (pops on light)
            const ImU32 out_assigned = IM_COL32( 40, 170,  60, 255);   // this slot
            const ImU32 out_other    = IM_COL32( 40, 110, 210, 255);   // another slot
            const ImU32 out_hardware = IM_COL32(205, 120,  20, 255);   // hardware peripheral

            // Three columns per pin, ordered outward from the indicator box:
            //   [GPIO #]  [primary function]  [user / predefined function]
            // Each is its own grey box; per-column widths are uniform across pins.
            std::string c1[40], c2[40], c3[40]; ImU32 c3col[40] = {};
            for (int idx = 0; idx < 40; ++idx) {
                const sys::GpioPin& p = sys::kPi40Pins[idx];
                c1[idx] = (p.bcm >= 0) ? ("GPIO" + std::to_string(p.bcm))
                                       : std::string(p.primary ? p.primary : "");
                c2[idx] = (p.secondary && p.secondary[0]) ? p.secondary : "";
                if (hardware[idx])            { c3[idx] = claim[idx];
                                                c3col[idx] = IM_COL32(235, 180, 90, 255); }
                else if (otherslot[idx] >= 0) { c3[idx] = "Slot " + std::to_string(otherslot[idx] + 1);
                                                c3col[idx] = IM_COL32(120, 170, 235, 255); }
            }
            auto colw = [&](std::string* arr) {
                float w = 0.f;
                for (int i = 0; i < 40; ++i)
                    if (!arr[i].empty())
                        w = std::max(w, font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, arr[i].c_str()).x);
                return w;
            };
            const float box_pad = 5.f, colgap = 3.f;
            const float r1 = colw(c1), r2 = colw(c2), r3 = colw(c3);
            float w1 = r1 > 0.f ? r1 + box_pad * 2.f : 0.f;
            float w2 = r2 > 0.f ? r2 + box_pad * 2.f : 0.f;
            float w3 = r3 > 0.f ? r3 + box_pad * 2.f : 0.f;
            {   // shrink to fit the side gap if the three columns are too wide
                const float avail = std::max(40.f, std::min(side_w_l, side_w_r));
                const float total = w1 + (w2 > 0.f ? w2 + colgap : 0.f) + (w3 > 0.f ? w3 + colgap : 0.f);
                if (total > avail && total > 0.f) { const float k = avail / total; w1 *= k; w2 *= k; w3 *= k; }
            }
            const ImU32 desc_bg = IM_COL32(56, 62, 70, 240);
            const ImU32 c1col   = IM_COL32(230, 234, 238, 255);   // GPIO #
            const ImU32 c2col   = IM_COL32(180, 188, 196, 255);   // primary function
            auto draw_cell = [&](float bx0, float w, float cy, const std::string& t, ImU32 col) {
                if (t.empty() || w < 6.f) return;
                dl->AddRectFilled({bx0, cy}, {bx0 + w, cy + pin_sz}, desc_bg, 3.f);
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, t.c_str());
                dl->PushClipRect({bx0, cy}, {bx0 + w, cy + pin_sz}, true);
                dl->AddText(font, label_fs, {bx0 + box_pad, cy + (pin_sz - ts.y) * 0.5f}, col, t.c_str());
                dl->PopClipRect();
            };
            auto draw_cols = [&](int idx, float cy, bool left) {
                if (left) {
                    float x = pin_left_x - 4.f;
                    draw_cell(x - w1, w1, cy, c1[idx], c1col);                 x -= w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x - w2, w2, cy, c2[idx], c2col); x -= w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x - w3, w3, cy, c3[idx], c3col[idx]);
                } else {
                    float x = pin_right_x + pin_sz + 4.f;
                    draw_cell(x, w1, cy, c1[idx], c1col);                       x += w1 + colgap;
                    if (w2 > 0.f) { draw_cell(x, w2, cy, c2[idx], c2col);       x += w2 + colgap; }
                    if (w3 > 0.f)   draw_cell(x, w3, cy, c3[idx], c3col[idx]);
                }
            };

            // 8-direction black outline — kept for the white pin numbers so they
            // read on bright cells (descriptions use the grey box instead).
            auto otext = [&](float x, float y, ImU32 col, const char* s) {
                const ImU32 blk = IM_COL32(0, 0, 0, 225);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (dx || dy) dl->AddText(font, label_fs, {x + dx, y + dy}, blk, s);
                dl->AddText(font, label_fs, {x, y}, col, s);
            };
            auto draw_pin_num = [&](float bx, float by, const char* s) {
                const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, s);
                otext(bx - ts.x * 0.5f, by - ts.y * 0.5f, IM_COL32(255, 255, 255, 255), s);
            };

            for (int row = 0; row < 20; ++row) {
                const int il = row * 2, ir = row * 2 + 1;
                const float y  = top + row * row_h;
                const float cy = y + (row_h - pin_sz) * 0.5f;
                auto draw_box = [&](float x, int idx) {
                    const sys::GpioPin& p = sys::kPi40Pins[idx];
                    ImU32 fill = sys::pin_kind_color(p.kind);
                    if (p.bcm >= 0 && !selectable[idx]) fill = (fill & 0x00FFFFFFu) | (120u << 24);
                    dl->AddRectFilled({x, cy}, {x + pin_sz, cy + pin_sz}, fill, 3.f);
                    ImU32 oc = 0; float ow = 2.f;
                    if      (p.bcm >= 0 && p.bcm == assigned) { oc = out_assigned; ow = 2.5f; }
                    else if (hardware[idx])                   { oc = out_hardware; }
                    else if (otherslot[idx] >= 0)             { oc = out_other; }
                    if (oc) dl->AddRect({x, cy}, {x + pin_sz, cy + pin_sz}, oc, 3.f, 0, ow);
                    if (p.bcm >= 0 && p.bcm == fbcm)
                        dl->AddRect({x - 1.5f, cy - 1.5f}, {x + pin_sz + 1.5f, cy + pin_sz + 1.5f},
                                    out_focus, 3.f, 0, 3.f);
                    char buf[6]; std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.physical));
                    draw_pin_num(x + pin_sz * 0.5f, cy + pin_sz * 0.5f, buf);
                };
                draw_box(pin_left_x,  il);
                draw_box(pin_right_x, ir);

                draw_cols(il, cy, true);
                draw_cols(ir, cy, false);
            }

            // Info column (dark text on the light card): focused pin + legend.
            dl->AddText(font, fs * 0.7f, {info_x, co.y}, text_dim, "Pi 5 / CM5 — 40-pin header");
            int fidx = -1;
            for (int idx = 0; idx < 40; ++idx)
                if (sys::kPi40Pins[idx].bcm >= 0 && sys::kPi40Pins[idx].bcm == fbcm) { fidx = idx; break; }
            float iy = co.y + fs * 1.5f;
            if (fidx >= 0) {
                const sys::GpioPin& fp = sys::kPi40Pins[fidx];
                char hdr[24]; std::snprintf(hdr, sizeof(hdr), "GPIO %d", fp.bcm);
                dl->AddText(font, fs * 0.95f, {info_x, iy}, text_dark, hdr);
                iy += fs * 1.1f;
                std::string fns = fp.primary;
                if (fp.secondary && fp.secondary[0]) fns += " / " + std::string(fp.secondary);
                if (fp.tertiary  && fp.tertiary[0])  fns += " / " + std::string(fp.tertiary);
                dl->AddText(font, fs * 0.7f, {info_x, iy}, text_dim, fns.c_str());
                iy += fs * 1.15f;
                if (hardware[fidx]) {
                    dl->AddText(font, fs * 0.74f, {info_x, iy}, out_hardware, "In use (hardware):");
                    iy += fs * 0.92f;
                    for (const auto& w : gpio_hw_claimants(fp.bcm)) {
                        dl->AddText(font, fs * 0.7f, {info_x + 6.f, iy}, text_dark, w.c_str());
                        iy += fs * 0.86f;
                    }
                } else if (fp.bcm == assigned) {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_assigned, "This slot's pin");
                    iy += fs * 1.0f;
                } else if (otherslot[fidx] >= 0) {
                    char b[44]; std::snprintf(b, sizeof(b), "Also on Slot %d", otherslot[fidx] + 1);
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, out_other, b);
                    iy += fs * 0.9f;
                    dl->AddText(font, fs * 0.66f, {info_x, iy}, text_dim, "override → fix later");
                    iy += fs * 1.0f;
                } else {
                    dl->AddText(font, fs * 0.78f, {info_x, iy}, IM_COL32(30, 150, 50, 255), "Available");
                    iy += fs * 1.0f;
                }
                iy += fs * 0.4f;
            }
            auto legend = [&](ImU32 c, const char* t) {
                dl->AddRect({info_x, iy + 2.f}, {info_x + 11.f, iy + 13.f}, c, 2.f, 0, 2.f);
                dl->AddText(font, fs * 0.66f, {info_x + 16.f, iy}, IM_COL32(20, 24, 30, 255), t);
                iy += fs * 0.98f;
            };
            legend(out_assigned, "this slot");
            legend(out_other,    "other slot");
            legend(out_hardware, "hardware");
            legend(out_focus,    "cursor");
        };
    };

    // ── GPIO Buttons (configurable pin → function map) ────────────────────────
    // One submenu per assignable slot: BCM pin, short/long-press function, pull
    // bias, polarity, and long-press threshold. Edits the live gpio_pins array;
    // persisted to cfg["gpio"]["pins"] and applied on the next launch.
    MenuItem gpio_buttons_item;
    if (gpio_pins_p) {
        input::GpioPinCfg* GP = gpio_pins_p;
        // Build a function-picker submenu (all GpioFunc options) writing *slot.
        auto fn_picker = [&leaf_sel](input::GpioFunc* slot) {
            std::vector<MenuItem> items;
            for (int i = 0; i < input::gpio_func_count(); ++i) {
                const auto f = static_cast<input::GpioFunc>(i);
                items.push_back(leaf_sel(input::gpio_func_name(f),
                    [slot, f]{ *slot = f; },
                    [slot, f]{ return *slot == f; }));
            }
            return items;
        };

        std::vector<MenuItem> gpio_btn_menu;
        if (gpio_inputs_enabled_p)
            gpio_btn_menu.push_back(with_desc(toggle("Enabled",
                [gpio_inputs_enabled_p]{ return *gpio_inputs_enabled_p; },
                [gpio_inputs_enabled_p, gpio_reload](bool v){
                    *gpio_inputs_enabled_p = v;
                    if (gpio_reload && *gpio_reload) (*gpio_reload)();  // apply now
                }),
                "Master switch for the GPIO button map. Applies immediately."));
        if (gpio_reload)
            gpio_btn_menu.push_back(with_desc(
                leaf("Apply Changes Now",
                     [gpio_reload]{ if (*gpio_reload) (*gpio_reload)(); }),
                "Rebuild the GPIO poller from the current slots so pin / "
                "function / pull / polarity edits take effect right away."));

        // Optional button/switch coprocessor (RP2350/RP2040). When enabled, an
        // external MCU debounces the switches and streams presses to the Pi,
        // dispatched through the same functions as the GPIO slots above. The
        // slots stay live unless replace_local_gpio is set in config.
        if (coproc_enabled_p) {
            std::vector<MenuItem> coproc_menu;
            coproc_menu.push_back(with_desc(toggle("Enabled",
                [coproc_enabled_p]{ return *coproc_enabled_p; },
                [coproc_enabled_p, coproc_reload](bool v){
                    *coproc_enabled_p = v;
                    if (coproc_reload && *coproc_reload) (*coproc_reload)();
                }),
                "Use an external button coprocessor (USB/I\xC2\xB2""C). Applies "
                "immediately. GPIO slots above stay active too unless "
                "replace_local_gpio is set in config.json."));
            if (coproc_status) {
                MenuItem st = leaf("Status", []{});
                st.label_fn = [coproc_status]{
                    return std::string("Status: ") +
                           (*coproc_status ? (*coproc_status)() : std::string("n/a"));
                };
                coproc_menu.push_back(with_desc(std::move(st),
                    "Connection state reported by the coprocessor link "
                    "(connected / offline). Button mapping is edited in "
                    "config.json under inputs.coprocessor.buttons."));
            }
            gpio_btn_menu.push_back(with_desc(
                submenu("Button Coprocessor", std::move(coproc_menu)),
                "Optional external MCU (RP2350/RP2040) that handles button "
                "debounce and streams presses to the Pi \xE2\x80\x94 frees GPIO "
                "and offloads timing. See docs/coprocessor-input.md."));
        }

        for (int i = 0; i < gpio_slot_count; ++i) {
            input::GpioPinCfg* p = &GP[i];

            // Pin picker page for this slot: an "(unused)" entry plus one leaf per
            // header GPIO line, gated by gpio_pin_available so only free pins show.
            // Selecting a pin sets it and pops back here to the slot's options.
            auto picker_focus = std::make_shared<int>(p->gpio);
            std::vector<MenuItem> picker_items;
            picker_items.push_back(leaf_sel("(Unused / disable)",
                [p, menu_sys_pp]{ p->gpio = -1;
                                  if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); },
                [p]{ return p->gpio < 0; }));
            for (const auto& gp : sys::kPi40Pins) {
                if (gp.bcm < 0) continue;   // skip power / ground / ID pins
                const int bcm = gp.bcm;
                const std::string alt = (gp.secondary && gp.secondary[0]) ? gp.secondary : "";
                MenuItem pm;
                pm.type         = MenuItemType::LEAF;
                pm.label        = "GPIO " + std::to_string(bcm);
                pm.label_fn     = [bcm, alt, i, gpio_other_slot]{
                    std::string s = "GPIO " + std::to_string(bcm);
                    if (!alt.empty()) s += "   (" + alt + ")";
                    const int other = gpio_other_slot(bcm, i);
                    if (other >= 0) s += "   ! Slot " + std::to_string(other + 1);
                    return s;
                };
                pm.get_state    = [p, bcm]{ return p->gpio == bcm; };
                pm.on_highlight = [picker_focus, bcm]{ *picker_focus = bcm; };
                // List every GPIO line not held by hardware. Pins already on
                // another slot stay listed (override allowed) and are flagged red.
                pm.visible_fn   = [bcm, gpio_pin_selectable]{ return gpio_pin_selectable(bcm); };
                pm.warn_fn      = [bcm, i, gpio_other_slot]{ return gpio_other_slot(bcm, i) >= 0; };
                pm.action       = [p, bcm, menu_sys_pp]{
                    p->gpio = bcm;
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
                };
                picker_items.push_back(std::move(pm));
            }
            MenuItem pin_item = with_panel(submenu("Pin", std::move(picker_items)),
                                           "Select GPIO Pin",
                                           draw_pin_picker_panel(picker_focus, i, p));
            pin_item.action   = [picker_focus, p]{ *picker_focus = p->gpio; };  // sync focus on enter
            pin_item.label_fn = [p]{
                return p->gpio < 0 ? std::string("Pin: (unused)")
                                   : std::string("Pin: GPIO ") + std::to_string(p->gpio);
            };
            pin_item.description =
                "Pick which GPIO pin drives this slot (header view shown "
                "alongside). Pins held by hardware (HUB75/SPI/I\xC2\xB2""C…) are "
                "hidden; a pin already used by another slot stays selectable but "
                "is flagged \xE2\x80\x94 overriding it marks both slots red until "
                "you fix the clash. Selecting a pin returns here to set functions.";

            std::vector<MenuItem> pull_items = {
                leaf_sel("Pull Up",   [p]{ p->pull = 1; }, [p]{ return p->pull == 1; }),
                leaf_sel("Pull Down", [p]{ p->pull = 2; }, [p]{ return p->pull == 2; }),
                leaf_sel("None",      [p]{ p->pull = 0; }, [p]{ return p->pull == 0; }),
            };
            std::vector<MenuItem> items = {
                pin_item,
                submenu("Short Press", fn_picker(&p->short_fn)),
                submenu("Long Press",  fn_picker(&p->long_fn)),
                slider("Long-press Time", 200.f, 2000.f, 50.f, " ms",
                    [p]{ return static_cast<float>(p->long_ms); },
                    [p](float v){ p->long_ms = static_cast<int>(v); }),
                submenu("Pull", std::move(pull_items)),
                toggle("Active Low",
                    [p]{ return p->active_low; },
                    [p](bool v){ p->active_low = v; }),
            };
            MenuItem m = submenu(std::string("Slot ") + std::to_string(i + 1), std::move(items));
            m.label_fn = [p, i, gpio_slot_conflict]{
                std::string s = "Slot " + std::to_string(i + 1) + "  ";
                if (p->gpio < 0) return s + "(unused)";
                if (gpio_slot_conflict(i)) s = "! " + s;   // leading marker + red (warn_fn)
                s += "GPIO" + std::to_string(p->gpio) + " \xE2\x86\x92 ";
                s += input::gpio_func_name(p->short_fn);
                return s;
            };
            // Paint the row red when this slot's pin collides with another slot
            // or a hardware peripheral, so the user knows to go back and fix it.
            m.warn_fn = [i, gpio_slot_conflict]{ return gpio_slot_conflict(i); };
            gpio_btn_menu.push_back(std::move(m));
        }

        gpio_buttons_item = with_desc(
            submenu("GPIO Buttons", std::move(gpio_btn_menu)),
            "Assign functions to GPIO switches. Each slot's Pin page shows the "
            "40-pin header and offers only the pins free of other peripherals; "
            "pick one to set its Short-press and optional Long-press function, "
            "pull bias, and polarity (Active Low = switch wired to GND with a "
            "pull-up). Toggling Enabled applies instantly; after editing slots "
            "choose \"Apply Changes Now\" to reload the poller. Saved to "
            "cfg[\"gpio\"][\"pins\"].");
    } else {
        gpio_buttons_item.visible_fn = []{ return false; };
    }

    // ── Pi Settings helpers ──────────────────────────────────────────────────
    // Most of these shell out to systemd/raspi tools. The ones that mutate
    // system state (hostname, timezone, NTP, apt, power) need sudo; ship
    // /etc/sudoers.d/protohud (chmod 440) with:
    //
    //   <user> ALL=(ALL) NOPASSWD: /usr/bin/hostnamectl, /usr/bin/timedatectl, \
    //       /usr/bin/apt-get, /usr/sbin/poweroff, /usr/sbin/reboot, \
    //       /usr/bin/systemctl restart protohud.service
    //
    // …so the menu calls succeed without an interactive password prompt.
    auto run_sh = [push_notif](std::string title, std::string cmd) {
        std::thread([push_notif, title=std::move(title), cmd=std::move(cmd)]() {
            const std::string full = cmd + " 2>&1 | logger -t protohud";
            const int rc = std::system(full.c_str());
            push_notif(NotifType::App, title,
                       rc == 0 ? std::string("OK")
                               : ("Failed (rc " + std::to_string(rc) + ")"),
                       5.f);
        }).detach();
    };
    // Capture popen output synchronously (small reads only — used for
    // hostname, current timezone, df output).
    auto sh_read = [](const char* cmd) -> std::string {
        std::string out;
        FILE* f = ::popen(cmd, "r");
        if (!f) return out;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) out += buf;
        ::pclose(f);
        // Trim trailing whitespace/newlines.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    };

    // ── Pi Settings submenu ──────────────────────────────────────────────────
    std::vector<MenuItem> tz_items;
    {
        // A small curated set of common zones plus UTC. The user can add
        // more by hand-editing /etc/timezone; this list is just menu polish.
        static const char* kZones[] = {
            "UTC",
            "America/Los_Angeles", "America/Denver",
            "America/Chicago",     "America/New_York",
            "Europe/London",       "Europe/Berlin",
            "Europe/Paris",        "Asia/Tokyo",
            "Asia/Shanghai",       "Australia/Sydney",
        };
        for (const char* z : kZones) {
            tz_items.push_back(leaf_sel(z,
                [run_sh, z]{
                    run_sh(std::string("Timezone → ") + z,
                           std::string("sudo timedatectl set-timezone ") + z);
                },
                [sh_read, z]{
                    return sh_read("timedatectl show -p Timezone --value") == z;
                }));
        }
    }
    std::vector<MenuItem> pi_settings_items = {
        // Hostname — current value shown in the context panel; descend to change it.
        with_panel(
            submenu("Hostname", std::vector<MenuItem>{
                with_desc(leaf("Set Hostname\xE2\x80\xA6",
                    [menu_sys_pp, run_sh]{
                        if (!menu_sys_pp || !*menu_sys_pp) return;
                        char host[256] = {0};
                        if (::gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';
                        (*menu_sys_pp)->open_keyboard("Hostname", host,
                            [run_sh](const std::string& nm){
                                if (nm.empty()) return;
                                std::string safe;   // keep only hostname-legal chars
                                for (char c : nm)
                                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                        || (c >= '0' && c <= '9') || c == '-' || c == '.')
                                        safe.push_back(c);
                                if (safe.empty()) return;
                                run_sh("Hostname", "sudo hostnamectl set-hostname " + safe);
                            });
                    }),
                    "Change the Pi's hostname (hostnamectl). Applies to new logins."),
            }),
            "Hostname",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                (void)sz;
                char host[256] = {0};
                const char* hn = (::gethostname(host, sizeof(host) - 1) == 0 && host[0])
                                   ? host : "(unknown)";
                ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 0.95f, {o.x, o.y},
                            IM_COL32(150, 160, 170, 210), "Current hostname");
                dl->AddText(font, fs * 1.4f, {o.x, o.y + fs * 1.4f},
                            IM_COL32(225, 235, 245, 240), hn);
            }),
        // Time — timezone, automatic sync, and clock display settings together.
        with_desc(submenu("Time", std::vector<MenuItem>{
            with_desc(submenu("Timezone", std::move(tz_items)),
                "System timezone (timedatectl). Affects the clock, log timestamps "
                "and scheduled tasks."),
            with_desc(toggle("Auto Time Sync (NTP)",
                [sh_read]{ return sh_read("timedatectl show -p NTP --value") == "yes"; },
                [run_sh](bool v){
                    run_sh(v ? "NTP On" : "NTP Off",
                           std::string("sudo timedatectl set-ntp ") + (v ? "true" : "false"));
                }),
                "Automatic time sync via systemd-timesyncd. Off to set time manually."),
            submenu("Clock", std::vector<MenuItem>{
                toggle("24-Hour",   [&state]{ return state.clock_cfg.use_24h; },
                                    [&state](bool v){ state.clock_cfg.use_24h = v; }),
                toggle("Seconds",   [&state]{ return state.clock_cfg.show_seconds; },
                                    [&state](bool v){ state.clock_cfg.show_seconds = v; }),
                toggle("Show Date", [&state]{ return state.clock_cfg.show_date; },
                                    [&state](bool v){ state.clock_cfg.show_date = v; }),
                slider("Font Size", 1.f, 3.f, 0.25f, "x",
                    [&state]{ return state.clock_cfg.font_scale; },
                    [&state](float v){ state.clock_cfg.font_scale = v; }),
            }),
        }), "Timezone, automatic NTP sync, and clock display settings."),
        with_panel(
            // Storage usage context panel — runs df once per draw.
            submenu("Storage", std::vector<MenuItem>{}),
            "Disk Usage",
            [sh_read](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                dl->AddText(font, fs * 1.0f, {o.x, o.y},
                            IM_COL32(230, 235, 240, 255), "Disk Usage");
                const std::string out = sh_read("df -h /");
                float y = o.y + fs * 1.5f;
                size_t pos = 0;
                while (pos < out.size()) {
                    size_t nl = out.find('\n', pos);
                    if (nl == std::string::npos) nl = out.size();
                    const std::string line = out.substr(pos, nl - pos);
                    dl->AddText(font, fs * 0.85f, {o.x, y},
                                IM_COL32(200, 210, 220, 220), line.c_str());
                    y += fs;
                    pos = nl + 1;
                }
            }),
        // (Restart ProtoHUD + Shutdown moved to System > Power; the apt updater
        //  moved to System > Software. GPIO visualizer/buttons + cooling fans are
        //  appended to this list further below.)
    };

    // ── Updates submenu (in-HUD updater — Phase 1, user-initiated only) ───────
    // Pings the repo over plain git (no GitHub API): lists remote branches,
    // shows their change-notes/log in the context panel, and applies a chosen
    // branch via scripts/update.sh <branch> --restart. A pre-update rollback
    // point is recorded by update.sh; "Rollback" returns to it via
    // scripts/rollback.sh (also runnable standalone over SSH if the HUD won't
    // boot). POLICY: there is deliberately NO automatic update path here —
    // nothing fetches, builds or restarts unless the user picks it.
    struct UpdaterState {
        std::mutex  mtx;
        std::string root;          // repo root (resolved from /proc/self/exe)
        std::string cur_short;     // current HEAD short hash
        std::string cur_subject;   // current HEAD commit subject
        std::string cur_branch;    // current branch name
        std::vector<std::string> all_remote; // every remote branch (authoritative)
        std::vector<std::string> branches;  // filtered view shown in the menu
        bool        show_all = false;       // curated (main + claude/*) vs all
        int         behind   = -1;          // commits behind origin/<branch>; -1 unknown
        std::string last_check;             // human time of last fetch; "" = never
        std::string status   = "Not checked yet.";
        bool        busy     = false;       // an async git/update op is running
        int         sel      = -1;          // highlighted branch index (log panel)
        std::string sel_name;               // highlighted branch name
        std::string sel_log;                // cached git log for the highlighted branch
        bool        rollback_avail = false; // state/update/last_good.env exists
        std::string rollback_target;        // "hash on branch" for the label
    };
    auto upd = std::make_shared<UpdaterState>();
    {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            std::string p(exe);                                       // .../build/protohud
            auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
            s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
            upd->root = p;
        }
    }

    // Run `git -C <root> <args>` and capture stdout (local queries are fast;
    // network ones — fetch — are only ever called from a worker thread).
    auto git_q = [sh_read, upd](const std::string& args) -> std::string {
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        if (root.empty()) return {};
        const std::string cmd = "git -C '" + root + "' " + args;
        return sh_read(cmd.c_str());
    };

    // Refresh the current-version fields (local, instant).
    auto upd_refresh_cur = [git_q, upd]{
        std::string sh = git_q("rev-parse --short HEAD");
        std::string su = git_q("log -1 --pretty=%s");
        std::string br = git_q("rev-parse --abbrev-ref HEAD");
        std::lock_guard<std::mutex> lk(upd->mtx);
        upd->cur_short = sh; upd->cur_subject = su; upd->cur_branch = br;
    };

    // Recompute the visible list from the cached full list + curated filter.
    // Instant (no git) — safe to call on the UI thread when toggling the view.
    auto upd_apply_filter = [upd]{
        std::lock_guard<std::mutex> lk(upd->mtx);
        std::vector<std::string> out;
        for (const auto& ref : upd->all_remote) {
            if (!upd->show_all) {
                // Curated view: main/master plus the working claude/* branches.
                const bool keep = (ref == "main" || ref == "master"
                                   || ref.rfind("claude/", 0) == 0);
                if (!keep) continue;
            }
            out.push_back(ref);
        }
        upd->branches = std::move(out);
    };

    // Populate the full remote-branch list, then re-filter. local=true reads
    // local tracking refs (instant, but only branches already fetched);
    // local=false runs `git ls-remote --heads origin` (network — authoritative,
    // lists EVERY branch on the repo even if it was never fetched locally).
    auto upd_load_branches = [git_q, upd, upd_apply_filter](bool local){
        const std::string raw = local
            ? git_q("for-each-ref --format=%(refname:short) --sort=-committerdate refs/remotes/origin")
            : git_q("ls-remote --heads origin");
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t nl = raw.find('\n', pos);
            if (nl == std::string::npos) nl = raw.size();
            std::string line = raw.substr(pos, nl - pos);
            pos = nl + 1;
            std::string ref;
            if (local) {
                ref = line;
                const std::string pfx = "origin/";
                if (ref.rfind(pfx, 0) == 0) ref = ref.substr(pfx.size());
            } else {
                // ls-remote line: "<sha>\trefs/heads/<branch>"
                const auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                ref = line.substr(tab + 1);
                const std::string pfx = "refs/heads/";
                if (ref.rfind(pfx, 0) != 0) continue;
                ref = ref.substr(pfx.size());
            }
            if (ref.empty() || ref == "HEAD" || ref.find("HEAD ->") != std::string::npos) continue;
            out.push_back(ref);
        }
        // Don't clobber a good cached list with an empty network result
        // (offline / fetch failed) — but always accept the local read.
        if (!out.empty() || local) {
            std::lock_guard<std::mutex> lk(upd->mtx);
            upd->all_remote = std::move(out);
        }
        upd_apply_filter();
    };

    // Refresh rollback availability from the marker update.sh writes.
    auto upd_refresh_rollback = [sh_read, upd]{
        std::string root;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
        const std::string marker = root + "/state/update/last_good.env";
        const std::string out = sh_read(
            ("[ -f '" + marker + "' ] && . '" + marker +
             "' && printf '%s|%s' \"${LAST_GOOD_COMMIT:0:9}\" \"${LAST_GOOD_BRANCH}\"").c_str());
        std::lock_guard<std::mutex> lk(upd->mtx);
        if (out.empty()) { upd->rollback_avail = false; upd->rollback_target.clear(); return; }
        upd->rollback_avail = true;
        auto bar = out.find('|');
        upd->rollback_target = (bar == std::string::npos)
            ? out : (out.substr(0, bar) + " on " + out.substr(bar + 1));
    };

    // Initial population (local only — never auto-fetches the network).
    upd_refresh_cur();
    upd_load_branches(/*local=*/true);
    upd_refresh_rollback();

    // "Check for Updates": async fetch + behind-count. The ONLY network call,
    // and only when the user picks it.
    auto upd_check = [git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
        { std::lock_guard<std::mutex> lk(upd->mtx);
          if (upd->busy) return; upd->busy = true; upd->status = "Checking\xe2\x80\xa6"; }
        std::thread([git_q, upd_refresh_cur, upd_load_branches, sh_read, upd, push_notif]{
            git_q("fetch --prune origin");                 // network (retry handled by git config / best-effort)
            upd_refresh_cur();
            upd_load_branches(/*local=*/false);            // authoritative remote list
            std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
            const std::string beh = git_q("rev-list --count HEAD..origin/" + br);
            int behind = beh.empty() ? -1 : std::atoi(beh.c_str());
            const std::string now = sh_read("date '+%a %H:%M'");
            std::string msg = behind > 0
                ? (std::to_string(behind) + " new commit(s) on " + br)
                : (behind == 0 ? "Up to date." : "Checked (branch not on remote).");
            { std::lock_guard<std::mutex> lk(upd->mtx);
              upd->behind = behind; upd->last_check = now; upd->status = msg; upd->busy = false; }
            push_notif(NotifType::App, "Updates",
                       behind > 0 ? (std::to_string(behind) + " update(s) available")
                                  : "Already up to date", 5.f);
        }).detach();
    };

    // Apply a branch via update.sh <branch> --restart, detached so the build +
    // restart survive ProtoHUD being killed during the restart step.
    auto upd_apply = [upd, push_notif](const std::string& branch){
        std::string root; bool busy;
        { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; busy = upd->busy; }
        if (busy || root.empty() || branch.empty()) return;
        const std::string sp = root + "/scripts/update.sh";
        // setsid + bash -c '"$0" "$1" --restart' keeps the script in its own
        // session; $0/$1 carry the (quoted) script path + branch so neither
        // needs escaping in the -c string.
        const std::string cmd =
            "setsid bash -c '\"$0\" \"$1\" --restart' '" + sp + "' '" + branch +
            "' </dev/null >/tmp/protohud-update.log 2>&1 &";
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Update started",
                   "Pulling " + branch + " and rebuilding. The HUD restarts when "
                   "the build succeeds, and stays up if it fails. Log: "
                   "/tmp/protohud-update.log", 0.f);
    };

    // Roll back to the recorded known-good build via the standalone script.
    auto upd_rollback = [upd, push_notif]{
        std::string root; bool busy, avail;
        { std::lock_guard<std::mutex> lk(upd->mtx);
          root = upd->root; busy = upd->busy; avail = upd->rollback_avail; }
        if (busy || root.empty() || !avail) return;
        const std::string sp = root + "/scripts/rollback.sh";
        const std::string cmd =
            "setsid bash -c '\"$0\" --restart' '" + sp +
            "' </dev/null >/tmp/protohud-rollback.log 2>&1 &";
        std::system(cmd.c_str());
        push_notif(NotifType::App, "Rollback started",
                   "Restoring the last known-good build + config, then restarting. "
                   "Log: /tmp/protohud-rollback.log", 0.f);
    };

    std::vector<MenuItem> updates_menu;
    {
        // Current version + status panel.
        MenuItem cur = leaf("Current Version", []{});
        cur.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            std::string s = "On " + (upd->cur_branch.empty() ? "?" : upd->cur_branch)
                          + " @ " + (upd->cur_short.empty() ? "?" : upd->cur_short);
            if (upd->behind > 0) s += "  (" + std::to_string(upd->behind) + " behind)";
            return s;
        };
        updates_menu.push_back(with_panel(std::move(cur), "Update Status",
            [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::lock_guard<std::mutex> lk(upd->mtx);
                float y = o.y;
                auto line = [&](const std::string& t, ImU32 col, float scale){
                    dl->AddText(font, fs * scale, {o.x, y}, col, t.c_str());
                    y += fs * (scale + 0.25f);
                };
                line("Current build", IM_COL32(230, 235, 240, 255), 1.0f);
                line(upd->cur_branch.empty() ? "branch ?" : upd->cur_branch,
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line((upd->cur_short.empty() ? "?" : upd->cur_short) + "  " + upd->cur_subject,
                     IM_COL32(180, 190, 200, 210), 0.85f);
                y += fs * 0.4f;
                line(upd->last_check.empty() ? "Last checked: never"
                                             : ("Last checked: " + upd->last_check),
                     IM_COL32(200, 210, 220, 220), 0.85f);
                line(upd->status, upd->behind > 0 ? IM_COL32(120, 220, 140, 235)
                                                  : IM_COL32(200, 210, 220, 220), 0.85f);
            }));

        // Check for Updates (the only network action).
        MenuItem chk = with_desc(leaf("Check for Updates", [upd_check]{ upd_check(); }),
            "Fetch the latest commit list from the repo (git fetch) and report how "
            "far behind the current branch is. Does NOT change any code \xe2\x80\x94 nothing "
            "updates until you pick a branch below. This is the only step that uses "
            "the network.");
        chk.label_fn = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->busy ? std::string("Checking\xe2\x80\xa6")
                             : std::string("Check for Updates");
        };
        updates_menu.push_back(std::move(chk));

        // Update current branch in place.
        updates_menu.push_back(with_desc(
            leaf("Update This Branch & Restart",
                 [upd, upd_apply]{
                     std::string br; { std::lock_guard<std::mutex> lk(upd->mtx); br = upd->cur_branch; }
                     upd_apply(br);
                 }),
            "Pull the latest commits for the CURRENT branch, rebuild, and restart. "
            "Your settings (config.json) and custom faces/effects are preserved \xe2\x80\x94 "
            "they're outside version control. A rollback point is saved first."));

        // Select Branch submenu — dynamic slots fed by upd->branches, with a
        // level context panel showing the highlighted branch's change log.
        std::vector<MenuItem> branch_items;
        branch_items.push_back(with_desc(toggle("Show All Branches",
            [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->show_all; },
            [upd, upd_apply_filter, upd_load_branches](bool v){
                { std::lock_guard<std::mutex> lk(upd->mtx); upd->show_all = v; }
                upd_apply_filter();   // instant re-filter of the cached list
                // …then refresh the authoritative list from the repo so branches
                // that were never fetched locally also appear (network).
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "OFF: show only main + the claude/* working branches (recommended). "
            "ON: list every branch on the repo (refreshed over the network)."));
        branch_items.push_back(with_desc(
            leaf("Refresh List", [upd_load_branches]{
                std::thread([upd_load_branches]{ upd_load_branches(/*local=*/false); }).detach();
            }),
            "Re-query the repo for its full branch list (git ls-remote). Shows "
            "every branch, including ones not yet downloaded to this Pi."));

        static constexpr int kBranchSlots = 48;
        for (int i = 0; i < kBranchSlots; ++i) {
            MenuItem b = leaf("branch", []{});
            b.visible_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                return i < static_cast<int>(upd->branches.size());
            };
            b.label_fn = [upd, i]{
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (i >= static_cast<int>(upd->branches.size())) return std::string();
                std::string nm = upd->branches[i];
                if (nm == upd->cur_branch) nm += "  (current)";
                return nm;
            };
            // Load the change-log into the panel cache when highlighted.
            b.on_highlight = [upd, git_q, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  if (upd->sel == i) return;          // already cached
                  nm = upd->branches[i]; upd->sel = i; upd->sel_name = nm;
                  upd->sel_log = "Loading\xe2\x80\xa6"; }
                // Richer changelog: short hash + author-date + subject.
                std::string log = git_q(
                    "log --pretty=format:'%h %ad  %s' --date=short -n 20 origin/" + nm);
                std::string ahead  = git_q("rev-list --count HEAD..origin/" + nm);
                std::string behind = git_q("rev-list --count origin/" + nm + "..HEAD");
                std::lock_guard<std::mutex> lk(upd->mtx);
                if (upd->sel != i) return;            // moved on while loading
                std::string hdr;
                if (!ahead.empty() && ahead != "0")  hdr += "+" + ahead + " new commit(s) to apply";
                if (!behind.empty() && behind != "0") hdr += (hdr.empty() ? "" : "  ") +
                                                             ("-" + behind + " not on this branch");
                if (hdr.empty() && !ahead.empty())   hdr = "Even with your current build";
                upd->sel_log = (hdr.empty() ? "" : (hdr + "\n\n"))
                             + (log.empty() ? "(no log \xe2\x80\x94 run Check for Updates to fetch it)" : log);
            };
            b.action = [upd, upd_apply, i]{
                std::string nm;
                { std::lock_guard<std::mutex> lk(upd->mtx);
                  if (i >= static_cast<int>(upd->branches.size())) return;
                  nm = upd->branches[i]; }
                upd_apply(nm);
            };
            b.description = "Select to update to this branch, rebuild, and restart. "
                            "The panel shows its recent commits. Settings + custom "
                            "content are preserved; a rollback point is saved first.";
            branch_items.push_back(std::move(b));
        }

        MenuItem branch_sub = submenu("Select Branch", std::move(branch_items));
        updates_menu.push_back(with_desc(
            with_panel(std::move(branch_sub), "Branch Changes",
                [upd](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                    (void)sz;
                    ImFont* font = ImGui::GetFont();
                    const float fs = ImGui::GetFontSize();
                    std::lock_guard<std::mutex> lk(upd->mtx);
                    dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                                upd->sel_name.empty() ? "Highlight a branch"
                                                      : upd->sel_name.c_str());
                    float y = o.y + fs * 1.5f;
                    const std::string& t = upd->sel_log;
                    size_t pos = 0;
                    while (pos < t.size()) {
                        size_t nl = t.find('\n', pos);
                        if (nl == std::string::npos) nl = t.size();
                        const std::string ln = t.substr(pos, nl - pos);
                        dl->AddText(font, fs * 0.8f, {o.x, y},
                                    IM_COL32(200, 210, 220, 220), ln.c_str());
                        y += fs * 0.95f;
                        pos = nl + 1;
                    }
                }),
            "Choose a branch to update to. Highlight one to read its recent commits "
            "in the panel, then select to apply (rebuild + restart)."));

        // Rollback — visible only when a recorded known-good point exists.
        MenuItem rb = with_desc(leaf("Rollback Last Update",
            [upd_rollback]{ upd_rollback(); }),
            "Restore the build + config saved just before your last update, then "
            "rebuild and restart. If the HUD won't boot, run scripts/rollback.sh "
            "over SSH to do the same from outside ProtoHUD.");
        rb.visible_fn = [upd]{ std::lock_guard<std::mutex> lk(upd->mtx); return upd->rollback_avail; };
        rb.label_fn   = [upd]{
            std::lock_guard<std::mutex> lk(upd->mtx);
            return upd->rollback_target.empty()
                ? std::string("Rollback Last Update")
                : ("Rollback to " + upd->rollback_target);
        };
        updates_menu.push_back(std::move(rb));

        // Update History — reads state/update/history.log (written by update.sh),
        // newest first. Each line: ISO-date \t branch \t before..after \t subject.
        MenuItem hist = leaf("Update History", []{});
        hist.type = MenuItemType::SUBMENU;   // submenu so its context panel shows
        updates_menu.push_back(with_panel(std::move(hist), "Update History",
            [upd, sh_read](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                std::string root;
                { std::lock_guard<std::mutex> lk(upd->mtx); root = upd->root; }
                const std::string log = root.empty() ? std::string() : sh_read(
                    ("f='" + root + "/state/update/history.log'; "
                     "[ -f \"$f\" ] && tac \"$f\" | head -n 18").c_str());
                dl->AddText(font, fs * 1.0f, {o.x, o.y}, IM_COL32(230, 235, 240, 255),
                            "Recent updates (newest first)");
                float y = o.y + fs * 1.6f;
                if (log.empty()) {
                    dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(180, 190, 200, 210),
                                "No updates recorded yet.");
                    return;
                }
                size_t pos = 0;
                while (pos < log.size()) {
                    size_t nl = log.find('\n', pos);
                    if (nl == std::string::npos) nl = log.size();
                    std::string ln = log.substr(pos, nl - pos);
                    pos = nl + 1;
                    // Split the tab-separated record into a compact two-line entry.
                    std::vector<std::string> f; size_t p = 0;
                    while (true) { size_t t = ln.find('\t', p);
                        f.push_back(ln.substr(p, t == std::string::npos ? std::string::npos : t - p));
                        if (t == std::string::npos) break; p = t + 1; }
                    std::string when = f.size() > 0 ? f[0] : ln;
                    if (when.size() >= 16) when = when.substr(0, 16).replace(10, 1, " "); // YYYY-MM-DD HH:MM
                    const std::string br  = f.size() > 1 ? f[1] : "";
                    const std::string rng = f.size() > 2 ? f[2] : "";
                    const std::string sub = f.size() > 3 ? f[3] : "";
                    dl->AddText(font, fs * 0.82f, {o.x, y}, IM_COL32(210, 220, 230, 230),
                                (when + "   " + br + "  " + rng).c_str());
                    y += fs * 0.9f;
                    if (!sub.empty()) {
                        dl->AddText(font, fs * 0.78f, {o.x + fs * 0.8f, y},
                                    IM_COL32(170, 180, 190, 200), sub.c_str());
                        y += fs * 0.95f;
                    }
                    y += fs * 0.15f;
                }
            }));

        // Settings & Data Safety — a static explainer of what survives an update.
        MenuItem safe = leaf("Settings & Data Safety", []{});
        safe.type = MenuItemType::SUBMENU;
        updates_menu.push_back(with_panel(std::move(safe), "What's Preserved",
            [](ImDrawList* dl, ImVec2 o, ImVec2 sz) {
                (void)sz;
                ImFont* font = ImGui::GetFont();
                const float fs = ImGui::GetFontSize();
                static const char* kLines[] = {
                    "Updates never touch your data:",
                    "",
                    "\xe2\x80\xa2 config.json (all menu settings) is outside",
                    "  version control \xe2\x80\x94 git can't overwrite it.",
                    "\xe2\x80\xa2 New defaults from a release are merged IN;",
                    "  your existing values always win.",
                    "\xe2\x80\xa2 Custom faces/effects + IMU calibration",
                    "  live in config.json / Protoface and persist.",
                    "\xe2\x80\xa2 Protoface (imported faces) updates without",
                    "  deleting anything you imported.",
                    "",
                    "Before each update a rollback point is saved",
                    "(commit + a config backup under state/update/).",
                    "Recover anytime via Rollback, or over SSH with",
                    "scripts/rollback.sh --restart.",
                };
                float y = o.y;
                for (const char* ln : kLines) {
                    const bool head = (ln[0] != '\0' && ln[0] != ' ' && ln[0] != '\xe2');
                    dl->AddText(font, fs * (head ? 0.92f : 0.82f), {o.x, y},
                                head ? IM_COL32(230, 235, 240, 255)
                                     : IM_COL32(195, 205, 215, 220), ln);
                    y += fs * 0.95f;
                }
            }));
    }

    // ── Power submenu (existing reboot/quit + new shutdown moved here) ──────
    // Confirm wrapper: a destructive action becomes a submenu the user has to
    // descend into and pick "Confirm …" before it fires — guards against an
    // accidental select on Reboot Pi / Shutdown.
    auto confirm_action = [&](const char* label, const char* warn,
                              std::function<void()> act) -> MenuItem {
        std::vector<MenuItem> kids;
        kids.push_back(with_desc(leaf(std::string("Confirm: ") + label, std::move(act)), warn));
        MenuItem m = submenu(label, std::move(kids));
        m.description = warn;
        return m;
    };
    // Relaunch ProtoHUD via scripts/restart.sh (resolved from /proc/self/exe),
    // detached in its own session so it survives this process being killed.
    auto restart_protohud = [] {
        char exe[4096];
        ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        std::string p(exe);
        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);  // .../build
        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);  // project root
        const std::string script = p + "/scripts/restart.sh";
        std::system(("setsid \"" + script + "\" >/tmp/protohud.log 2>&1 </dev/null &").c_str());
    };
    std::vector<MenuItem> power_menu = {
        with_desc(leaf("Reboot ProtoHUD", restart_protohud),
            "Relaunch ProtoHUD (stop + start via scripts/restart.sh). The display "
            "blanks briefly while it comes back."),
        with_desc(leaf("Reboot ProtoFace", [teensy]{ if (teensy) teensy->restart(); }),
            "Restart the face backend (Protoface daemon / native renderer) without "
            "touching the rest of the HUD."),
        confirm_action("Reboot Pi",
            "Reboots the whole Raspberry Pi after a short delay. Unsaved work is lost.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n reboot 2>/dev/null || reboot 2>/dev/null &");
                }).detach();
            }),
        confirm_action("Shutdown",
            "Powers off the Raspberry Pi. You'll need to cycle power to start it again.",
            [&state]{
                state.quit = true;
                std::thread([]{
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::system("sudo -n poweroff 2>/dev/null || poweroff 2>/dev/null &");
                }).detach();
            }),
        with_desc(leaf("Close Program", [&state]{ state.quit = true; }),
            "Exit ProtoHUD without rebooting the system."),
        with_desc(toggle("Skip Startup Screen",
            [&state]{ return state.skip_landing; },
            [&state](bool v){ state.skip_landing = v; }),
            "Bypass the profile/continue landing screen at boot and run the current "
            "config directly. Takes effect on the next launch."),
    };

    // ── Display & HUD submenu (cosmetic / per-window controls) ───────────────
    std::vector<MenuItem> display_hud_menu = {
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
        with_desc(slider("Text Size", 0.7f, 2.0f, 0.1f, "x",
            [hud_cfg]{ return hud_cfg->text_scale; },
            [hud_cfg](float v){ hud_cfg->text_scale = v; }),
            "Scale factor applied to all HUD text. Lower = more on-screen at once."),
        // Window mode (Fullscreen/Frameless) + Resolution moved to System > Display.
        // Legacy HUD toggle moved to HUD > Legacy HUD (grouped with its settings).
    };

    // ── Display submenu (window mode + windowed resolution) ───────────────────
    // Desktop/dev only — ignored while the glasses drive the window.
    std::vector<MenuItem> resolution_menu;
    {
        static const struct { const char* l; int w, h; } kRes[] = {
            {"1280 x 720",  1280,  720}, {"1600 x 900",  1600,  900},
            {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
            {"3840 x 1080 (SBS)", 3840, 1080}, {"3840 x 2160", 3840, 2160},
        };
        for (const auto& r : kRes) {
            const int w = r.w, h = r.h;
            resolution_menu.push_back(leaf(r.l, [&state, w, h]{
                state.win_resize_w.store(w); state.win_resize_h.store(h);
                state.win_resize_dirty.store(true);
            }));
        }
    }
    std::vector<MenuItem> display_menu = {
        with_desc(toggle("Fullscreen",
            [&state]{ return state.win_fullscreen.load(); },
            [&state](bool v){ state.win_fullscreen.store(v); state.win_mode_dirty.store(true); }),
            "Borderless fullscreen covering the whole screen. Desktop/dev only \xe2\x80\x94 "
            "ignored while the glasses are connected. Applied live."),
        with_desc(toggle("Frameless Window",
            [&state]{ return state.win_frameless.load(); },
            [&state](bool v){ state.win_frameless.store(v); state.win_mode_dirty.store(true); }),
            "Remove the OS window title bar and borders (windowed mode). Desktop/dev "
            "only. Applied live."),
        with_desc(submenu("Resolution", std::move(resolution_menu)),
            "Resize the windowed (non-fullscreen) output. Desktop/dev only; ignored "
            "while the glasses drive the display."),
    };

    // ── Connectivity submenu ─────────────────────────────────────────────────
    // ── Bluetooth controls (bluetoothctl shell-out) ──────────────────────────
    // Scan/power + per-device connect/pair/trust/remove. Devices come from
    // BtMonitor (state.bt_devices), which now also lists discovered-but-unpaired
    // devices after a scan so they can be paired here.
    auto bt_cmd = [&state, bt_mon](int i, const char* verb) {
        std::string mac;
        { std::lock_guard<std::mutex> lk(state.mtx);
          if (i >= 0 && i < (int)state.bt_devices.size()) mac = state.bt_devices[i].mac; }
        if (mac.empty()) return;
        std::system((std::string("bluetoothctl ") + verb + " " + mac +
                     " >/dev/null 2>&1 &").c_str());
        if (bt_mon) bt_mon->refresh();
    };
    std::vector<MenuItem> bluetooth_menu;
    bluetooth_menu.push_back(with_desc(leaf("Scan for Devices", [bt_mon]{
        std::system("bluetoothctl --timeout 20 scan on >/dev/null 2>&1 &");
        if (bt_mon) bt_mon->refresh();
    }), "Discover nearby devices for ~20s; new ones appear in the list below to pair."));
    bluetooth_menu.push_back(with_desc(leaf("Power On", [bt_mon]{
        std::system("bluetoothctl power on >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter on."));
    bluetooth_menu.push_back(with_desc(leaf("Power Off", [bt_mon]{
        std::system("bluetoothctl power off >/dev/null 2>&1 &"); if (bt_mon) bt_mon->refresh(); }),
        "Turn the Bluetooth adapter off."));
    bluetooth_menu.push_back(with_desc(leaf("Refresh", [bt_mon]{ if (bt_mon) bt_mon->refresh(); }),
        "Re-scan known devices and update the list now."));
    for (int i = 0; i < 12; ++i) {
        MenuItem dev; dev.type = MenuItemType::SUBMENU; dev.label = "btdev";
        dev.label_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)state.bt_devices.size()) return std::string();
            const auto& d = state.bt_devices[i];
            return d.name + (d.connected ? "  \xE2\x9C\x93" : d.paired ? "  (paired)" : "  (new)");
        };
        dev.visible_fn = [&state, i]{
            std::lock_guard<std::mutex> lk(state.mtx); return i < (int)state.bt_devices.size(); };
        dev.children = {
            with_desc(leaf("Connect",    [bt_cmd, i]{ bt_cmd(i, "connect"); }),
                "Connect to this device."),
            with_desc(leaf("Disconnect", [bt_cmd, i]{ bt_cmd(i, "disconnect"); }),
                "Disconnect this device."),
            with_desc(leaf("Pair",       [bt_cmd, i]{ bt_cmd(i, "pair"); }),
                "Pair with this device (confirm any prompt on both ends)."),
            with_desc(leaf("Trust",      [bt_cmd, i]{ bt_cmd(i, "trust"); }),
                "Mark trusted so it can auto-reconnect."),
            with_desc(leaf("Remove",     [bt_cmd, i]{ bt_cmd(i, "remove"); }),
                "Unpair and forget this device."),
        };
        bluetooth_menu.push_back(std::move(dev));
    }

    std::vector<MenuItem> connectivity_menu = {
        with_desc(toggle("SSH Access",
            [state_ptr]{ return state_ptr && state_ptr->ssh.active; },
            [state_ptr](bool v){
                system(v ? "systemctl start ssh 2>&1 | logger -t protohud &"
                         : "systemctl stop ssh 2>&1 | logger -t protohud &");
                if (state_ptr) {
                    std::lock_guard<std::mutex> lk(state_ptr->mtx);
                    state_ptr->ssh.active = v;
                }
            }),
            "Toggle the sshd service. ON exposes the Pi to network logins on TCP 22."),
        with_desc(submenu("Bluetooth", std::move(bluetooth_menu)),
                  "Scan, pair, connect and manage Bluetooth devices."),
    };

    // ── Diagnostics submenu (probes + overlays + per-tab tools) ──────────────
    std::vector<MenuItem> diagnostics_group_menu = {
        with_desc(toggle("System Panel",
            [sys_panel_active]{ return sys_panel_active && *sys_panel_active; },
            [sys_panel_active](bool v){ if (sys_panel_active) *sys_panel_active = v; }),
            "Live system overlay (CPU, RAM, GPU temp, network ping)."),
        with_desc(toggle("FPS Overlay",
            [fps_overlay_active]{ return fps_overlay_active && *fps_overlay_active; },
            [fps_overlay_active](bool v){ if (fps_overlay_active) *fps_overlay_active = v; }),
            "Show the current frame rate as an overlay."),
        with_desc(submenu("FPS Average",   std::move(fps_interval_menu)),
                  "Averaging window for the FPS readout."),
        with_desc(submenu("Diagnostics",   std::move(diagnostics_menu)),
                  "Camera / network / Bluetooth / GPIO probes."),
        with_desc(leaf("Request Status", [teensy]{ teensy->request_status(); }),
                  "Poll the face controller for a fresh status frame."),
    };
    // Moved out of Diagnostics per the menu reorg: the GPIO visualizer + buttons
    // and the cooling-fan controls live under Pi Settings (hardware); Demo Mode
    // lives under Software (appended at the tail of this block).
    pi_settings_items.push_back(std::move(gpio_viz_item));
    pi_settings_items.push_back(std::move(gpio_buttons_item));
    // Cooling fans (Pi-driven PWM). Hidden when no FanController is wired.
    pi_settings_items.push_back([&]() -> MenuItem {
            if (!fans || fans->zone_count() == 0) {
                MenuItem m = leaf("Cooling Fans", []{}); m.visible_fn = []{ return false; }; return m;
            }
            // Per-zone speed/mode/curve submenu.
            auto build_zone = [&](int z) -> MenuItem {
                std::vector<MenuItem> mode_items = {
                    leaf_sel("Manual", [fans, z]{ fans->set_zone_auto(z, false); },
                                       [fans, z]{ return !fans->zone_auto(z); }),
                    leaf_sel("Auto (by CPU temp)", [fans, z]{ fans->set_zone_auto(z, true); },
                                       [fans, z]{ return fans->zone_auto(z); }),
                };
                MenuItem speed = slider("Speed", 0.f, 100.f, 5.f, "%",
                    [fans, z]{ return static_cast<float>(fans->zone_speed(z) * 100.0); },
                    [fans, z](float v){ fans->set_zone_speed(z, v / 100.0); });
                speed.visible_fn = [fans, z]{ return !fans->zone_auto(z); };
                MenuItem amin = slider("Auto Min Temp", 30.f, 80.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_min(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, v, fans->zone_auto_max(z)); });
                amin.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem amax = slider("Auto Max Temp", 40.f, 90.f, 1.f, "\xc2\xb0""C",
                    [fans, z]{ return static_cast<float>(fans->zone_auto_max(z)); },
                    [fans, z](float v){ fans->set_zone_auto_range(z, fans->zone_auto_min(z), v); });
                amax.visible_fn = [fans, z]{ return fans->zone_auto(z); };
                MenuItem status = leaf("Output", []{});
                status.label_fn = [fans, z]{
                    char b[40];
                    std::snprintf(b, sizeof(b), "Output: %d%%",
                                  (int)std::lround(fans->zone_duty(z) * 100.0));
                    return std::string(b);
                };
                std::vector<MenuItem> zi = {
                    submenu("Mode", std::move(mode_items)),
                    std::move(speed), std::move(amin), std::move(amax), std::move(status),
                };
                MenuItem m = submenu(fans->zone_name(z), std::move(zi));
                m.label_fn = [fans, z]{
                    char b[48];
                    std::snprintf(b, sizeof(b), "%s  [%d%%%s]", fans->zone_name(z).c_str(),
                                  (int)std::lround(fans->zone_duty(z) * 100.0),
                                  fans->zone_auto(z) ? " auto" : "");
                    return std::string(b);
                };
                return m;
            };
            std::vector<MenuItem> fan_items;
            fan_items.push_back(with_desc(toggle("Enabled",
                [fans]{ return fans->running(); },
                [fans](bool v){ if (v) fans->start(); else fans->stop(); }),
                "Drive the cooling fans on their configured GPIO. Off releases the lines."));
            for (int z = 0; z < fans->zone_count(); ++z)
                fan_items.push_back(build_zone(z));
            MenuItem temp = leaf("CPU Temp", []{});
            temp.label_fn = [fans]{
                char b[32]; std::snprintf(b, sizeof(b), "CPU Temp: %.0f\xc2\xb0""C",
                                          fans->current_temp_c());
                return std::string(b);
            };
            fan_items.push_back(std::move(temp));
            return with_desc(submenu("Cooling Fans", std::move(fan_items)),
                "Pi-driven PWM cooling fans \xe2\x80\x94 up to 4 fans in 2 zones, each "
                "with its own speed/mode. Pins in config[\"fans\"][\"zones\"]; use "
                "GPIO clear of HUB75 (see carrier PINMAP).");
        }());
    software_menu.push_back(with_desc(submenu("Demo Mode", std::move(demo_menu)),
        "Cycle prefab scenes for screenshots / video."));

    // ── Communications: LoRa + Phone (KDE Connect) + Notification Log ─────────
    MenuItem phone_item = [&]() -> MenuItem {
            // ── Phone (KDE Connect) ───────────────────────────────────────────
            // Ring the phone + edit the ignore list (mute servers/chats) and the
            // message-apps list (which apps get the big chat toast), from the HUD.
            auto ring_toast = [kdc_p, &state]{
                const bool ok = kdc_p && kdc_p->ring_phone();
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n;
                n.type = NotifType::App; n.icon = "message";
                n.title = ok ? "Ringing phone\xE2\x80\xA6" : "Phone not connected";
                n.body  = ok ? "KDE Connect \xC2\xB7 findmyphone"
                             : "Pair a device in the KDE Connect app first";
                n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };
            // Generic CSV-list editor: an "Add… (keyboard)" row plus up to 16
            // removable entry rows. apply() pushes the joined CSV to the bridge.
            // menu_sys_pp is captured by value (it points at main's menu_ptr).
            auto list_menu = [menu_sys_pp](std::vector<std::string>* vec,
                                           std::function<void()> apply,
                                           std::string add_label, std::string osk_title) {
                std::vector<MenuItem> items;
                MenuItem add; add.type = MenuItemType::LEAF; add.label = std::move(add_label);
                add.action = [menu_sys_pp, vec, apply, osk_title]{
                    if (!menu_sys_pp || !*menu_sys_pp || !vec) return;
                    (*menu_sys_pp)->open_keyboard(osk_title, "",
                        [vec, apply](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            vec->push_back(s.substr(b, e - b + 1));
                            apply();
                        });
                };
                items.push_back(std::move(add));
                for (int i = 0; i < 16; ++i) {
                    MenuItem m; m.type = MenuItemType::LEAF; m.label = "entry";
                    m.label_fn   = [vec, i]{
                        return (vec && i < static_cast<int>(vec->size()))
                                   ? ("\xE2\x9C\x95  " + (*vec)[i]) : std::string(); };
                    m.visible_fn = [vec, i]{ return vec && i < static_cast<int>(vec->size()); };
                    m.action     = [vec, i, apply]{
                        if (vec && i < static_cast<int>(vec->size())) {
                            vec->erase(vec->begin() + i); apply(); } };
                    items.push_back(std::move(m));
                }
                return items;
            };
            auto join = [](std::vector<std::string>* v){
                std::string csv;
                if (v) for (size_t i = 0; i < v->size(); ++i) { if (i) csv += ','; csv += (*v)[i]; }
                return csv;
            };
            auto apply_ignore = [kdc_p, kdc_ignore_p, join]{
                if (kdc_p) kdc_p->set_ignore_list(join(kdc_ignore_p)); };
            auto apply_msg = [kdc_p, kdc_msg_p, join]{
                if (kdc_p) kdc_p->set_message_apps(join(kdc_msg_p)); };

            // Small toast helper for action feedback.
            auto toast = [&state](std::string t, std::string b){
                std::lock_guard<std::mutex> lk(state.mtx);
                Notification n; n.type = NotifType::App; n.icon = "message";
                n.title = std::move(t); n.body = std::move(b); n.auto_dismiss_s = 4.f;
                state.notifs.push(std::move(n));
            };

            // ── Notifications (reply / dismiss) ────────────────────────────────
            // Each row mirrors a recent phone notification; Reply (messaging apps)
            // opens the OSK, Dismiss clears it on the phone.
            std::vector<MenuItem> notif_menu;
            notif_menu.push_back(with_desc(leaf("Dismiss All", [kdc_p, toast]{
                if (kdc_p && kdc_p->dismiss_notification("")) toast("Dismissed", "Cleared phone notifications");
            }), "Dismiss every active notification on the phone."));
            for (int i = 0; i < 12; ++i) {
                MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "notif";
                row.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return std::string();
                    const auto& n = v[i];
                    std::string who = !n.title.empty() ? n.title
                                    : (!n.app.empty() ? n.app : std::string("Phone"));
                    return who + (n.repliable ? "  \xE2\x9C\x89" : "");
                };
                row.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->phone_notifications().size(); };
                MenuItem reply = leaf("Reply\xE2\x80\xA6", [kdc_p, i, menu_sys_pp, toast]{
                    if (!kdc_p || !menu_sys_pp || !*menu_sys_pp) return;
                    auto v = kdc_p->phone_notifications();
                    if (i >= (int)v.size()) return;
                    const std::string id = v[i].id, who = v[i].title;
                    (*menu_sys_pp)->open_keyboard("Reply to " + who, "",
                        [kdc_p, id, toast](const std::string& s){
                            if (s.empty()) return;
                            if (kdc_p->reply_notification(id, s)) toast("Reply sent", s);
                        });
                });
                reply.visible_fn = [kdc_p, i]{
                    auto v = kdc_p ? kdc_p->phone_notifications() : std::vector<integrations::KdeConnectBridge::PhoneNotif>{};
                    return i < (int)v.size() && v[i].repliable; };
                MenuItem dismiss = leaf("Dismiss", [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->phone_notifications();
                    if (i < (int)v.size() && kdc_p->dismiss_notification(v[i].id))
                        toast("Dismissed", v[i].title);
                });
                // Context panel: show the full body text of the focused notification.
                row.children = { std::move(reply), std::move(dismiss) };
                notif_menu.push_back(std::move(row));
            }

            // ── Media control (mprisremote) ────────────────────────────────────
            std::vector<MenuItem> media_menu;
            {
                MenuItem now; now.type = MenuItemType::LEAF; now.label = "now playing";
                now.label_fn = [kdc_p]{
                    if (!kdc_p) return std::string("No media");
                    auto m = kdc_p->media_status();
                    if (!m.has_player) return std::string("No media");
                    std::string s = m.playing ? "\xE2\x96\xB6 " : "\xE2\x9D\x9A ";
                    s += !m.title.empty() ? m.title : std::string("(unknown)");
                    if (!m.artist.empty()) s += " \xE2\x80\x94 " + m.artist;
                    return s;
                };
                media_menu.push_back(std::move(now));
                auto act = [kdc_p](const char* a){ if (kdc_p) kdc_p->media_action(a); };
                media_menu.push_back(leaf("Play / Pause", [act]{ act("PlayPause"); }));
                media_menu.push_back(leaf("Next",         [act]{ act("Next"); }));
                media_menu.push_back(leaf("Previous",     [act]{ act("Previous"); }));
                media_menu.push_back(leaf("Stop",         [act]{ act("Stop"); }));
                media_menu.push_back(slider("Volume", 0.f, 100.f, 5.f, "%",
                    [kdc_p]{ auto m = kdc_p ? kdc_p->media_status() : integrations::KdeConnectBridge::MediaStatus{};
                             return (float)(m.volume < 0 ? 0 : m.volume); },
                    [kdc_p](float v){ if (kdc_p) kdc_p->media_set_volume((int)v); }));
                // Player picker (up to 6).
                std::vector<MenuItem> players;
                for (int i = 0; i < 6; ++i) {
                    MenuItem p; p.type = MenuItemType::LEAF; p.label = "player";
                    p.label_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        if (i >= (int)m.players.size()) return std::string();
                        return m.players[i] + (m.players[i] == m.player ? "  \xE2\x97\x8F" : ""); };
                    p.visible_fn = [kdc_p, i]{ auto m = kdc_p ? kdc_p->media_status()
                            : integrations::KdeConnectBridge::MediaStatus{};
                        return i < (int)m.players.size(); };
                    p.action = [kdc_p, i]{ if (!kdc_p) return; auto m = kdc_p->media_status();
                        if (i < (int)m.players.size()) kdc_p->media_set_player(m.players[i]); };
                    players.push_back(std::move(p));
                }
                media_menu.push_back(submenu("Player", std::move(players)));
            }

            // ── Run commands (remotecommands) ──────────────────────────────────
            std::vector<MenuItem> cmd_menu;
            for (int i = 0; i < 16; ++i) {
                MenuItem c; c.type = MenuItemType::LEAF; c.label = "command";
                c.label_fn = [kdc_p, i]{ auto v = kdc_p ? kdc_p->run_commands()
                        : std::vector<integrations::KdeConnectBridge::RunCommand>{};
                    return i < (int)v.size() ? v[i].name : std::string(); };
                c.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->run_commands().size(); };
                c.action = [kdc_p, i, toast]{ if (!kdc_p) return; auto v = kdc_p->run_commands();
                    if (i < (int)v.size() && kdc_p->run_command(v[i].key)) toast("Sent", v[i].name); };
                cmd_menu.push_back(std::move(c));
            }

            // ── Grouped ignore picker (app → senders; red = muted) ─────────────
            auto roster = [kdc_p]{ return kdc_p ? kdc_p->notif_roster()
                : std::vector<std::pair<std::string, std::vector<std::string>>>{}; };
            auto ig_has = [kdc_ignore_p](const std::string& s){
                return kdc_ignore_p && std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s)
                                       != kdc_ignore_p->end(); };
            auto ig_toggle = [kdc_ignore_p, apply_ignore](const std::string& s){
                if (!kdc_ignore_p || s.empty()) return;
                auto it = std::find(kdc_ignore_p->begin(), kdc_ignore_p->end(), s);
                if (it != kdc_ignore_p->end()) kdc_ignore_p->erase(it);
                else                           kdc_ignore_p->push_back(s);
                apply_ignore();
            };
            auto build_app_senders = [roster, ig_has, ig_toggle](int ai){
                std::vector<MenuItem> kids;
                MenuItem all; all.type = MenuItemType::TOGGLE; all.label = "Mute whole app";
                all.get_toggle = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                all.set_toggle = [roster, ig_toggle, ai](bool){ auto r = roster();
                    if (ai < (int)r.size()) ig_toggle(r[ai].first); };
                all.warn_fn = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                kids.push_back(std::move(all));
                for (int si = 0; si < 24; ++si) {
                    MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
                    m.label_fn   = [roster, ai, si]{ auto r = roster();
                        return (ai < (int)r.size() && si < (int)r[ai].second.size())
                                   ? r[ai].second[si] : std::string(); };
                    m.visible_fn = [roster, ai, si]{ auto r = roster();
                        return ai < (int)r.size() && si < (int)r[ai].second.size(); };
                    m.get_toggle = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    m.set_toggle = [roster, ig_toggle, ai, si](bool){ auto r = roster();
                        if (ai < (int)r.size() && si < (int)r[ai].second.size())
                            ig_toggle(r[ai].second[si]); };
                    m.warn_fn    = [roster, ig_has, ai, si]{ auto r = roster();
                        if (ai >= (int)r.size() || si >= (int)r[ai].second.size()) return false;
                        return ig_has(r[ai].second[si]); };
                    kids.push_back(std::move(m));
                }
                return kids;
            };
            std::vector<MenuItem> ignore_menu;
            ignore_menu.push_back(with_desc(leaf("Add Word\xE2\x80\xA6 (keyboard)",
                [menu_sys_pp, kdc_ignore_p, apply_ignore]{
                    if (!menu_sys_pp || !*menu_sys_pp || !kdc_ignore_p) return;
                    (*menu_sys_pp)->open_keyboard("Ignore (matches title/text)", "",
                        [kdc_ignore_p, apply_ignore](const std::string& s){
                            const size_t b = s.find_first_not_of(" \t");
                            const size_t e = s.find_last_not_of(" \t");
                            if (b == std::string::npos) return;
                            kdc_ignore_p->push_back(s.substr(b, e - b + 1)); apply_ignore();
                        });
                }), "Add a free-text mute (matches a notification's title/text)."));
            ignore_menu.push_back(with_desc(leaf("Unmute All",
                [kdc_ignore_p, apply_ignore]{ if (kdc_ignore_p) { kdc_ignore_p->clear(); apply_ignore(); } }),
                "Clear every mute."));
            for (int ai = 0; ai < 24; ++ai) {
                MenuItem appsub = submenu("app", build_app_senders(ai));
                appsub.label_fn   = [roster, ai]{ auto r = roster();
                    return ai < (int)r.size() ? r[ai].first : std::string(); };
                appsub.visible_fn = [roster, ai]{ return ai < (int)roster().size(); };
                appsub.warn_fn    = [roster, ig_has, ai]{ auto r = roster();
                    return ai < (int)r.size() && ig_has(r[ai].first); };
                ignore_menu.push_back(std::move(appsub));
            }

            // ── SMS (send) ─────────────────────────────────────────────────────
            auto send_sms_flow = [menu_sys_pp, kdc_p, toast]{
                if (!menu_sys_pp || !*menu_sys_pp || !kdc_p) return;
                (*menu_sys_pp)->open_keyboard("SMS to (number)", "",
                    [menu_sys_pp, kdc_p, toast](const std::string& num){
                        if (num.empty() || !menu_sys_pp || !*menu_sys_pp) return;
                        (*menu_sys_pp)->open_keyboard("Message", "",
                            [kdc_p, num, toast](const std::string& msg){
                                if (msg.empty()) return;
                                if (kdc_p->send_sms(num, msg)) toast("SMS sent", num + ": " + msg);
                            });
                    });
            };

            // ── Connectivity readout ───────────────────────────────────────────
            MenuItem conn_item; conn_item.type = MenuItemType::LEAF; conn_item.label = "Signal";
            conn_item.label_fn = [kdc_p]{
                if (!kdc_p) return std::string("Signal: n/a");
                auto c = kdc_p->connectivity();
                if (!c.ok) return std::string("Signal: n/a");
                std::string s = "Signal: " + (c.network_type.empty() ? std::string("?") : c.network_type);
                if (c.strength >= 0) { s += "  "; for (int i = 0; i < 5; ++i) s += (i < c.strength) ? "\xE2\x96\xB0" : "\xE2\x96\xB1"; }
                return s;
            };

            // ── Pair Device ────────────────────────────────────────────────────
            // Lists every reachable KDE Connect device (paired or not). Selecting
            // an unpaired one requests pairing (accept the prompt on the phone);
            // selecting a paired one unpairs it.
            std::vector<MenuItem> pair_menu;
            for (int i = 0; i < 8; ++i) {
                MenuItem d; d.type = MenuItemType::LEAF; d.label = "device";
                d.label_fn = [kdc_p, i]{
                    if (!kdc_p) return std::string();
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return std::string();
                    const auto& dev = v[i];
                    std::string suffix = dev.paired ? "  \xE2\x9C\x93 paired"
                                       : dev.reachable ? "  \xE2\x80\x94 tap to pair"
                                                       : "  (offline)";
                    return dev.name + suffix;
                };
                d.visible_fn = [kdc_p, i]{ return kdc_p && i < (int)kdc_p->devices().size(); };
                d.action = [kdc_p, i, toast]{
                    if (!kdc_p) return;
                    auto v = kdc_p->devices();
                    if (i >= (int)v.size()) return;
                    const auto& dev = v[i];
                    if (dev.paired) {
                        if (kdc_p->unpair(dev.id)) toast("Unpaired", dev.name);
                    } else if (kdc_p->request_pairing(dev.id)) {
                        toast("Pairing requested", "Accept the prompt on " + dev.name);
                    }
                };
                pair_menu.push_back(std::move(d));
            }

            // ── Export to Phone ─────────────────────────────────────────────────
            // Bundle a whole category (faces / gifs / qr codes) into one .tar.gz
            // and share it to the phone via KDE Connect. Source dirs are derived
            // from the active face path (faces + its sibling gifs) and state.qr_dir,
            // so no extra params have to be threaded through build_menu.
            auto export_to_phone = [kdc_p, teensy, &state, toast](const char* kind) {
                namespace fsx = std::filesystem;
                std::string src;
                if (std::string(kind) == "qr") {
                    std::lock_guard<std::mutex> lk(state.mtx); src = state.qr_dir;
                } else {
                    const std::string fp = teensy ? teensy->face_image_path("neutral")
                                                  : std::string();
                    if (!fp.empty()) {
                        fsx::path faces = fsx::path(fp).parent_path().parent_path();  // <assets>/faces
                        src = (std::string(kind) == "faces")
                                ? faces.string()
                                : (faces.parent_path() / "gifs").string();           // <assets>/gifs
                    }
                }
                const std::string k = kind;
                // Archive + share off-thread so a big folder doesn't stall the menu.
                std::thread([kdc_p, toast, src, k]{
                    namespace fsx = std::filesystem;
                    if (src.empty() || !fsx::exists(src)) {
                        toast("Export failed", "No " + k + " folder found"); return;
                    }
                    const std::string out    = "/tmp/protohud_" + k + ".tar.gz";
                    const std::string parent = fsx::path(src).parent_path().string();
                    const std::string base   = fsx::path(src).filename().string();
                    const std::string cmd = "tar -czf '" + out + "' -C '" + parent +
                                            "' '" + base + "' 2>/dev/null";
                    if (std::system(cmd.c_str()) != 0 || !fsx::exists(out)) {
                        toast("Export failed", "Could not archive " + k); return;
                    }
                    if (kdc_p && kdc_p->share_file(out)) toast("Sent to phone", k + ".tar.gz");
                    else                                 toast("Export ready", "Saved " + out);
                }).detach();
            };
            std::vector<MenuItem> export_menu = {
                with_desc(leaf("Export Faces",    [export_to_phone]{ export_to_phone("faces"); }),
                    "Bundle the faces folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export GIFs",     [export_to_phone]{ export_to_phone("gifs"); }),
                    "Bundle the gifs folder into one .tar.gz and send it to the phone."),
                with_desc(leaf("Export QR Codes", [export_to_phone]{ export_to_phone("qr"); }),
                    "Bundle the captured QR-code folder into one .tar.gz and send it."),
                with_desc(leaf("Export All", [export_to_phone]{
                    export_to_phone("faces"); export_to_phone("gifs"); export_to_phone("qr"); }),
                    "Send all three bundles to the phone, one file each."),
            };

            std::vector<MenuItem> phone_menu;
            phone_menu.push_back(with_desc(submenu("Pair Device", std::move(pair_menu)),
                "Pair or unpair a KDE Connect device. Reachable devices are listed; "
                "select an unpaired one and accept the prompt on the phone."));
            phone_menu.push_back(with_desc(submenu("Export to Phone", std::move(export_menu)),
                "Send your faces, GIFs or captured QR codes to the phone \xE2\x80\x94 each "
                "category bundled into a single .tar.gz over KDE Connect."));
            phone_menu.push_back(with_desc(leaf("Ring My Phone", ring_toast),
                "Ring the paired phone (KDE Connect findmyphone) so it plays its "
                "ringtone \xE2\x80\x94 handy for locating it."));
            phone_menu.push_back(with_desc(submenu("Notifications", std::move(notif_menu)),
                "Recent phone notifications. Reply to messaging apps or dismiss them "
                "(clears on the phone too)."));
            phone_menu.push_back(with_desc(submenu("Media", std::move(media_menu)),
                "Control the phone's media playback (play/pause, skip, volume, player)."));
            phone_menu.push_back(with_desc(submenu("Run Command", std::move(cmd_menu)),
                "Trigger one of the phone's saved KDE Connect commands."));
            phone_menu.push_back(with_desc(leaf("Send SMS\xE2\x80\xA6", send_sms_flow),
                "Send a text message through the phone (enter number, then message)."));
            phone_menu.push_back(with_desc(leaf("Mute Ringer", [kdc_p, toast]{
                if (kdc_p && kdc_p->mute_ringer()) toast("Ringer muted", "Silenced incoming call"); }),
                "Silence an incoming call's ringer (best-effort, depends on the phone)."));
            phone_menu.push_back(with_desc(conn_item,
                "Cellular network type + signal strength reported by the phone."));
            phone_menu.push_back(with_desc(submenu("Ignore List", std::move(ignore_menu)),
                "Mute notifications by app or sender. Apps/senders the phone has sent "
                "are grouped here \xE2\x80\x94 select one to mute it (muted entries turn red). "
                "\"Add Word\" still takes free text."));
            phone_menu.push_back(with_desc(submenu("Message Apps",
                list_menu(kdc_msg_p, apply_msg, "Add App\xE2\x80\xA6", "App name (e.g. Discord)")),
                "Apps whose notifications get the larger chat toast (sender + wrapped "
                "message, held longer). Select an entry to remove it."));
            return with_desc(submenu("Phone (KDE Connect)", std::move(phone_menu)),
                "Ring/mute, reply to & dismiss notifications, control media, run "
                "commands, send SMS, and mute apps/senders.");
    }();
    MenuItem notiflog_item = [&]() -> MenuItem {
        // ── Notification Log: grouped by sender, filterable, full text in panel ──
        // Type filter (radio).
        auto type_opt = [&leaf_sel, &state](const char* lbl, int t){
            return leaf_sel(lbl, [&state, t]{ state.notif_type_filter = t; },
                                 [&state, t]{ return state.notif_type_filter == t; });
        };
        std::vector<MenuItem> typ_menu = {
            type_opt("All Types",    -1),
            type_opt("Alarms",       static_cast<int>(NotifType::Alarm)),
            type_opt("Timers",       static_cast<int>(NotifType::Timer)),
            type_opt("LoRa",         static_cast<int>(NotifType::LoRa)),
            type_opt("Phone / Apps", static_cast<int>(NotifType::App)),
        };
        MenuItem typ_item = submenu("Filter: Type", std::move(typ_menu));
        typ_item.label_fn = [&state]{
            const char* t = state.notif_type_filter < 0 ? "All"
                : state.notif_type_filter == (int)NotifType::Alarm ? "Alarms"
                : state.notif_type_filter == (int)NotifType::Timer ? "Timers"
                : state.notif_type_filter == (int)NotifType::LoRa  ? "LoRa" : "Phone/Apps";
            return std::string("Filter: Type  [") + t + "]";
        };

        // Distinct sender names currently in the log (sorted).
        auto distinct_senders = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            std::set<std::string> s;
            for (const auto& n : state.notifs.items) if (!n.title.empty()) s.insert(n.title);
            return std::vector<std::string>(s.begin(), s.end());
        };
        // Sender checklist (multi-select).
        std::vector<MenuItem> snd_menu;
        snd_menu.push_back(with_desc(leaf("Show All Senders", [&state]{
            std::lock_guard<std::mutex> lk(state.mtx); state.notif_sender_sel.clear();
        }), "Clear the selection so every sender shows."));
        for (int i = 0; i < 24; ++i) {
            MenuItem m; m.type = MenuItemType::TOGGLE; m.label = "sender";
            m.label_fn   = [distinct_senders, i]{
                auto d = distinct_senders(); return i < (int)d.size() ? d[i] : std::string(); };
            m.visible_fn = [distinct_senders, i]{ return i < (int)distinct_senders().size(); };
            m.get_toggle = [distinct_senders, i, &state]{
                auto d = distinct_senders(); if (i >= (int)d.size()) return false;
                std::lock_guard<std::mutex> lk(state.mtx); return state.notif_sender_sel.count(d[i]) > 0; };
            m.set_toggle = [distinct_senders, i, &state](bool v){
                auto d = distinct_senders(); if (i >= (int)d.size()) return;
                std::lock_guard<std::mutex> lk(state.mtx);
                if (v) state.notif_sender_sel.insert(d[i]); else state.notif_sender_sel.erase(d[i]); };
            snd_menu.push_back(std::move(m));
        }
        MenuItem snd_item = submenu("Filter: Senders", std::move(snd_menu));
        snd_item.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            return state.notif_sender_sel.empty()
                ? std::string("Filter: Senders  [all]")
                : std::string("Filter: Senders  [") + std::to_string(state.notif_sender_sel.size()) + "]";
        };

        // Display order: queue indices passing the filters, grouped by sender
        // (sender A-Z, newest first within each). Rebuilt only when something changes.
        auto order     = std::make_shared<std::vector<int>>();
        auto order_key = std::make_shared<size_t>(SIZE_MAX);
        auto ensure_order = [&state, order, order_key]{
            std::lock_guard<std::mutex> lk(state.mtx);
            size_t key = static_cast<size_t>(state.notifs.next_id) * 1315423911u
                       + static_cast<size_t>(state.notif_type_filter + 2) * 2654435761u
                       + state.notifs.items.size();
            for (const auto& s : state.notif_sender_sel)
                key ^= std::hash<std::string>{}(s) + 0x9e3779b9u + (key << 6) + (key >> 2);
            if (key == *order_key) return;
            *order_key = key;
            order->clear();
            for (int i = 0; i < (int)state.notifs.items.size(); ++i) {
                const auto& n = state.notifs.items[i];
                if (state.notif_type_filter >= 0 &&
                    static_cast<int>(n.type) != state.notif_type_filter) continue;
                if (!state.notif_sender_sel.empty() && !state.notif_sender_sel.count(n.title)) continue;
                order->push_back(i);
            }
            std::stable_sort(order->begin(), order->end(), [&state](int a, int b){
                const auto& na = state.notifs.items[a]; const auto& nb = state.notifs.items[b];
                if (na.title != nb.title) return na.title < nb.title;
                return a < b;   // newest-first within a sender (queue is newest-first)
            });
        };

        std::vector<MenuItem> nlog_menu;
        // Fixed items (must stay 4 — the context panel maps cursor → row via -4).
        nlog_menu.push_back(with_desc(toggle("Persist Log",
            [&state]{ return state.notif_persist; },
            [&state](bool v){ state.notif_persist = v; }),
            "Save the log to disk so a sudden reboot doesn't lose it."));
        // Bulk clear — a small confirmation layer with the safe option first.
        // "Clear Unsaved" keeps anything the user pinned; "Clear All" wipes
        // everything including saved messages (red).
        std::vector<MenuItem> clr_menu;
        clr_menu.push_back(with_desc(leaf("Cancel", [menu_sys_pp]{
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); }),
            "Back out without clearing anything."));
        clr_menu.push_back(with_desc(leaf("Clear Unsaved", [&state, menu_sys_pp]{
            { std::lock_guard<std::mutex> lk(state.mtx);
              state.notifs.clear_if([](const Notification&){ return true; }, /*include_saved=*/false); }
            if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
        }), "Remove everything except messages you've saved."));
        {
            MenuItem all = leaf("Clear All", [&state, menu_sys_pp]{
                { std::lock_guard<std::mutex> lk(state.mtx); state.notifs.items.clear(); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            all.warn_fn = []{ return true; };   // red — also drops saved messages
            clr_menu.push_back(with_desc(std::move(all),
                "Remove every notification, including saved ones."));
        }
        MenuItem clr_item = submenu("Clear Log\xE2\x80\xA6", std::move(clr_menu));
        clr_item.context_panel_title = "Clear";
        clr_item.context_panel_draw  = [&state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            (void)sz; ImFont* f = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            int total, saved; { std::lock_guard<std::mutex> lk(state.mtx);
                total = (int)state.notifs.items.size(); saved = 0;
                for (const auto& n : state.notifs.items) saved += n.saved ? 1 : 0; }
            char ln[64]; float y = o.y;
            snprintf(ln, sizeof(ln), "%d total", total);
            dl->AddText(f, fs, {o.x,y}, IM_COL32(235,240,245,255), ln); y += fs*1.4f;
            snprintf(ln, sizeof(ln), "%d saved \xC2\xB7 %d unsaved", saved, total - saved);
            dl->AddText(f, fs*0.85f, {o.x,y}, IM_COL32(190,196,204,230), ln);
        };
        nlog_menu.push_back(with_desc(std::move(clr_item),
            "Bulk-clear the log: keep saved messages or wipe everything."));
        nlog_menu.push_back(with_desc(std::move(typ_item), "Show only the chosen type."));
        nlog_menu.push_back(with_desc(std::move(snd_item),
            "Tick which senders to show. None ticked = all senders."));
        // Message rows (grouped by sender, scrollable). Full text shows in the panel.
        // Draw a single notification (full text) into a panel — used by both the
        // log preview and the per-row delete confirmation.
        auto draw_one = [&state, order, ensure_order](ImDrawList* dl, ImVec2 o, ImVec2 sz,
                                                      int slot, bool confirm){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            if (confirm) {
                dl->AddText(font, fs * 0.95f, {o.x, y}, IM_COL32(255,110,100,255),
                            "Delete this message?");
                y += fs * 1.4f;
            }
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);
        };
        // Selecting a row opens a per-message popup with the full text in the
        // panel and three actions: Save (pin), Confirm Delete (red), Cancel.
        // Helper: queue index of the message currently at display slot `i`, or -1.
        auto qindex = [&state, order, ensure_order](int i) -> int {
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i >= (int)order->size()) return -1;
            const int qi = (*order)[i];
            return qi < (int)state.notifs.items.size() ? qi : -1;
        };
        for (int i = 0; i < NotificationQueue::kMax; ++i) {
            MenuItem m; m.type = MenuItemType::SUBMENU; m.label = "msg";
            m.label_fn = [&state, qindex, i]{
                const int qi = qindex(i);
                std::lock_guard<std::mutex> lk(state.mtx);
                if (qi < 0) return std::string();
                const auto& n = state.notifs.items[qi];
                std::string s = n.saved ? std::string("\xE2\x98\x85 ") : std::string();  // ★ if saved
                s += n.title;
                if (!n.body.empty()) s += "  \xC2\xB7  " + n.body;
                if (s.size() > 60) s = s.substr(0, 59) + "\xE2\x80\xA6";
                return s;
            };
            m.visible_fn = [order, ensure_order, i]{ ensure_order(); return i < (int)order->size(); };
            // Save / pin toggle. qindex() locks internally, so resolve it first,
            // then re-validate the index after re-acquiring the lock.
            MenuItem save; save.type = MenuItemType::TOGGLE; save.label = "Save Message";
            save.get_toggle = [&state, qindex, i]{
                const int qi = qindex(i); std::lock_guard<std::mutex> lk(state.mtx);
                return qi >= 0 && qi < (int)state.notifs.items.size() && state.notifs.items[qi].saved; };
            save.set_toggle = [&state, qindex, i](bool v){
                const int qi = qindex(i);
                { std::lock_guard<std::mutex> lk(state.mtx);
                  if (qi >= 0 && qi < (int)state.notifs.items.size()) state.notifs.items[qi].saved = v; }
                state.notif_dirty.store(true);   // metadata-only edit → force a flush
            };
            MenuItem confirm = leaf("Confirm Delete",
                [&state, qindex, i, menu_sys_pp]{
                    const int qi = qindex(i);
                    { std::lock_guard<std::mutex> lk(state.mtx);
                      if (qi >= 0 && qi < (int)state.notifs.items.size())
                          state.notifs.items.erase(state.notifs.items.begin() + qi); }
                    if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();   // back to the log
                });
            confirm.warn_fn = []{ return true; };   // red — destructive
            MenuItem cancel = leaf("Cancel",
                [menu_sys_pp]{ if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back(); });
            m.children = { with_desc(std::move(save), "Pin this message so Clear and the rolling buffer keep it."),
                           std::move(confirm), std::move(cancel) };
            m.context_panel_title = "Message";
            m.context_panel_draw  = [draw_one, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_one(dl, o, sz, i, false); };
            nlog_menu.push_back(std::move(m));
        }
        // Context panel: full text of the focused row.
        auto panel = [&state, order, ensure_order, menu_sys_pp]
                     (ImDrawList* dl, ImVec2 o, ImVec2 sz) {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const int idx  = (menu_sys_pp && *menu_sys_pp) ? (*menu_sys_pp)->current_index() : -1;
            const int slot = idx - 4;   // 4 fixed items precede the rows
            ensure_order();
            std::lock_guard<std::mutex> lk(state.mtx);
            if (slot < 0 || slot >= (int)order->size() ||
                (*order)[slot] >= (int)state.notifs.items.size()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a message to read it here.");
                return;
            }
            const auto& n = state.notifs.items[(*order)[slot]];
            float y = o.y;
            dl->AddText(font, fs * 1.05f, {o.x, y}, IM_COL32(235,240,245,255), n.title.c_str());
            y += fs * 1.3f;
            if (n.timestamp > 0) {
                char ts[24]; time_t t = (time_t)n.timestamp;
                strftime(ts, sizeof(ts), "%a %H:%M", localtime(&t));
                dl->AddText(font, fs * 0.7f, {o.x, y}, IM_COL32(150,158,166,210), ts);
                y += fs * 1.2f;
            }
            if (!n.body.empty())
                dl->AddText(font, fs * 0.85f, {o.x, y}, IM_COL32(210,214,220,235),
                            n.body.c_str(), nullptr, sz.x - 6.f);   // wrapped
        };
        return with_panel(with_desc(submenu("Notification Log", std::move(nlog_menu)),
            "Browse the log grouped by sender; scroll to a message to read its full "
            "text on the right. Filter by type/sender, save messages to pin them, or "
            "bulk-clear the rest."),
            "Message", panel);
    }();
    std::vector<MenuItem> communications_menu;
    communications_menu.push_back(with_desc(submenu("LoRa", std::move(lora_menu)),
        "Long-range radio: team nodes, messages and status."));
    // KDE Connect now lives under System > Connectivity (alongside SSH/Bluetooth);
    // Communications keeps the notification log + QR codes.
    connectivity_menu.push_back(std::move(phone_item));
    communications_menu.push_back(std::move(notiflog_item));

    // ── Scanned QR Codes ──────────────────────────────────────────────────
    // Each captured code lives in its own folder (link + raw image). This
    // submenu browses the de-duplicated running list; the panel shows the
    // captured image + link, and each row can Open (URLs) or Delete.
    MenuItem qr_codes_item = [&]() -> MenuItem {
        struct QrThumb { GLuint tex = 0; std::string loaded; cv::Mat img; };
        auto thumb = std::make_shared<QrThumb>();
        auto focus = std::make_shared<int>(-1);

        auto cap_at = [&state](int i) -> QrCapture {
            std::lock_guard<std::mutex> lk(state.mtx);
            if (i < 0 || i >= static_cast<int>(state.qr_captures.items.size())) return {};
            return state.qr_captures.items[i];
        };
        auto is_url = [](const std::string& t){
            return t.size() > 7 && (t.compare(0, 7, "http://") == 0 ||
                                    t.compare(0, 8, "https://") == 0);
        };
        // Shared panel painter: thumbnail (top) + link (wrapped) + type/time.
        auto draw_cap = [thumb](ImDrawList* dl, ImVec2 o, ImVec2 sz, const QrCapture& c){
            ImFont* font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
            if (c.text.empty()) {
                dl->AddText(font, fs * 0.8f, {o.x, o.y}, IM_COL32(170,176,184,220),
                            "Scroll to a code.");
                return;
            }
            float y = o.y;
            // Prefer the colour camera frame; fall back to the grayscale decode.
            std::string ipath;
            const std::string& fn = !c.image.empty() ? c.image : c.decode;
            if (!c.folder.empty() && !fn.empty())
                ipath = (std::filesystem::path(c.folder) / fn).string();
            if (!ipath.empty()) {
                if (ipath != thumb->loaded) {
                    thumb->img = face::load_png_rgba(ipath, 240, 180);
                    thumb->loaded = ipath;
                }
                const float pw = std::min(sz.x * 0.85f, 200.f), ph = pw * 0.75f;
                const float px = o.x, py = y;
                dl->AddRectFilled({px, py}, {px + pw, py + ph}, IM_COL32(10,16,22,190));
                if (!thumb->img.empty() && thumb->img.isContinuous()) {
                    if (!thumb->tex) {
                        glGenTextures(1, &thumb->tex);
                        glBindTexture(GL_TEXTURE_2D, thumb->tex);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }
                    glBindTexture(GL_TEXTURE_2D, thumb->tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, thumb->img.cols, thumb->img.rows,
                                 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb->img.data);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    dl->AddImage(reinterpret_cast<ImTextureID>(
                        static_cast<uintptr_t>(thumb->tex)), {px, py}, {px + pw, py + ph});
                }
                y = py + ph + 8.f;
            }
            dl->AddText(font, fs * 0.9f, {o.x, y}, IM_COL32(235,240,245,255),
                        c.text.c_str(), nullptr, sz.x - 6.f);
            ImVec2 tsz = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, sz.x - 6.f, c.text.c_str());
            y += tsz.y + 6.f;
            char meta[80]; std::string when;
            if (c.timestamp > 0) {
                char ts[24]; time_t t = static_cast<time_t>(c.timestamp);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&t));
                when = ts;
            }
            std::snprintf(meta, sizeof(meta), "%s%s%s", c.type.c_str(),
                          when.empty() ? "" : "  \xC2\xB7  ", when.c_str());
            dl->AddText(font, fs * 0.72f, {o.x, y}, IM_COL32(150,158,166,210), meta);
        };

        // List removable mounts (USB sticks etc.) under the auto-mount roots as
        // {path, display}. Cached ~1 s so the menu's per-slot label/visible fns
        // don't readdir every frame. Cheap listing only (no write probe).
        auto usb_cache   = std::make_shared<std::vector<std::pair<std::string,std::string>>>();
        auto usb_cache_t = std::make_shared<double>(-100.0);
        auto list_usb_mounts =
            [usb_cache, usb_cache_t]() -> const std::vector<std::pair<std::string,std::string>>& {
            const double now = ImGui::GetTime();
            if (now - *usb_cache_t > 1.0) {
                *usb_cache_t = now;
                usb_cache->clear();
                namespace fs = std::filesystem;
                std::vector<std::string> roots;
                std::error_code ec;
                if (const char* u = std::getenv("USER")) {
                    const std::string mu = std::string("/media/") + u;
                    if (fs::is_directory(mu, ec)) roots.push_back(mu);
                }
                if (roots.empty()) roots.push_back("/media");
                roots.push_back("/mnt");
                for (const auto& root : roots) {
                    if (!fs::is_directory(root, ec)) continue;
                    for (const auto& e : fs::directory_iterator(root, ec))
                        if (e.is_directory(ec))
                            usb_cache->emplace_back(e.path().string(),
                                                    e.path().filename().string());
                }
            }
            return *usb_cache;
        };
        // Derive a filename-safe, human-ish name from a code's text so the image
        // matches its link in the manifest. Host/path for URLs, else first chars,
        // plus a short hash of the full text to keep it unique.
        auto qr_parsed_name = [](const std::string& text) -> std::string {
            std::string base = text;
            const auto scheme = base.find("://");
            if (scheme != std::string::npos) base = base.substr(scheme + 3);
            std::string out;
            for (char ch : base) {
                if (std::isalnum(static_cast<unsigned char>(ch))) out += ch;
                else if (ch == '.' || ch == '-' || ch == '_') out += ch;
                else out += '_';
                if (out.size() >= 40) break;
            }
            if (out.empty()) out = "qr";
            char hx[8];
            std::snprintf(hx, sizeof(hx), "%04x",
                          static_cast<unsigned>(std::hash<std::string>{}(text) & 0xffff));
            return out + "_" + hx;
        };
        // Stage a temp bundle: each code's image copied to <parsed_name>.png plus
        // a qr_links.txt manifest mapping names → links. Returns the file list.
        auto build_qr_bundle = [qr_parsed_name](const std::vector<QrCapture>& caps)
            -> std::vector<std::string> {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path dir = fs::temp_directory_path(ec) / "protohud_qr_share";
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            std::vector<std::string> files;
            std::ofstream man((dir / "qr_links.txt").string());
            if (man) man << "ProtoHUD scanned codes\n======================\n\n";
            for (const auto& c : caps) {
                const std::string nm = qr_parsed_name(c.text);
                std::string src;
                if (!c.folder.empty()) {
                    for (const char* pref : {"camera.png", "decode.png"}) {
                        const fs::path p = fs::path(c.folder) / pref;
                        if (fs::exists(p, ec)) { src = p.string(); break; }
                    }
                    if (src.empty())
                        for (const auto& e : fs::directory_iterator(c.folder, ec))
                            if (e.is_regular_file() && e.path().extension() == ".png") {
                                src = e.path().string(); break; }
                }
                if (!src.empty()) {
                    const fs::path dst = dir / (nm + ".png");
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (!ec) files.push_back(dst.string());
                }
                if (man) {
                    char when[24] = "";
                    if (c.timestamp > 0) { time_t t = static_cast<time_t>(c.timestamp);
                        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t)); }
                    man << nm << ".png\n    " << c.text << "\n    [" << c.type
                        << (when[0] ? std::string("  ") + when : std::string()) << "]\n\n";
                }
            }
            if (man) { man.close(); files.push_back((dir / "qr_links.txt").string()); }
            return files;
        };
        // Send a set of captures (images + manifest) to the paired phone.
        auto send_to_phone = [kdc_p, build_qr_bundle, &state](const std::vector<QrCapture>& caps) {
            const bool dev = kdc_p && kdc_p->device_ready();
            int n = 0;
            if (dev) for (const auto& f : build_qr_bundle(caps))
                if (kdc_p->share_file(f)) ++n;
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = dev ? "Sent to phone" : "No phone connected";
            nt.body  = dev ? (std::to_string(n) + " file(s) shared (images + qr_links.txt)")
                           : "Pair + connect a device in KDE Connect first.";
            nt.auto_dismiss_s = 5.f; state.notifs.push(std::move(nt));
        };
        // Copy the bundle (renamed images + manifest) to a chosen mount.
        auto export_to_usb = [build_qr_bundle, &state](const std::vector<QrCapture>& caps,
                                                       const std::string& dest) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path target = fs::path(dest) / "protohud_qr";
            fs::create_directories(target, ec);
            int n = 0;
            for (const auto& f : build_qr_bundle(caps)) {
                fs::copy_file(f, target / fs::path(f).filename(),
                              fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
            std::lock_guard<std::mutex> lk(state.mtx);
            Notification nt; nt.type = NotifType::App;
            nt.title = n > 0 ? "Exported to USB" : "Export failed";
            nt.body  = n > 0 ? ("Copied " + std::to_string(n) + " file(s) to " + target.string())
                             : ("Could not write to " + dest);
            nt.auto_dismiss_s = 6.f; state.notifs.push(std::move(nt));
        };
        // Build the "Export / Send" target list (phone + each connected drive).
        // get_caps() returns the captures to export (one row, or all).
        auto build_export_targets =
            [kdc_p, list_usb_mounts, send_to_phone, export_to_usb](
                std::function<std::vector<QrCapture>()> get_caps) {
            std::vector<MenuItem> t;
            MenuItem p; p.type = MenuItemType::LEAF; p.label = "phone";
            p.label_fn   = [kdc_p]{ return std::string("Phone: ") +
                                    (kdc_p ? kdc_p->active_device_name() : std::string()); };
            p.visible_fn = [kdc_p]{ return kdc_p && kdc_p->device_ready(); };
            p.action     = [send_to_phone, get_caps]{ send_to_phone(get_caps()); };
            t.push_back(std::move(p));
            for (int j = 0; j < 6; ++j) {
                MenuItem m; m.type = MenuItemType::LEAF; m.label = "drive";
                m.label_fn   = [list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    return j < (int)v.size() ? ("USB: " + v[j].second) : std::string(); };
                m.visible_fn = [list_usb_mounts, j]{ return j < (int)list_usb_mounts().size(); };
                m.action     = [export_to_usb, get_caps, list_usb_mounts, j]{
                    const auto& v = list_usb_mounts();
                    if (j < (int)v.size()) export_to_usb(get_caps(), v[j].first); };
                t.push_back(std::move(m));
            }
            MenuItem none; none.type = MenuItemType::LEAF; none.label = "(no devices connected)";
            none.visible_fn = [kdc_p, list_usb_mounts]{
                return !(kdc_p && kdc_p->device_ready()) && list_usb_mounts().empty(); };
            t.push_back(std::move(none));
            return t;
        };

        std::vector<MenuItem> qmenu;
        // Clear All (red) — drops every entry + its folder.
        {
            MenuItem clr = leaf("Clear All", [&state]{
                std::vector<std::string> folders;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    for (auto& c : state.qr_captures.items)
                        if (!c.folder.empty()) folders.push_back(c.folder);
                    state.qr_captures.items.clear();
                    state.qr_captures.seen.clear();
                    qr_write_index(state.qr_dir, state.qr_captures);
                }
                std::error_code ec;
                for (auto& f : folders) std::filesystem::remove_all(f, ec);
            });
            clr.warn_fn    = []{ return true; };
            clr.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(clr),
                "Delete every saved QR code and its folder."));
        }
        // Export / Send All — to a chosen connected device (phone or a drive).
        // One bundle with every code's image + a combined qr_links.txt manifest.
        {
            MenuItem all = submenu("Export / Send All", build_export_targets(
                [&state]() -> std::vector<QrCapture> {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    return std::vector<QrCapture>(state.qr_captures.items.begin(),
                                                  state.qr_captures.items.end());
                }));
            all.visible_fn = [&state]{ std::lock_guard<std::mutex> lk(state.mtx);
                                       return !state.qr_captures.items.empty(); };
            qmenu.push_back(with_desc(std::move(all),
                "Send all saved codes to a connected device as named images plus a "
                "single qr_links.txt listing every link."));
        }
        // Capture rows (newest first).
        for (int i = 0; i < 40; ++i) {
            MenuItem row; row.type = MenuItemType::SUBMENU; row.label = "qr";
            row.label_fn = [&state, i]{
                std::lock_guard<std::mutex> lk(state.mtx);
                if (i >= static_cast<int>(state.qr_captures.items.size())) return std::string();
                std::string s = state.qr_captures.items[i].text;
                if (s.size() > 48) s = s.substr(0, 47) + "\xE2\x80\xA6";
                return s;
            };
            row.visible_fn   = [&state, i]{ std::lock_guard<std::mutex> lk(state.mtx);
                                 return i < static_cast<int>(state.qr_captures.items.size()); };
            row.on_highlight = [focus, i]{ *focus = i; };
            MenuItem open = leaf("Open Link", [cap_at, is_url, i]{
                QrCapture c = cap_at(i);
                if (!is_url(c.text)) return;
                std::string safe = c.text; for (auto& ch : safe) if (ch == '\'') ch = ' ';
                std::string cmd = "xdg-open '" + safe + "' >/dev/null 2>&1 &";
                system(cmd.c_str());
            });
            open.visible_fn = [cap_at, is_url, i]{ return is_url(cap_at(i).text); };
            MenuItem del = leaf("Delete", [&state, i, menu_sys_pp]{
                std::string folder;
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (i < static_cast<int>(state.qr_captures.items.size())) {
                        folder = state.qr_captures.items[i].folder;
                        state.qr_captures.seen.erase(state.qr_captures.items[i].text);
                        state.qr_captures.items.erase(state.qr_captures.items.begin() + i);
                        qr_write_index(state.qr_dir, state.qr_captures);
                    }
                }
                if (!folder.empty()) { std::error_code ec;
                    std::filesystem::remove_all(folder, ec); }
                if (menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->back();
            });
            del.warn_fn = []{ return true; };
            MenuItem exp = submenu("Export / Send", build_export_targets(
                [cap_at, i]() -> std::vector<QrCapture> { return { cap_at(i) }; }));
            exp.visible_fn = [cap_at, i]{ return !cap_at(i).folder.empty(); };
            exp.description = "Send this code (image + a qr_links.txt) to a connected "
                              "device — the paired phone (KDE Connect) or a USB drive.";
            row.children = { std::move(open), std::move(exp), std::move(del) };
            row.context_panel_title = "QR Code";
            row.context_panel_draw  = [draw_cap, cap_at, i](ImDrawList* dl, ImVec2 o, ImVec2 sz){
                draw_cap(dl, o, sz, cap_at(i)); };
            qmenu.push_back(std::move(row));
        }
        MenuItem sub = submenu("QR Codes", std::move(qmenu));
        sub.label_fn = [&state]{
            std::lock_guard<std::mutex> lk(state.mtx);
            const size_t n = state.qr_captures.items.size();
            return n ? ("QR Codes  [" + std::to_string(n) + "]") : std::string("QR Codes");
        };
        sub.context_panel_title = "QR Code";
        sub.context_panel_draw  = [draw_cap, focus, &state](ImDrawList* dl, ImVec2 o, ImVec2 sz){
            QrCapture c;
            { std::lock_guard<std::mutex> lk(state.mtx);
              if (*focus >= 0 && *focus < static_cast<int>(state.qr_captures.items.size()))
                  c = state.qr_captures.items[*focus]; }
            draw_cap(dl, o, sz, c);
        };
        return with_desc(std::move(sub),
            "Scanned QR/barcodes — each saved in its own folder with the link "
            "and the captured image. Already-seen codes aren't captured twice.");
    }();
    communications_menu.push_back(std::move(qr_codes_item));

    // Audio submenu wrapped with its existing live-status context panel.
    MenuItem audio_item = with_panel(
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
            bool enabled;  int  out_idx;  float cpu;  int  xruns;
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
        });

    // ── System root: grouped, one screen tall ────────────────────────────────
    std::vector<MenuItem> system_menu = {
        with_desc(submenu("Display", std::move(display_menu)),
                  "Fullscreen / frameless window mode and windowed resolution."),
        with_desc(submenu("HUD & Menu", std::move(display_hud_menu)),
                  "Text scale, HUD/menu theme + presets, radial-menu options."),
        std::move(audio_item),
        with_desc(submenu("Connectivity",     std::move(connectivity_menu)),
                  "SSH, Bluetooth and other network/peripheral toggles."),
        with_desc(submenu("Pi Settings",      std::move(pi_settings_items)),
                  "Hostname, time, storage, GPIO visualizer/buttons and cooling fans."),
        with_desc(submenu("XR Headset (Viture Beast)", std::move(headset_menu)),
                  "Electrochromic transparency, HUD/backlight brightness, recenter, "
                  "gaze lock and 3D side-by-side \xe2\x80\x94 specific to the glasses."),
        with_desc(submenu("Timers and Alarm",   std::move(timers_alarm_menu)),
                  "Stopwatches, countdowns and one-shot alarms."),
        with_desc(submenu("Diagnostics",        std::move(diagnostics_group_menu)),
                  "Probes, overlays and per-tab tools."),
        with_desc(submenu("Power",              std::move(power_menu)),
                  "Restart ProtoHUD / ProtoFace, reboot or shut down the Pi."),
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

    // Assemble Software last, now that profiles_menu exists: the in-HUD Updater
    // first, then Demo Mode (appended earlier), then Profiles & Backup. Added to
    // the System root as a single "Software" tab.
    software_menu.insert(software_menu.begin(),
        with_desc(submenu("Updates", std::move(updates_menu)),
            "Check the repo for new commits, switch/update branches, and roll back "
            "\xe2\x80\x94 all user-initiated. ProtoHUD never auto-updates."));
    // Pi system (apt) update — confirm-gated, since it changes the OS. Runs in
    // the background and toasts on completion.
    software_menu.push_back(confirm_action("Pi System Update",
        "Runs apt-get update && apt-get -y upgrade in the background (30 s to "
        "several minutes). Don't power off while it runs.",
        [run_sh]{ run_sh("System Update",
            "sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get -y upgrade"); }));
    // Submit Issue — compose a short report; saved under state/issues/ and (when a
    // phone is connected) shared to it so it can be filed on GitHub. No network /
    // token is used on the Pi, so it can't post directly.
    software_menu.push_back(with_desc(leaf("Submit Issue\xE2\x80\xA6",
        [menu_sys_pp, &state, kdc_p]{
            if (!menu_sys_pp || !*menu_sys_pp) return;
            (*menu_sys_pp)->open_keyboard("Describe the issue", "",
                [&state, kdc_p](const std::string& text){
                    if (text.empty()) return;
                    char exe[4096]; ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                    std::string root;
                    if (n > 0) { exe[n] = '\0'; std::string p(exe);
                        auto s = p.find_last_of('/'); if (s != std::string::npos) p.resize(s);
                        s = p.find_last_of('/');      if (s != std::string::npos) p.resize(s);
                        root = p; }
                    const std::string dir = (root.empty() ? std::string("/tmp")
                                                          : root + "/state/issues");
                    std::error_code ec; std::filesystem::create_directories(dir, ec);
                    std::time_t t = std::time(nullptr); char ts[32];
                    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
                    const std::string path = dir + "/issue_" + ts + ".txt";
                    { std::ofstream f(path);
                      if (f) f << "ProtoHUD issue (" << ts << ")\n\n" << text << "\n\n"
                               << "File at: https://github.com/brooksthemaker/protohud/issues/new\n"; }
                    if (kdc_p) kdc_p->share_file(path);   // hand off to the phone if paired
                    Notification nt; nt.type = NotifType::App; nt.icon = "message";
                    nt.title = "Issue saved"; nt.body = path; nt.auto_dismiss_s = 6.f;
                    std::lock_guard<std::mutex> lk(state.mtx); state.notifs.push(std::move(nt));
                });
        }),
        "Write a short bug report. Saved under state/issues/ and shared to the "
        "phone (if connected) so you can file it on GitHub."));
    software_menu.push_back(with_desc(submenu("Profiles & Backup", std::move(profiles_menu)),
        "Save and load full-setup snapshots. Loading a profile restarts ProtoHUD "
        "with that config."));
    system_menu.push_back(with_desc(submenu("Software", std::move(software_menu)),
        "In-HUD updater, Pi system update, demo scenes, issue reports, and "
        "full-setup profiles & backups."));

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
        with_desc(submenu("Communications", std::move(communications_menu)),
                  "Radio + phone: LoRa team mesh, KDE Connect phone, and the "
                  "notification log."),
        with_desc(submenu("Games",        [&]{
                  std::vector<MenuItem> games;
                  games.push_back(with_desc(toggle("Play",
                      [&state]{ return state.game_active.load(); },
                      [&state, menu_sys_pp](bool v){
                          state.game_active.store(v);
                          if (v && menu_sys_pp && *menu_sys_pp) (*menu_sys_pp)->close();
                      }),
                      "Start the selected game. It plays on the glasses and mirrors "
                      "to the LED face. Re-open the menu (or a GPIO Game-Toggle "
                      "button) to stop."));
                  // Source picker — Snake always; Doom only in the Doom-enabled build.
                  std::vector<MenuItem> src;
                  src.push_back(leaf_sel("Snake",
                      [&state]{ state.game_source_sel.store(0); },
                      [&state]{ return state.game_source_sel.load() == 0; }));
#ifdef PROTOHUD_HAVE_DOOM
                  src.push_back(leaf_sel("Doom",
                      [&state]{ state.game_source_sel.store(1); },
                      [&state]{ return state.game_source_sel.load() == 1; }));
#endif
#ifdef PROTOHUD_HAVE_LIBRETRO
                  src.push_back(leaf_sel("Emulator (libretro)",
                      [&state]{ state.game_source_sel.store(2); },
                      [&state]{ return state.game_source_sel.load() == 2; }));
#endif
                  games.push_back(with_desc(submenu("Source", std::move(src)),
                      "Pick the game. Snake is the built-in demo (D-pad / arrows "
                      "to steer, A / Z or Start / Enter to restart). Doom needs a "
                      "DOOM/Freedoom WAD at game.doom_wad. Emulator runs a libretro "
                      "core (game.libretro_core) + ROM (game.libretro_rom) \xe2\x80\x94 "
                      "software cores (NES/SNES/Genesis/GBA/PS1) for now."));
                  games.push_back(with_desc(toggle("Windowed",
                      [&state]{ return state.game_windowed.load(); },
                      [&state](bool v){ state.game_windowed.store(v); }),
                      "ON: show the game in a centred window so the HUD stays "
                      "visible around it. OFF: fullscreen (HUD hidden)."));
                  return games;
              }()),
                  "Play games with controller input on the glasses + LED face. "
                  "Ships with a Snake demo; Doom (doomgeneric) is available when "
                  "built with ENABLE_DOOM and a WAD is provided."),
        with_desc(submenu("System",       std::move(system_menu)),
                  "Display, audio, connectivity, Pi settings, timers, "
                  "diagnostics, profiles & power."),
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

    // ── Notification log persistence ──────────────────────────────────────────
    // Save the notification queue next to config.json so a sudden reboot doesn't
    // lose it. Loaded once here; saved (debounced) from the render loop + on exit.
    const std::string notif_path =
        (fs::path(cfg_path).parent_path() / "protohud_notifications.json").string();
    if (cfg.contains("notifications") && cfg["notifications"].is_object())
        state.notif_persist = cfg["notifications"].value("persist", true);
    auto save_notifs = [&state, notif_path]{
        json arr = json::array();
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            for (const auto& n : state.notifs.items)
                arr.push_back({{"type", static_cast<int>(n.type)}, {"title", n.title},
                               {"body", n.body}, {"ts", n.timestamp},
                               {"read", n.read}, {"dismissed", n.dismissed},
                               {"saved", n.saved}});
        }
        std::ofstream f(notif_path);
        if (f) f << arr.dump();
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
        owl_left.rotation_deg = jl.value("rotation_deg", 0);
    }
    if (jcam.contains("owlsight_right")) {
        auto& jr               = jcam["owlsight_right"];
        owl_right.libcamera_id = jr.value("libcamera_id", 1);
        owl_right.model_name   = jr.value("model_name",   std::string(""));
        owl_right.width        = jr.value("width",  1280);
        owl_right.height       = jr.value("height",  800);
        owl_right.fps          = jr.value("fps",      60);
        owl_right.rotation_deg = jr.value("rotation_deg", 0);
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
        if (jpf.contains("gradient") && jpf["gradient"].is_object()) {
            auto& jg = jpf["gradient"];
            pf_gradient.count     = std::clamp(jval(jg, "count", pf_gradient.count), 2, 6);
            pf_gradient.smooth    = jval(jg, "smooth", pf_gradient.smooth);
            pf_gradient.direction = jg.value("direction", pf_gradient.direction);
            pf_gradient.speed     = jval(jg, "speed", pf_gradient.speed);
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
        native_ctrl->start();
        // Push the user's saved animation tunables into every panel's
        // FaceState. The defaults in FaceState/FaceCfg apply otherwise.
        native_ctrl->set_blink_enabled(pf_blink_enabled);
        native_ctrl->set_blink_timing(pf_blink_min, pf_blink_max, pf_blink_duration);
        native_ctrl->set_expression_fade(pf_expr_fade);
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
                                   gpw, gph, gchain, gpar);
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
        native_ctrl->start();
        native_ctrl->set_blink_enabled(pf_blink_enabled);
        native_ctrl->set_blink_timing(pf_blink_min, pf_blink_max, pf_blink_duration);
        native_ctrl->set_expression_fade(pf_expr_fade);
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
                                   gpw, gph, gchain, gpar);
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

    MenuSystem menu(build_menu(&face_proxy, &xr, &cameras, &lora, &knob, &audio, state,
                               &android_mirror, &android_overlay_active,
                               &pip_overlay_cfg1, &pip_overlay_cfg2, &pip_overlay_cfg3,
                               &pip_cam1_overlay_active, &pip_cam2_overlay_active, &pip_cam3_overlay_active,
                               &android_overlay_cfg,
                               &hud.colors(), &hud.config(), &menu_ptr,
                               &mpu9250, &bno055, gif_names,
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
                               &multicam_layout, &multicam_usb_a, &multicam_usb_b,
                               &multicam_top_a, &multicam_top_b,
                               &profiles, &hud_presets, &quick_items,
                               cfg_gifs_dir,
                               &bg_lib_ptr, cfg_bg_user_dir,
                               &boop_sensor_ptr,
                               audio.voice(),
                               &accessory_leds,
                               &cooling_fans,
                               swap_backend, &pf_backend,
                               edit_face,
                               &pf_eye_layout, &pf_mouth_layout, &pf_nose_layout,
                               &pf_hub75,
                               &pf_hub75_layouts, &pf_hub75_active,
                               /* pf_layout_changed */ [&]{
                                   if (!native_ctrl) return;
                                   native_ctrl->set_active_layout_name(pf_hub75_active);
                                   // Push per-panel flips live so the orientation
                                   // toggles take effect on the panels immediately.
                                   std::vector<std::array<bool, 2>> flips;
                                   for (int i = 0; i < pf_hub75.panel_count && i < 4; ++i)
                                       flips.push_back({pf_hub75.flip_x[i], pf_hub75.flip_y[i]});
                                   native_ctrl->set_panel_flips(flips);
                               },
                               &pf_blink_enabled, &pf_blink_min, &pf_blink_max,
                               &pf_blink_duration, &pf_expr_fade,
                               &pf_preview_duration_s,
                               /* pf_anim_push */ [&]{
                                   if (!native_ctrl) return;
                                   native_ctrl->set_blink_enabled(pf_blink_enabled);
                                   native_ctrl->set_blink_timing(pf_blink_min,
                                                                 pf_blink_max,
                                                                 pf_blink_duration);
                                   native_ctrl->set_expression_fade(pf_expr_fade);
                               },
                               /* pf_set_effect_json */ [&](const nlohmann::json& spec){
                                   if (native_ctrl) native_ctrl->set_effect_json(spec);
                               },
                               /* pf_set_expr_effects */ [&](bool v){
                                   if (native_ctrl) native_ctrl->set_expression_effects(v);
                               },
                               /* pf_expr_effects_p */ &pf_expr_effects,
                               /* pf_live_tick */ pf_live_tick,
                               /* cfg_root */ &cfg,
                               /* USB camera preview wiring */
                               &tex_usb1, &tex_usb2, &tex_usb3, &usb_preview_req,
                               /* pf_gradient */ &pf_gradient,
                               /* pf_set_material */ [&](const std::string& spec){
                                   if (native_ctrl) native_ctrl->set_material_spec(spec);
                               },
                               /* pf_restart_renderer */ [&]{
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
                                       native_ctrl->canvas_height(), gpw, gph, gchain, gpar);
                                   Notification n; n.type = NotifType::App;
                                   n.title = "Panel driver restarted";
                                   n.body  = "If panels stay dark, check /tmp/panel_driver.log";
                                   n.auto_dismiss_s = 6.f;
                                   std::lock_guard<std::mutex> lk(state.mtx);
                                   state.notifs.push(std::move(n));
                               },
                               /* gpio_pins */ gpio_pins.data(), kGpioSlots,
                               &gpio_inputs_enabled, gpio_reload, kdc_menu_ptr,
                               &kdc_ignore, &kdc_msgapps,
                               &coproc_cfg.enabled, coproc_reload, coproc_status));
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
        // Editor lockdown — when the face editor is the active overlay the
        // wireless cursor must not page through the menu underneath. Up/Down
        // still route through menu.navigate (which forwards to face_editor_'s
        // vertical cursor step); Left/Right route through menu.back/select,
        // which would cancel/paint inside the editor — drop those.
        wireless.on_nav_up   ([&menu]{ if (menu.is_open()) menu.navigate(-1); });
        wireless.on_nav_down ([&menu]{ if (menu.is_open()) menu.navigate(+1); });
        wireless.on_nav_left ([&menu, &hud]{
            if (menu.is_face_editor_open())   return;
            if      (hud.toast_has_focused()) hud.toast_navigate(-1);
            else if (menu.is_open())          menu.back();
        });
        wireless.on_nav_right([&menu, &hud, &state]{
            if (menu.is_face_editor_open())   return;
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

    // Map an assigned function to its action. Runs on the GPIO poll thread;
    // each action hops onto an already-thread-safe path (fire_boop, the menu,
    // state.mtx) or flips a plain bool the render loop reads.
    auto gpio_dispatch = [&, restart_script](input::GpioFunc f) {
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
        case F::GameToggle: {
            bool on = !state.game_active.load();
            state.game_active.store(on);
            if (on && menu.is_open()) menu.close();   // hand the controller to the game
        } break;
        case F::None: default: break;
        }
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
        else if (landing.active)          bg_lib.prev();
        else if (hud.toast_has_focused()) hud.toast_navigate(-1);
        else if (menu.is_open())          menu.back();
    });
    gamepad.on_nav_right([&menu, &hud, &state, &landing, &bg_lib, &map_pan]{
        if (menu.is_face_editor_open())   return;
        if      (state.map_overlay.expanded) map_pan(-0.06f, 0);
        else if (menu.is_keyboard_open()) menu.osk_move(+1, 0);
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
        {
            auto& jg = cfg["protoface"]["gradient"];
            jg["count"]     = std::clamp(pf_gradient.count, 2, 6);
            jg["smooth"]    = pf_gradient.smooth;
            jg["direction"] = pf_gradient.direction;
            jg["speed"]     = pf_gradient.speed;
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

        cfg["resolution"]["width"]  = state.camera_resolution.width;
        cfg["resolution"]["height"] = state.camera_resolution.height;
        cfg["resolution"]["fps"]    = state.camera_resolution.fps;

        // Persist CSI display rotation so the in-app setting survives a
        // restart (rotation_deg lives next to the camera-id/width/height/fps
        // block where the user already finds the rest of the per-eye config).
        cfg["cameras"]["owlsight_left"]["rotation_deg"]  = cameras.owl_left_rotation();
        cfg["cameras"]["owlsight_right"]["rotation_deg"] = cameras.owl_right_rotation();

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

    // ── Game mode ─────────────────────────────────────────────────────────────
    // The active GameSource renders an RGBA framebuffer; in the render loop we
    // tick it from the controller/keyboard, blit it to both eyes (fullscreen or
    // windowed) and mirror it onto the HUB75 panels via the panel override. The
    // source is hot-swappable via state.game_source_sel (0 = Snake, 1 = Doom).
    json   jgame   = cfg.value("game", json::object());
    std::string doom_wad = jgame.value("doom_wad",
                                       std::string("/home/user/.local/share/protohud/doom.wad"));
    std::string lr_core    = jgame.value("libretro_core",   std::string());
    std::string lr_rom     = jgame.value("libretro_rom",    std::string());
    std::string lr_sysdir  = jgame.value("libretro_system_dir",
                                         std::string("/home/user/.local/share/protohud/system"));
    int    game_which = 0;               // currently-built source (mirrors game_source_sel)
    std::unique_ptr<game::GameSource> game_src = game::make_snake();
    GLuint game_tex = 0;                 // lazily created on first frame upload
    bool   game_was_active = false;      // edge-detect to (re)start / clear override

    double prev_time = glfwGetTime();
    uint32_t notif_last_saved = state.notifs.next_id;   // debounced log persistence
    size_t   notif_last_size  = state.notifs.items.size();
    double   notif_next_save  = 0.0;

    while (!glfwWindowShouldClose(xr.glfw_window()) && !state.quit) {
        wd_heartbeat.fetch_add(1, std::memory_order_relaxed);

        // Apply a pending window-mode change (Settings > Fullscreen / Frameless).
        if (state.win_mode_dirty.exchange(false))
            xr.apply_window_mode(state.win_fullscreen.load(), state.win_frameless.load());
        if (state.win_resize_dirty.exchange(false)) {
            const int rw = state.win_resize_w.load(), rh = state.win_resize_h.load();
            if (rw > 0 && rh > 0 && xr.glfw_window())
                glfwSetWindowSize(xr.glfw_window(), rw, rh);
        }

        // ── Delta time ────────────────────────────────────────────────────────
        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - prev_time);
        prev_time  = now;

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
            const size_t sz = [&]{ std::lock_guard<std::mutex> lk(state.mtx);
                                   return state.notifs.items.size(); }();
            bool dirty = state.notif_dirty.exchange(false);
            if (state.notifs.next_id != notif_last_saved || sz != notif_last_size || dirty) {
                save_notifs();
                notif_last_saved = state.notifs.next_id;
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
            if (key_pressed(ImGuiKey_Enter)) {
                if (ImGui::GetIO().KeyCtrl) menu.secondary();   // Ctrl+Select → settings
                else                        menu.select();
            } else if (!editor_up && key_pressed(ImGuiKey_RightArrow)) {
                menu.select();
            }
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

        // Multi-cam quad composite. Renders the same content into both eyes
        // (CSI L/R on the top half, two selected USB cams on the bottom),
        // using sub-viewports inside the bound eye FBO so each feed's
        // existing draw call fills only its quadrant. The CSI rotation
        // configured under Cameras > Left/Right Camera > Rotation is
        // applied by the NV12 shader as usual — so a 90° rotation on the
        // CSI cameras turns the top half into two upright portrait feeds.
        auto draw_multicam_into_current_fbo = [&](int fw, int fh) {
            const int hw  = fw / 2;             // left half width
            const int rw  = fw - hw;            // right half width (absorbs odd column)
            const int hh  = fh / 2;             // top half height
            const int top = fh - hh;            // bottom half height; top starts at y=top
            // Bottom halves use height `top` and right halves width `rw` so an
            // odd fw/fh leaves no 1px seam between the quadrants.

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

            glViewport(0,  top, hw, hh); draw_src(multicam_top_a);   // top-left
            glViewport(hw, top, rw, hh); draw_src(multicam_top_b);   // top-right
            glViewport(0,  0,   hw, top); draw_src(multicam_usb_a);  // bottom-left
            glViewport(hw, 0,   rw, top); draw_src(multicam_usb_b);  // bottom-right

            // Restore full viewport so any later passes (post-process /
            // HUD overlays into this FBO) see the whole eye region.
            glViewport(0, 0, fw, fh);
        };

        // ── Game mode: tick + upload frame ────────────────────────────────────
        // When active, build the held-button mask from the gamepad and keyboard,
        // advance the GameSource, upload its RGBA frame to game_tex, and mirror
        // it onto the HUB75 panels. The per-eye blocks below blit game_tex
        // (fullscreen or windowed) instead of the cameras.
        const bool game_active = state.game_active.load();
        if (game_active) {
            // Hot-swap the source if the menu changed the selection.
            int want = state.game_source_sel.load();
            if (want != game_which) {
                game_which = want;
#ifdef PROTOHUD_HAVE_DOOM
                if (want == 1) game_src = game::make_doom(doom_wad);
                else
#endif
#ifdef PROTOHUD_HAVE_LIBRETRO
                if (want == 2) game_src = game::make_libretro(lr_core, lr_rom, lr_sysdir);
                else
#endif
                    game_src = game::make_snake();
                game_was_active = false;   // run the (re)start path below
            }

            uint32_t gb = 0;
            auto gpb = gamepad.buttons();
            if (gpb.dup)    gb |= game::BtnUp;
            if (gpb.ddown)  gb |= game::BtnDown;
            if (gpb.dleft)  gb |= game::BtnLeft;
            if (gpb.dright) gb |= game::BtnRight;
            if (gpb.a)      gb |= game::BtnA;
            if (gpb.b)      gb |= game::BtnB;
            if (gpb.x)      gb |= game::BtnX;
            if (gpb.y)      gb |= game::BtnY;
            if (gpb.lb)     gb |= game::BtnL;
            if (gpb.rb)     gb |= game::BtnR;
            if (gpb.start)  gb |= game::BtnStart;
            // Keyboard fallback (arrows + Z/X/Enter), so the demo is playable
            // without a controller on the desktop build.
            if (GLFWwindow* w = static_cast<GLFWwindow*>(xr.glfw_window())) {
                if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) gb |= game::BtnUp;
                if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) gb |= game::BtnDown;
                if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) gb |= game::BtnLeft;
                if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) gb |= game::BtnRight;
                if (glfwGetKey(w, GLFW_KEY_Z)     == GLFW_PRESS) gb |= game::BtnA;
                if (glfwGetKey(w, GLFW_KEY_X)     == GLFW_PRESS) gb |= game::BtnB;
                if (glfwGetKey(w, GLFW_KEY_ENTER) == GLFW_PRESS) gb |= game::BtnStart;
            }

            if (!game_was_active) game_src->reset();
            game_src->tick(dt, gb);

            const cv::Mat& gf = game_src->frame();
            if (!gf.empty() && gf.isContinuous() && gf.type() == CV_8UC4) {
                if (game_tex == 0) {
                    glGenTextures(1, &game_tex);
                    glBindTexture(GL_TEXTURE_2D, game_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, game_tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gf.cols, gf.rows, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, gf.data);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            if (native_ctrl) native_ctrl->set_panel_override(gf);   // mirror to HUB75
        } else if (game_was_active) {
            if (native_ctrl) native_ctrl->clear_panel_override();   // hand panels back to the face
        }
        game_was_active = game_active;

        // Blit game_tex into the current eye FBO: fullscreen, or centred at a
        // 1:1-ish "window" rect when game_windowed. Returns true so the eye
        // blocks below can skip the camera path.
        auto draw_game_into_current_fbo = [&](int fw, int fh) {
            if (state.game_windowed.load()) {
                const cv::Mat& gf = game_src->frame();
                const float ar = (gf.cols > 0 && gf.rows > 0)
                               ? float(gf.cols) / float(gf.rows) : 16.f / 9.f;
                int vw = fw / 2, vh = static_cast<int>(vw / ar);
                if (vh > fh) { vh = fh / 2; vw = static_cast<int>(vh * ar); }
                glViewport((fw - vw) / 2, (fh - vh) / 2, vw, vh);
                cameras.draw_tex_fullscreen(game_tex);
                glViewport(0, 0, fw, fh);
            } else {
                cameras.draw_tex_fullscreen(game_tex);
            }
        };

        // Left eye
        {
            xr.eye_left().bind();
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (game_active && game_tex != 0) {
                // Game mode owns the whole eye FBO (over cameras / multicam).
                draw_game_into_current_fbo(xr.eye_left().w, xr.eye_left().h);
            } else if (multicam_layout) {
                // Quad layout overrides the per-eye source pickers and
                // theater mode — it owns the whole eye FBO.
                draw_multicam_into_current_fbo(xr.eye_left().w, xr.eye_left().h);
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

            if (game_active && game_tex != 0) {
                draw_game_into_current_fbo(xr.eye_right().w, xr.eye_right().h);
            } else if (multicam_layout) {
                draw_multicam_into_current_fbo(xr.eye_right().w, xr.eye_right().h);
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
        // Editor full-screen mode: skip every HUD chrome draw (PiP underlays,
        // info panel + compass + minimap + clock chrome, toast banners) so
        // the editor sits cleanly on top of the eye texture with no UI
        // clutter. Toasts re-appear on close because they're a queue, not a
        // timed overlay.
        // Fullscreen game mode also hides the HUD chrome (the game owns the
        // view); windowed game mode keeps it so the player still sees the HUD.
        const bool game_fullscreen = game_active && !state.game_windowed.load();
        if (!editor_open && !game_fullscreen) {
            // Hide the on-screen PiP for a slot whose live-preview panel is up — its
            // feed is shown in the context pane instead (preview set above this frame).
            hud.draw_pip_underlays(tex_usb1, p1 && preview != 1, pip_overlay_cfg1,
                                   tex_usb2, p2 && preview != 2, pip_overlay_cfg2,
                                   tex_usb3, p3 && preview != 3, pip_overlay_cfg3,
                                   xr.display_width(), xr.display_height());
            hud.draw_hud_frame(snap, xr.display_width(), xr.display_height(), fps_overlay_active);
            hud.draw_toasts(state.notifs, xr.display_width(), xr.display_height());
        }
        hud.end_nvg_overlay();

        // ── Phase 2: ImGui overlays (menu, popups) ────────────────────────
        menu.set_glow_enabled(hud.config().glow_enabled);
        if (menu.is_deep_open() || menu.is_keyboard_open()) {
            // Keyboard takes over full-screen (draws only the OSK), so this also
            // covers text entry opened from the corner / radial quick menu.
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
            // Top-docked (minimap in the upper screen half) → flip wedge labels
            // as one run so they read right-way-up; matches the info module.
            const bool radial_top = mcy < dispH * 0.5f;
            menu.draw_radial(mcx, mcy, half, focus, rotate, radial_top);
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

        if (panel_preview_enabled && !editor_open) {
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

        hud.render_menu_overlay();

        // ── Swap ──────────────────────────────────────────────────────────────
        xr.present();
    }

    // Final notification-log flush so the last messages survive a clean exit.
    if (state.notif_persist) save_notifs();

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
