#pragma once
// ── coproc_inputs.h ─────────────────────────────────────────────────────────────
// OPTIONAL button/switch coprocessor source. A small MCU (RP2350, or an earlier
// RP2040/Pico) debounces the physical switches and streams button events to the
// Pi, which dispatches them through the SAME input::GpioFunc path as the local
// GPIO poller (input::GpioInputs). This offloads debounce timing and frees the
// scarce free GPIO that HUB75 leaves behind — see
// hardware/carrier-board/MULTI-BACKEND.md and docs/coprocessor-input.md.
//
// This is a FRAMEWORK SKELETON: the interface mirrors GpioInputs so it can be a
// drop-in additional (or replacement) source. Method bodies are intentionally
// left for implementation in coproc_inputs.cpp (not yet added to CMake SOURCES,
// so adding this header alone does not affect the build).
//
// Integration seam (main.cpp ~line 11860/11897):
//   auto gpio_dispatch = [&](input::GpioFunc f){ ... };          // existing
//   if (coproc_cfg.enabled)
//       coproc = std::make_unique<input::CoprocInputs>(coproc_cfg, gpio_dispatch);
//   // local GpioInputs stays unless coproc_cfg.replace_local_gpio == true
//
// Both sources call the one dispatch — nothing downstream (menu, boop, camera
// actions) needs to know where the press came from.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gpio_function.h"
#include "../../firmware/proto_link/coproc_proto.h"   // shared wire contract (v2)

namespace input {

struct CoprocConfig {
    bool        enabled = false;            // master toggle — false = pure local GPIO

    // Transport. "usb_serial" (USB CDC/ACM — default, hot-pluggable) or "i2c"
    // (carrier-wired, Pi master + a data-ready IRQ line, frees a USB port).
    std::string transport = "usb_serial";

    // usb_serial: prefer a STABLE path so it doesn't race with the other ACM
    // devices (Teensy/SmartKnob/RAK4631). Use /dev/serial/by-id/... matched on
    // the coprocessor's USB serial string, not /dev/ttyACMn.
    std::string device    = "/dev/serial/by-id/usb-ProtoHUD_Buttons-if00";
    int         baud      = 115200;

    // i2c: bus + 7-bit address of the coprocessor, plus an interrupt GPIO the
    // coproc pulls to signal "events queued" (avoids polling the bus).
    std::string i2c_bus   = "/dev/i2c-1";
    int         i2c_addr  = 0x42;
    int         irq_gpio  = -1;             // BCM line; -1 = poll instead

    // When true, the coproc is the only button source (local GpioInputs is not
    // started). When false (default) the two are ADDITIVE — mix local + coproc.
    bool        replace_local_gpio = false;

    // Button-id → function map. The coprocessor reports a small integer button
    // id + a SHORT/LONG classification; the Pi resolves the id to a GpioFunc
    // here so REMAPPING stays in the HUD config (the coproc firmware is dumb
    // about meaning, smart about timing). Unmapped ids dispatch GpioFunc::None.
    std::map<int, GpioFunc> short_map;      // button id → short-press function
    std::map<int, GpioFunc> long_map;       // button id → long-press function

    // Heartbeat: if no PING/event seen within this window, mark offline (and,
    // if replace_local_gpio, optionally fall back — see docs).
    int         heartbeat_timeout_ms = 2000;

    // ── Sensor ownership (v2 aggregator) ────────────────────────────────────
    // When the coproc is the I²C master for a sensor, the CM5 must NOT also
    // start its own driver for that chip (two masters on one bus = contention).
    // These flags are config-declared (deterministic — no startup race waiting
    // for the HELLO caps) and main.cpp uses them to SKIP the local driver and
    // route the coproc's decoded stream into the same AppState sinks instead.
    // The runtime caps() (from HELLO) are advisory: a mismatch is logged.
    bool        provides_imu_bno = false;
    bool        provides_imu_mpu = false;
    bool        provides_boop    = false;
    bool        provides_light   = false;
    bool        provides_panels  = false;
};

namespace cp = coproc_proto;

// Mirrors GpioInputs' lifecycle so main.cpp treats them the same way.
class CoprocInputs {
public:
    CoprocInputs(CoprocConfig cfg, std::function<void(GpioFunc)> dispatch);
    ~CoprocInputs();

    bool init();          // open transport + start reader thread; false on failure
    void shutdown();
    bool connected() const { return connected_.load(); }   // surfaced to HUD status

    // Capability bitmask advertised in the coproc's HELLO line (coproc_proto::Caps).
    // 0 until the first HELLO. Advisory — gating uses the config provides_* flags.
    uint16_t caps() const { return caps_.load(); }

    // ── Telemetry sinks (v2) ────────────────────────────────────────────────
    // Set by main.cpp to route decoded sensor frames into the SAME AppState
    // updates the on-board drivers feed. Called on the reader thread — keep the
    // handlers cheap and lock AppState::mtx like the existing driver callbacks.
    void on_bno  (std::function<void(const cp::BnoPayload&)>   fn) { on_bno_   = std::move(fn); }
    void on_mpu  (std::function<void(const cp::MpuPayload&)>   fn) { on_mpu_   = std::move(fn); }
    void on_boop (std::function<void(const cp::BoopPayload&)>  fn) { on_boop_  = std::move(fn); }
    void on_light(std::function<void(const cp::LightPayload&)> fn) { on_light_ = std::move(fn); }

private:
    void reader_loop();                       // transport read + reconnect loop
    void process_byte(uint8_t c);             // dual-mode demux: 0xAA→frame, else ASCII
    void ascii_byte(uint8_t c);               // accumulate v1 ASCII line → on_line
    void on_line(const std::string& line);    // parse one ASCII line (HELLO/PING/BTN)
    void on_frame(uint8_t cmd, const uint8_t* payload, uint16_t len);  // v2 binary frame
    void handle_button(int id, bool is_long); // map id→GpioFunc, call dispatch_
    void parse_hello_caps(const std::string& line);   // pull caps= bitmask from HELLO

    CoprocConfig                  cfg_;
    std::function<void(GpioFunc)> dispatch_;
    std::function<void(const cp::BnoPayload&)>   on_bno_;
    std::function<void(const cp::MpuPayload&)>   on_mpu_;
    std::function<void(const cp::BoopPayload&)>  on_boop_;
    std::function<void(const cp::LightPayload&)> on_light_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             connected_{false};
    std::atomic<uint16_t>         caps_{0};
    std::thread                   thread_;
    int                           fd_ = -1;   // serial or i2c fd

    // Reader-thread parse state (single-threaded; no lock needed).
    std::string          line_buf_;   // ASCII accumulator (v1 lines)
    std::vector<uint8_t> frame_;       // binary frame in progress (starts at 0xAA)
};

} // namespace input
