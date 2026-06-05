#include "fan_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace sys {

FanController::FanController(Config cfg) : cfg_(std::move(cfg)) {
    auto_mode_.store(cfg_.auto_mode);
    speed_.store(std::clamp(cfg_.speed, 0.0, 1.0));
    auto_min_.store(cfg_.auto_min_c);
    auto_max_.store(cfg_.auto_max_c);
}

FanController::~FanController() { stop(); }

void FanController::set_speed(double duty) {
    speed_.store(std::clamp(duty, 0.0, 1.0));
}

void FanController::set_auto_range(double min_c, double max_c) {
    if (max_c < min_c + 1.0) max_c = min_c + 1.0;   // keep a usable span
    auto_min_.store(min_c);
    auto_max_.store(max_c);
}

bool FanController::start() {
    if (running_.load()) return true;
    offsets_.clear();
    for (int g : cfg_.gpios) if (g >= 0) offsets_.push_back(static_cast<uint32_t>(g));
    if (offsets_.empty()) {
        std::cerr << "[fan] no GPIO lines configured\n";
        return false;
    }
    if (!lines_.open(cfg_.chip, offsets_.data(),
                     static_cast<int>(offsets_.size()), "protohud-fans")) {
        std::cerr << "[fan] failed to claim GPIO lines on " << cfg_.chip
                  << " (already in use?)\n";
        return false;
    }
    mask_ = (offsets_.size() >= 64) ? ~0ull
                                    : ((1ull << offsets_.size()) - 1ull);
    running_.store(true);
    thread_ = std::thread(&FanController::pwm_loop, this);
    std::cout << "[fan] cooling fans active (" << offsets_.size() << " line(s))\n";
    return true;
}

void FanController::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (lines_.is_open()) {
        lines_.set_values(cfg_.invert ? mask_ : 0, mask_);   // drive off
        lines_.close();
    }
}

double FanController::read_temp_c() {
    FILE* f = std::fopen(cfg_.temp_path.c_str(), "r");
    if (!f) return cur_temp_.load();
    long milli = 0;
    const int n = std::fscanf(f, "%ld", &milli);
    std::fclose(f);
    return (n == 1) ? milli / 1000.0 : cur_temp_.load();
}

double FanController::resolve_duty() {
    double duty;
    if (auto_mode_.load()) {
        const double t  = cur_temp_.load();
        const double lo = auto_min_.load(), hi = auto_max_.load();
        duty = std::clamp((t - lo) / std::max(1.0, hi - lo), 0.0, 1.0);
    } else {
        duty = speed_.load();
    }
    // Floor to min_duty once the fan is meant to be moving, so it doesn't stall.
    if (duty > 0.001 && duty < cfg_.min_duty) duty = cfg_.min_duty;
    return std::clamp(duty, 0.0, 1.0);
}

void FanController::pwm_loop() {
    using clock = std::chrono::steady_clock;
    const double period = 1.0 / std::max(1.0, cfg_.pwm_hz);
    const auto on_high = [&](bool high){
        lines_.set_values((high != cfg_.invert) ? mask_ : 0, mask_);
    };
    auto last_temp = clock::now() - std::chrono::seconds(10);

    while (running_.load()) {
        if (auto_mode_.load() &&
            clock::now() - last_temp >= std::chrono::seconds(2)) {
            cur_temp_.store(read_temp_c());
            last_temp = clock::now();
        }
        const double duty = resolve_duty();
        cur_duty_.store(duty);

        if (duty <= 0.001) {                      // fully off
            on_high(false);
            std::this_thread::sleep_for(std::chrono::duration<double>(period));
        } else if (duty >= 0.999) {               // full speed
            on_high(true);
            std::this_thread::sleep_for(std::chrono::duration<double>(period));
        } else {
            on_high(true);
            std::this_thread::sleep_for(std::chrono::duration<double>(period * duty));
            on_high(false);
            std::this_thread::sleep_for(std::chrono::duration<double>(period * (1.0 - duty)));
        }
    }
}

} // namespace sys
