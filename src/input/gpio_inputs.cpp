#include "gpio_inputs.h"

#include <gpiod.h>

#include <chrono>
#include <iostream>

// libgpiod v2 (same API generation as gpio_buttons.cpp). We poll line values on
// a fixed interval rather than using edge events so a long-press timeout can be
// detected while the line is held steady.

namespace input {

GpioInputs::GpioInputs(std::vector<GpioPinCfg> pins,
                       std::function<void(GpioFunc)> dispatch, std::string chip)
    : dispatch_(std::move(dispatch)), chip_path_(std::move(chip)) {
    // Keep only usable slots: a real pin with at least one assigned function.
    for (auto& p : pins)
        if (p.gpio >= 0 &&
            (p.short_fn != GpioFunc::None || p.long_fn != GpioFunc::None))
            pins_.push_back(p);
}

GpioInputs::~GpioInputs() { shutdown(); }

bool GpioInputs::init() {
    if (pins_.empty()) return false;

    gpiod_chip* chip = gpiod_chip_open(chip_path_.c_str());
    if (!chip) {
        std::cerr << "[gpio] cannot open " << chip_path_ << "\n";
        return false;
    }

    gpiod_line_config* line_cfg = gpiod_line_config_new();
    if (!line_cfg) { gpiod_chip_close(chip); return false; }

    bool ok = true;
    for (const auto& p : pins_) {
        gpiod_line_settings* s = gpiod_line_settings_new();
        if (!s) { ok = false; break; }
        gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_bias bias = GPIOD_LINE_BIAS_DISABLED;
        if      (p.pull == 1) bias = GPIOD_LINE_BIAS_PULL_UP;
        else if (p.pull == 2) bias = GPIOD_LINE_BIAS_PULL_DOWN;
        gpiod_line_settings_set_bias(s, bias);
        unsigned int off = static_cast<unsigned int>(p.gpio);
        if (gpiod_line_config_add_line_settings(line_cfg, &off, 1, s) < 0) ok = false;
        gpiod_line_settings_free(s);
        if (!ok) break;
    }
    if (!ok) {
        gpiod_line_config_free(line_cfg);
        gpiod_chip_close(chip);
        std::cerr << "[gpio] line config failed\n";
        return false;
    }

    gpiod_request_config* req = gpiod_request_config_new();
    if (req) gpiod_request_config_set_consumer(req, "protohud-inputs");
    gpiod_line_request* request = gpiod_chip_request_lines(chip, req, line_cfg);
    if (req) gpiod_request_config_free(req);
    gpiod_line_config_free(line_cfg);
    gpiod_chip_close(chip);

    if (!request) {
        std::cerr << "[gpio] request_lines failed (pins already in use?)\n";
        return false;
    }

    request_ = request;
    running_ = true;
    thread_  = std::thread(&GpioInputs::poll_loop, this);
    std::cout << "[gpio] input map active (" << pins_.size() << " pins)\n";
    return true;
}

void GpioInputs::shutdown() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (request_) {
        gpiod_line_request_release(static_cast<gpiod_line_request*>(request_));
        request_ = nullptr;
    }
}

void GpioInputs::poll_loop() {
    using clock = std::chrono::steady_clock;
    auto* request = static_cast<gpiod_line_request*>(request_);
    const size_t n = pins_.size();

    std::vector<bool>              pressed(n, false);
    std::vector<bool>              long_fired(n, false);
    std::vector<clock::time_point> t0(n);

    while (running_.load()) {
        const auto now = clock::now();
        for (size_t i = 0; i < n; ++i) {
            const auto& p = pins_[i];
            const gpiod_line_value v =
                gpiod_line_request_get_value(request, static_cast<unsigned int>(p.gpio));
            const int  level = (v == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
            const bool down  = p.active_low ? (level == 0) : (level == 1);

            if (down && !pressed[i]) {
                pressed[i]    = true;
                long_fired[i] = false;
                t0[i]         = now;
            } else if (down && pressed[i]) {
                if (p.long_fn != GpioFunc::None && !long_fired[i]) {
                    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - t0[i]).count();
                    if (held >= p.long_ms) {
                        long_fired[i] = true;
                        if (dispatch_) dispatch_(p.long_fn);
                    }
                }
            } else if (!down && pressed[i]) {
                pressed[i] = false;
                // Short press only if the long-press action didn't already fire.
                if (!long_fired[i] && p.short_fn != GpioFunc::None) {
                    if (dispatch_) dispatch_(p.short_fn);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

} // namespace input
