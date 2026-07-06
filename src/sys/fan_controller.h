#pragma once
// ── fan_controller.h ────────────────────────────────────────────────────────────
// Pi-driven cooling fan control via software PWM. Up to 4 fans grouped into 2
// independently-controlled zones (e.g. intake / exhaust, or left / right). Each
// zone has its own speed, mode (manual fixed / auto temp-ramp), and curve; one
// PWM thread drives all zones with per-zone duty. Reuses face::GpioOutputGroup
// (libgpiod v2) so no DT overlay is needed; pick pins HUB75 doesn't claim (see
// hardware/carrier-board/PINMAP.md — e.g. BCM 18/19, and 14/15 if free).
//
// All live controls are lock-free atomics so the menu can adjust zones while the
// PWM thread runs.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../face/gpio_v2.h"

namespace sys {

class FanController {
public:
    static constexpr int kMaxZones = 2;
    static constexpr int kMaxFans  = 4;

    struct ZoneCfg {
        std::string      name      = "zone";
        std::vector<int> gpios;                 // BCM lines for this zone's fan(s)
        bool             auto_mode = false;     // false = manual, true = temp-driven
        double           speed     = 0.50;      // manual duty [0,1]
        double           auto_min_c= 50.0;
        double           auto_max_c= 75.0;
    };
    struct Config {
        bool        enabled   = false;
        std::string chip      = "/dev/gpiochip0";
        double      pwm_hz    = 100.0;
        double      min_duty  = 0.20;           // stall floor applied when a zone is on
        bool        invert    = false;          // true if the drive is active-low
        std::string temp_path = "/sys/class/thermal/thermal_zone0/temp";
        // Where the PWM happens: "gpio" (default — this class bit-bangs CM5
        // lines) or "coproc" (the RP2350 owns the fan pins; the curve stays
        // here and resolved duties go out through the duty sink below).
        std::string output    = "gpio";
        std::vector<ZoneCfg> zones;             // up to kMaxZones
    };

    // output == "coproc": called ~2 Hz per zone with the resolved duty [0,1]
    // whenever it changes (plus a periodic refresh so a rebooted coprocessor
    // re-learns it). Set before start(); main wires it to the coproc link.
    using DutySink = std::function<void(int zone, double duty)>;
    void set_duty_sink(DutySink sink) { duty_sink_ = std::move(sink); }

    // Optional temperature source that overrides temp_path — lets the curve
    // follow a probe that lives on the coprocessor instead of a sysfs file.
    // Returns false to fall back to temp_path. Set before start().
    using TempProvider = std::function<bool(double& c_out)>;
    void set_temp_provider(TempProvider fn) { temp_provider_ = std::move(fn); }

    explicit FanController(Config cfg);
    ~FanController();

    bool start();    // claim the GPIO lines + start the PWM thread
    void stop();     // stop the thread, drive fans off, release the lines
    bool running() const { return running_.load(); }

    int  zone_count() const { return nzones_; }
    const std::string& zone_name(int z) const { return zones_[clampz(z)].name; }

    // Live per-zone controls (safe to call any time).
    void   set_zone_auto(int z, bool a)        { zones_[clampz(z)].auto_mode.store(a); }
    bool   zone_auto(int z) const              { return zones_[clampz(z)].auto_mode.load(); }
    void   set_zone_speed(int z, double duty);
    double zone_speed(int z) const             { return zones_[clampz(z)].speed.load(); }
    void   set_zone_auto_range(int z, double min_c, double max_c);
    double zone_auto_min(int z) const          { return zones_[clampz(z)].auto_min.load(); }
    double zone_auto_max(int z) const          { return zones_[clampz(z)].auto_max.load(); }
    double zone_duty(int z) const              { return zones_[clampz(z)].cur_duty.load(); }

    double current_temp_c() const { return cur_temp_.load(); }
    const Config& config() const { return cfg_; }

private:
    struct ZoneRT {
        std::string         name;
        uint64_t            bits = 0;           // line bits within the shared request
        std::atomic<bool>   auto_mode{false};
        std::atomic<double> speed{0.5};
        std::atomic<double> auto_min{50.0};
        std::atomic<double> auto_max{75.0};
        std::atomic<double> cur_duty{0.0};
    };

    int    clampz(int z) const { return (z < 0) ? 0 : (z >= nzones_ ? (nzones_ ? nzones_-1 : 0) : z); }
    void   pwm_loop();
    void   coproc_loop();   // output=="coproc": push duties instead of bit-banging
    double read_temp_c();
    double resolve_duty(const ZoneRT& z) const;   // manual/auto → duty, min_duty floor
    void   apply(uint64_t drive_high);            // honour invert, write the lines

    Config                cfg_;
    face::GpioOutputGroup lines_;
    uint64_t              all_mask_ = 0;
    std::array<ZoneRT, kMaxZones> zones_;
    int                   nzones_ = 0;

    std::atomic<bool>   running_ { false };
    std::atomic<double> cur_temp_{ 0.0 };
    std::thread         thread_;
    DutySink            duty_sink_;      // coproc mode (set before start)
    TempProvider        temp_provider_;  // optional curve source (set before start)
};

} // namespace sys
