#pragma once
// ── gpio_inputs.h ───────────────────────────────────────────────────────────────
// Configurable GPIO switch input. Each pin is assigned a short-press and an
// optional long-press function (input::GpioFunc) with configurable pull bias and
// polarity. A background poll thread debounces the lines and dispatches the
// mapped function on release (short) / hold (long). Replaces the fixed 3-button
// GpioButtons controller with a user-remappable map driven by config.json.

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gpio_function.h"

namespace input {

struct GpioPinCfg {
    int      gpio       = -1;                 // BCM line number; < 0 = unused slot
    bool     active_low = true;               // pressed = LOW (switch to GND + pull-up)
    int      pull       = 1;                  // 0 = none, 1 = up, 2 = down
    GpioFunc short_fn   = GpioFunc::None;     // fired on release (held < long_ms)
    GpioFunc long_fn    = GpioFunc::None;     // fired once when held >= long_ms
    int      long_ms    = 600;                // long-press threshold (ms)
};

class GpioInputs {
public:
    // dispatch is invoked (from the poll thread) with the function to perform.
    GpioInputs(std::vector<GpioPinCfg> pins, std::function<void(GpioFunc)> dispatch,
               std::string chip = "/dev/gpiochip0");
    ~GpioInputs();

    bool init();        // request lines + start polling; false if no usable pins / chip error
    void shutdown();

private:
    void poll_loop();

    std::vector<GpioPinCfg>        pins_;     // only entries with gpio >= 0 and a function
    std::function<void(GpioFunc)>  dispatch_;
    std::string                    chip_path_;
    void*                          request_ = nullptr;   // gpiod_line_request*
    std::atomic<bool>              running_{false};
    std::thread                    thread_;
};

} // namespace input
