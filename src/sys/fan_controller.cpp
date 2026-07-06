#include "fan_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace sys {

FanController::FanController(Config cfg) : cfg_(std::move(cfg)) {
    // Seed per-zone runtime from config (up to kMaxZones).
    for (const auto& zc : cfg_.zones) {
        if (nzones_ >= kMaxZones) break;
        ZoneRT& z = zones_[nzones_++];
        z.name = zc.name;
        z.auto_mode.store(zc.auto_mode);
        z.speed.store(std::clamp(zc.speed, 0.0, 1.0));
        z.auto_min.store(zc.auto_min_c);
        z.auto_max.store(zc.auto_max_c);
    }
}

FanController::~FanController() { stop(); }

void FanController::set_zone_speed(int z, double duty) {
    zones_[clampz(z)].speed.store(std::clamp(duty, 0.0, 1.0));
}

void FanController::set_zone_auto_range(int z, double min_c, double max_c) {
    if (max_c < min_c + 1.0) max_c = min_c + 1.0;
    ZoneRT& zr = zones_[clampz(z)];
    zr.auto_min.store(min_c);
    zr.auto_max.store(max_c);
}

bool FanController::start() {
    if (running_.load()) return true;
    if (nzones_ == 0) { std::cerr << "[fan] no zones configured\n"; return false; }

    // Coprocessor mode: the RP2350 owns the fan pins; no CM5 GPIO to claim.
    // The curve/menu logic all stays here — only the PWM output moves.
    if (cfg_.output == "coproc") {
        if (!duty_sink_) { std::cerr << "[fan] output=coproc but no duty sink wired\n"; return false; }
        running_.store(true);
        thread_ = std::thread(&FanController::coproc_loop, this);
        std::cout << "[fan] cooling fans active (" << nzones_
                  << " zone(s) via coprocessor PWM)\n";
        return true;
    }

    // Flatten all zone GPIOs into one shared line request; remember each zone's
    // bit positions so the PWM thread can drive zones independently.
    std::vector<uint32_t> offsets;
    for (int i = 0; i < nzones_; ++i) {
        uint64_t bits = 0;
        for (int g : cfg_.zones[i].gpios) {
            if (g < 0) continue;
            if (static_cast<int>(offsets.size()) >= kMaxFans) {
                std::cerr << "[fan] more than " << kMaxFans << " fans configured; extra ignored\n";
                break;
            }
            bits |= (1ull << offsets.size());
            offsets.push_back(static_cast<uint32_t>(g));
        }
        zones_[i].bits = bits;
    }
    if (offsets.empty()) { std::cerr << "[fan] no GPIO lines configured\n"; return false; }

    if (!lines_.open(cfg_.chip, offsets.data(), static_cast<int>(offsets.size()),
                     "protohud-fans")) {
        std::cerr << "[fan] failed to claim GPIO lines on " << cfg_.chip
                  << " (already in use?)\n";
        return false;
    }
    all_mask_ = (offsets.size() >= 64) ? ~0ull : ((1ull << offsets.size()) - 1ull);
    running_.store(true);
    thread_ = std::thread(&FanController::pwm_loop, this);
    std::cout << "[fan] cooling fans active (" << nzones_ << " zone(s), "
              << offsets.size() << " line(s))\n";
    return true;
}

void FanController::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (lines_.is_open()) {
        apply(0);            // all zones off
        lines_.close();
    } else if (cfg_.output == "coproc" && duty_sink_) {
        for (int i = 0; i < nzones_; ++i) duty_sink_(i, 0.0);   // fans off
    }
}

void FanController::apply(uint64_t drive_high) {
    // drive_high is the active-high intent; honour active-low wiring on write.
    const uint64_t out = cfg_.invert ? (~drive_high & all_mask_) : drive_high;
    lines_.set_values(out, all_mask_);
}

double FanController::read_temp_c() {
    // Preferred source: an injected provider (e.g. a probe on the coprocessor).
    if (temp_provider_) {
        double c = 0.0;
        if (temp_provider_(c)) return c;
        // fall through to temp_path when the provider has no reading yet
    }
    FILE* f = std::fopen(cfg_.temp_path.c_str(), "r");
    if (!f) return cur_temp_.load();
    long milli = 0;
    const int n = std::fscanf(f, "%ld", &milli);
    std::fclose(f);
    return (n == 1) ? milli / 1000.0 : cur_temp_.load();
}

double FanController::resolve_duty(const ZoneRT& z) const {
    double duty;
    if (z.auto_mode.load()) {
        const double t  = cur_temp_.load();
        const double lo = z.auto_min.load(), hi = z.auto_max.load();
        duty = std::clamp((t - lo) / std::max(1.0, hi - lo), 0.0, 1.0);
    } else {
        duty = z.speed.load();
    }
    if (duty > 0.001 && duty < cfg_.min_duty) duty = cfg_.min_duty;   // stall floor
    return std::clamp(duty, 0.0, 1.0);
}

// output == "coproc": the RP2350's hardware PWM holds the duty, so this loop
// only re-resolves each zone's target ~2 Hz and pushes it when it moves (plus
// a 5 s heartbeat so a rebooted coprocessor re-learns the duties).
void FanController::coproc_loop() {
    using clock = std::chrono::steady_clock;
    std::array<double, kMaxZones> sent;
    sent.fill(-1.0);                                  // force the first push
    auto last_push = clock::now();

    while (running_.load()) {
        cur_temp_.store(read_temp_c());
        const bool heartbeat = clock::now() - last_push >= std::chrono::seconds(5);
        for (int i = 0; i < nzones_; ++i) {
            const double d = resolve_duty(zones_[i]);
            zones_[i].cur_duty.store(d);
            if (heartbeat || std::fabs(d - sent[i]) > 0.01) {
                duty_sink_(i, d);
                sent[i] = d;
            }
        }
        if (heartbeat) last_push = clock::now();
        for (int t = 0; t < 5 && running_.load(); ++t)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void FanController::pwm_loop() {
    using clock = std::chrono::steady_clock;
    using dsec  = std::chrono::duration<double>;
    const double period = 1.0 / std::max(1.0, cfg_.pwm_hz);
    auto last_temp = clock::now() - std::chrono::seconds(10);

    while (running_.load()) {
        bool any_auto = false;
        for (int i = 0; i < nzones_; ++i) any_auto |= zones_[i].auto_mode.load();
        if (any_auto && clock::now() - last_temp >= std::chrono::seconds(2)) {
            cur_temp_.store(read_temp_c());
            last_temp = clock::now();
        }

        // Per-zone duty → an initial high mask plus mid-period "turn off" events
        // (independent duties, so we can't just toggle everything together).
        uint64_t high = 0;
        struct Ev { double t; uint64_t bits; };
        std::array<Ev, kMaxZones> events; int nev = 0;
        for (int i = 0; i < nzones_; ++i) {
            const double d = resolve_duty(zones_[i]);
            zones_[i].cur_duty.store(d);
            const uint64_t b = zones_[i].bits;
            if (b == 0 || d <= 0.001) continue;          // stays low
            high |= b;
            if (d < 0.999) events[nev++] = { period * d, b };   // off partway through
        }

        apply(high);
        if (nev == 0) {                                  // all zones fully off or full on
            std::this_thread::sleep_for(dsec(period));
            continue;
        }
        std::sort(events.begin(), events.begin() + nev,
                  [](const Ev& a, const Ev& b){ return a.t < b.t; });
        double t = 0.0;
        for (int i = 0; i < nev && running_.load(); ++i) {
            if (events[i].t > t) std::this_thread::sleep_for(dsec(events[i].t - t));
            t = events[i].t;
            high &= ~events[i].bits;                     // turn this zone off
            apply(high);
        }
        if (period > t) std::this_thread::sleep_for(dsec(period - t));
    }
}

} // namespace sys
