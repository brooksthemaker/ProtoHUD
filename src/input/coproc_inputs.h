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
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpio_function.h"

namespace input {

// One physical switch on the coprocessor. The order in CoprocConfig::pins is the
// button id (index) the firmware reports, which short_map/long_map then resolve
// to a GpioFunc. Pushed to the firmware over "PINCFG …" on connect, so the pin
// ROLES are HUD config — no firmware reflash to move a switch or a backlight.
struct CoprocPin {
    int         gp         = -1;       // RP2350 GPIO number
    std::string pull       = "up";     // up | down | none
    bool        active_low = true;     // pressed pulls the pin LOW
    int         led_gp     = -1;       // optional backlight GPIO, -1 = none
};

struct CoprocConfig {
    bool        enabled = false;            // master toggle — false = pure local GPIO

    // Coprocessor board, for the pin visualizer/editor (GPIO > RP2350 GPIO
    // Expander): "rp2350a" (Pico 2), "pico_plus_2", "pico_lipo2_xl_w"
    // (RP2350B, GP0-47), or "raw" (board-agnostic GP grid). Pins-only — the
    // firmware doesn't care which board it's flashed to.
    std::string variant = "rp2350a";

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

    // TTP223 touch-pad map (firmware "BOOP <idx> <1|0>" from the pre-assigned
    // touch pins). Pads whose index matches a boop ZONE electrode fire that
    // zone (snout/cheek behavior); any pad mapped here ALSO fires this
    // GpioFunc on touch-down — so up to 6 pads can be boop zones, extra
    // buttons, or both. Unmapped, un-zoned pads do nothing.
    std::map<int, GpioFunc> touch_map;      // touch pad idx → function

    // Physical pin map pushed to the firmware on connect (order = button id).
    // Empty = leave the firmware on its compiled-in config.h defaults.
    std::vector<CoprocPin>  pins;

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

    // I²C bus test: ask the coprocessor to probe its I²C lines (default GP20/21,
    // or the given SDA/SCL) and report which addresses ACK. The reply is captured
    // asynchronously; poll i2c_scan_result() for the last result ("scanning…",
    // "none", or a hex address list). A quick connectivity check for the DAC /
    // any I²C device wired to the coprocessor.
    void request_i2c_scan(int sda = -1, int scl = -1);
    std::string i2c_scan_result() const;

    // ── Peripheral hub (firmware built with -DPERIPHERAL_HUB) ────────────────
    // Boop pads on the coprocessor: "BOOP <electrode> <1|0>" edges call this
    // handler from the reader thread — post to the input queue inside it.
    // Set BEFORE init() (or re-set right after a reload's init()).
    void set_boop_handler(std::function<void(int electrode, bool touched)> fn) {
        boop_fn_ = std::move(fn);
    }
    // Latest DS18B20 reading for a ROM id (16 hex chars, as the firmware
    // reports it). False when the probe hasn't reported within ~3 periods.
    bool coproc_temp(const std::string& rom_id, double& c_out) const;
    // Drive a coprocessor fan zone ("FAN <zone> <duty%>"). Thread-safe: one
    // whole line per write() call, like every other sender on this link.
    void send_fan_duty(int zone, int duty_pct);

    // ── Peripheral TEST verbs (pre-assigned pins; see firmware config.h) ─────
    // Servo test channel: 0-180 degrees, or -1 = off/detach ("SERVO ch off").
    void send_servo(int ch, int deg);
    // Addressable LED zone (WS2812 or APA102, firmware config.h picks):
    // solid fill ("LEDZ r g b [count]"); 0/0/0 = off.
    void send_led_zone(int r, int g, int b, int count = -1);
    // Local pattern, animated on the MCU (USB stays idle):
    // 0 off, 1 solid, 2 rainbow, 3 chase, 4 breathe.
    void send_led_pattern(int mode, int r, int g, int b, int speed);
    // Software brightness 0-255 (APA102 also maps to its 5-bit global).
    void send_led_brightness(int b);
    // Per-pixel frame for custom panels: rgb = 3 bytes/pixel, chunked into
    // "LEDF <start> <hex>" lines + "LEDSHOW". Future accessory-content hook.
    void send_led_frame(const uint8_t* rgb, int count);
    // One-shot ADC report: request, then poll adc_result() for
    // "ch0 <mV>mV  ch1 <mV>mV  ch2 <mV>mV".
    void request_adc();
    std::string adc_result() const;

    // ── Live pin readout ("PINS" dump) ───────────────────────────────────────
    // request_pins() asks the firmware for a fresh dump of every GP: its
    // role(s) as the FIRMWARE sees them ("btn0", "touch3+i2s_bclk", "free"…)
    // and its live level ("0"/"1", ADC pins "812mv"). pins_snapshot() returns
    // the last COMPLETE dump keyed by GP (empty until one arrives);
    // pins_age_ms() its age, -1 before the first. Poll at a few Hz while a
    // pin visualizer is open — the firmware answers each request once.
    struct PinStat { std::string role, val; };
    void request_pins();
    std::map<int, PinStat> pins_snapshot() const;
    int pins_age_ms() const;

private:
    void reader_loop();                       // transport read + reconnect loop
    void on_line(const std::string& line);    // parse one framed message → dispatch
    void handle_button(int id, bool is_long); // map id→GpioFunc, call dispatch_
    void push_pin_config();                   // send PINCFG map to the firmware

    CoprocConfig                  cfg_;
    std::function<void(GpioFunc)> dispatch_;
    std::atomic<bool>             running_{false};
    std::atomic<bool>             connected_{false};
    std::thread                   thread_;
    int                           fd_ = -1;   // serial or i2c fd
    bool                          pins_pushed_ = false;  // once per connection
    bool                          pins_repushed_ = false;   // mismatch retry, once
    int                           pushed_count_  = 0;    // BTN lines last pushed
    mutable std::mutex            i2c_mtx_;
    std::string                   i2c_result_ = "not scanned";  // last I2CSCAN reply
    mutable std::mutex            adc_mtx_;
    int                           adc_mv_[3] = { -1, -1, -1 };  // last ADC replies

    // PINS dump state: lines accumulate in pinstat_accum_ until "PINS END"
    // publishes them to pinstat_ (so readers never see a half dump).
    mutable std::mutex            pins_mtx_;
    std::map<int, PinStat>        pinstat_;
    std::map<int, PinStat>        pinstat_accum_;
    std::chrono::steady_clock::time_point pinstat_at_{};
    bool                          pinstat_valid_ = false;

    // Peripheral hub state (see set_boop_handler / coproc_temp above).
    std::function<void(int, bool)> boop_fn_;
    struct TempSample { double c; std::chrono::steady_clock::time_point at; };
    mutable std::mutex             temp_mtx_;
    std::map<std::string, TempSample> temps_;   // ROM hex → latest reading
};

} // namespace input
