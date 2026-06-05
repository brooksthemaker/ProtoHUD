#pragma once
// ── fan_controller.h ────────────────────────────────────────────────────────────
// Pi-driven cooling fan control via software PWM on one or more GPIO lines
// (drive a 2-pin fan through a MOSFET, or a 4-pin fan's control line). Reuses
// face::GpioOutputGroup (libgpiod v2) so no DT overlay is needed; pick pins that
// HUB75 doesn't claim (see hardware/carrier-board/PINMAP.md — e.g. BCM 18/19).
//
// Two modes: "manual" (fixed speed) and "auto" (speed ramps with CPU temperature
// between auto_min_c and auto_max_c). All live controls are lock-free atomics so
// the menu can adjust speed/mode while the PWM thread runs.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "../face/gpio_v2.h"

namespace sys {

class FanController {
public:
    struct Config {
        bool             enabled   = false;
        std::vector<int> gpios;                 // BCM lines driving the fans
        std::string      chip      = "/dev/gpiochip0";
        double           pwm_hz    = 100.0;     // software-PWM carrier frequency
        double           min_duty  = 0.20;      // fans stall below this; floor when on
        bool             invert    = false;     // true if the drive is active-low
        bool             auto_mode = false;     // false = manual, true = temp-driven
        double           speed     = 0.50;      // manual duty [0,1]
        double           auto_min_c= 50.0;      // temp where the fan starts ramping
        double           auto_max_c= 75.0;      // temp where the fan hits full speed
        std::string      temp_path = "/sys/class/thermal/thermal_zone0/temp";
    };

    explicit FanController(Config cfg);
    ~FanController();

    bool start();    // claim the GPIO lines + start the PWM thread
    void stop();     // stop the thread, drive fans off, release the lines
    bool running() const { return running_.load(); }

    // Live controls (safe to call any time).
    void   set_auto_mode(bool a)              { auto_mode_.store(a); }
    bool   auto_mode() const                  { return auto_mode_.load(); }
    void   set_speed(double duty);            // manual duty [0,1]
    double speed() const                      { return speed_.load(); }
    void   set_auto_range(double min_c, double max_c);
    double auto_min_c() const                 { return auto_min_.load(); }
    double auto_max_c() const                 { return auto_max_.load(); }

    // Observability for the menu/HUD.
    double current_duty() const   { return cur_duty_.load(); }
    double current_temp_c() const { return cur_temp_.load(); }

    const Config& config() const { return cfg_; }

private:
    void   pwm_loop();
    double read_temp_c();
    double resolve_duty();   // manual/auto → duty, with the min_duty floor applied

    Config                cfg_;
    face::GpioOutputGroup lines_;
    std::vector<uint32_t> offsets_;
    uint64_t              mask_ = 0;

    std::atomic<bool>   running_  { false };
    std::atomic<bool>   auto_mode_{ false };
    std::atomic<double> speed_    { 0.5 };
    std::atomic<double> auto_min_ { 50.0 };
    std::atomic<double> auto_max_ { 75.0 };
    std::atomic<double> cur_duty_ { 0.0 };
    std::atomic<double> cur_temp_ { 0.0 };

    std::thread thread_;
};

} // namespace sys
