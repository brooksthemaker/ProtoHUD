#include "accessory_leds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace accessory {

AccessoryLeds::AccessoryLeds(Config cfg)
    : cfg_(std::move(cfg)),
      strip_(cfg_.strip)
{
    strip_.set_global_brightness(cfg_.global_brightness);
}

AccessoryLeds::~AccessoryLeds() { stop(); }

bool AccessoryLeds::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    if (!strip_.open()) {
        std::fprintf(stderr, "[led] accessory LEDs unavailable — SPI open failed\n");
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&AccessoryLeds::render_loop, this);
    return true;
}

void AccessoryLeds::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // Blank the strip on shutdown so leftover pixels don't linger.
    strip_.fill(0, 0, 0);
    strip_.show();
    strip_.close();
}

void AccessoryLeds::set_zone_pattern(Zone z, Pattern p) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].pattern = p;
}

void AccessoryLeds::set_zone_color(Zone z, uint8_t r, uint8_t g, uint8_t b) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].r = r;
    cfg_.zones[zi].g = g;
    cfg_.zones[zi].b = b;
}

void AccessoryLeds::set_zone_breathe_hz(Zone z, float hz) {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return;
    if (hz < 0.05f) hz = 0.05f;
    if (hz > 5.0f)  hz = 5.0f;
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    cfg_.zones[zi].breathe_hz = hz;
}

void AccessoryLeds::set_global_brightness(uint8_t b) {
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        cfg_.global_brightness = b;
    }
    // strip_ has its own atomic-enough store; safe to call outside the lock.
    strip_.set_global_brightness(b);
}

ZoneConfig AccessoryLeds::zone(Zone z) const {
    const auto zi = static_cast<int>(z);
    if (zi < 0 || zi >= ZoneCount) return {};
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(cfg_mtx_));
    return cfg_.zones[zi];
}

uint8_t AccessoryLeds::global_brightness() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(cfg_mtx_));
    return cfg_.global_brightness;
}

void AccessoryLeds::render_loop() {
    using clock = std::chrono::steady_clock;
    const double period_s = 1.0 / std::max(1.0, cfg_.frame_hz);
    const auto period = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::duration<double>(period_s));

    const auto t0 = clock::now();

    while (running_.load()) {
        const auto next_t = clock::now() + period;
        const double t = std::chrono::duration<double>(clock::now() - t0).count();

        // Snapshot zone state under the lock; release before any SPI work so
        // the menu thread doesn't stall on the SPI write.
        std::array<ZoneConfig, ZoneCount> zones;
        {
            std::lock_guard<std::mutex> lk(cfg_mtx_);
            zones = cfg_.zones;
        }

        // Start from black — anything we don't write stays dark.
        strip_.fill(0, 0, 0);

        for (const auto& z : zones) {
            if (z.count <= 0) continue;
            switch (z.pattern) {
            case Pattern::Off:
                // already cleared by fill()
                break;
            case Pattern::Solid: {
                for (int i = 0; i < z.count; ++i)
                    strip_.set_pixel(z.start + i, z.r, z.g, z.b);
                break;
            }
            case Pattern::Breathe: {
                // (1 - cos) / 2 keeps the envelope in [0, 1], crests bright,
                // troughs at zero — feels more "breathing" than a sine.
                const double phase = 2.0 * 3.14159265358979323846 * z.breathe_hz * t;
                const double env   = 0.5 * (1.0 - std::cos(phase));
                const uint8_t r = static_cast<uint8_t>(z.r * env);
                const uint8_t g = static_cast<uint8_t>(z.g * env);
                const uint8_t b = static_cast<uint8_t>(z.b * env);
                for (int i = 0; i < z.count; ++i)
                    strip_.set_pixel(z.start + i, r, g, b);
                break;
            }
            }
        }

        strip_.show();
        std::this_thread::sleep_until(next_t);
    }
}

} // namespace accessory
