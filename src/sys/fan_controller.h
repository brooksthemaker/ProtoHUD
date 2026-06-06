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
        std::vector<ZoneCfg> zones;             // up to kMaxZones
    };

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
};

} // namespace sys
