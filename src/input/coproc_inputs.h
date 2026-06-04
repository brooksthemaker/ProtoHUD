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
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "gpio_function.h"

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
};

// Mirrors GpioInputs' lifecycle so main.cpp treats them the same way.
class CoprocInputs {
public:
    CoprocInputs(CoprocConfig cfg, std::function<void(GpioFunc)> dispatch);
    ~CoprocInputs();

    bool init();          // open transport + start reader thread; false on failure
    void shutdown();
    bool connected() const { return connected_.load(); }   // surfaced to HUD status

private:
    void reader_loop();                       // transport read + reconnect loop
    void on_line(const std::string& line);    // parse one framed message → dispatch
    void handle_button(int id, bool is_long); // map id→GpioFunc, call dispatch_

    CoprocConfig                  cfg_;
    std::function<void(GpioFunc)> dispatch_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             connected_{false};
    std::thread                   thread_;
    int                           fd_ = -1;   // serial or i2c fd
};

} // namespace input
