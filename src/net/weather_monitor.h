#pragma once
#include "../app_state.h"
#include <atomic>
#include <thread>

// Fetches current weather from Open-Meteo (no API key) every weather_cfg.interval_min,
// off the render thread, and writes AppState::weather. Location comes from IP
// geolocation (ip-api.com) when weather_cfg.auto_locate is set, else from the
// configured lat/lon. Networking is done via `curl` (popen), like the ping/bt
// monitors; if curl or the network is unavailable, weather just reports not-ok.
class WeatherMonitor {
public:
    WeatherMonitor()  = default;
    ~WeatherMonitor() { stop(); }

    void start(AppState* state);
    void stop();
    bool is_running() const { return running_.load(); }

private:
    void thread_fn();

    AppState*         state_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_ { false };
};
