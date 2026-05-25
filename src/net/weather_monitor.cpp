#include "weather_monitor.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// GET a URL via curl (popen). Returns the response body, or "" on failure. The
// URL is built internally from numeric lat/lon + fixed hosts, so there is nothing
// shell-injectable, but it is single-quoted defensively.
static std::string http_get(const std::string& url) {
    const std::string cmd = "curl -s --max-time 8 '" + url + "'";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    pclose(fp);
    return out;
}

// IP geolocation (free, no key). Fills lat/lon/city on success.
static bool geolocate(double& lat, double& lon, std::string& city) {
    const std::string body = http_get("http://ip-api.com/json");
    if (body.empty()) return false;
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || j.value("status", std::string()) != "success") return false;
    lat  = j.value("lat", 0.0);
    lon  = j.value("lon", 0.0);
    city = j.value("city", std::string());
    return true;
}

void WeatherMonitor::start(AppState* state) {
    state_   = state;
    running_ = true;
    thread_  = std::thread(&WeatherMonitor::thread_fn, this);
}

void WeatherMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void WeatherMonitor::thread_fn() {
    time_t last_fetch = 0;

    while (running_) {
        // Snapshot the settings + a one-shot refresh request.
        const bool force = state_->weather_refresh.exchange(false);
        bool   enabled, autoloc, metric; double lat, lon; std::string place; int interval;
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            enabled  = state_->weather_cfg.enabled;
            autoloc  = state_->weather_cfg.auto_locate;
            metric   = state_->weather_cfg.metric;
            lat      = state_->weather_cfg.lat;
            lon      = state_->weather_cfg.lon;
            place    = state_->weather_cfg.place;
            interval = state_->weather_cfg.interval_min;
        }
        if (interval < 1) interval = 1;

        const time_t now = time(nullptr);
        const bool due = enabled &&
            (force || last_fetch == 0 || (now - last_fetch) >= static_cast<time_t>(interval) * 60);

        if (due) {
            last_fetch = now;

            std::string city = place;
            if (autoloc) {
                double a = 0, o = 0; std::string c;
                if (geolocate(a, o, c)) { lat = a; lon = o; if (!c.empty()) city = c; }
            }

            char url[480];
            snprintf(url, sizeof(url),
                "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                "is_day,weather_code,wind_speed_10m,precipitation"
                "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                "&forecast_days=1"
                "&temperature_unit=%s&wind_speed_unit=%s&precipitation_unit=%s&timezone=auto",
                lat, lon, metric ? "celsius" : "fahrenheit", metric ? "kmh" : "mph",
                metric ? "mm" : "inch");

            const std::string body = http_get(url);
            WeatherState w;   // ok=false unless parse succeeds
            json j = json::parse(body, nullptr, false);
            if (!j.is_discarded() && j.contains("current") && j["current"].is_object()) {
                const auto& c = j["current"];
                w.temp        = c.value("temperature_2m", 0.f);
                w.feels       = c.value("apparent_temperature", w.temp);
                w.wind        = c.value("wind_speed_10m", 0.f);
                w.humidity    = c.value("relative_humidity_2m", -1);
                w.code        = c.value("weather_code", -1);
                w.is_day      = c.value("is_day", 1) != 0;
                w.precip_now  = c.value("precipitation", 0.f);
                w.condition   = wmo_text(w.code);
                w.location    = city;
                w.updated_utc = now;
                w.ok          = true;
                // Daily block: today's high/low + max rain probability (arrays of 1).
                if (j.contains("daily") && j["daily"].is_object()) {
                    const auto& d = j["daily"];
                    auto first = [](const json& arr, float def) {
                        return (arr.is_array() && !arr.empty()) ? arr[0].get<float>() : def;
                    };
                    if (d.contains("temperature_2m_max")) w.temp_high = first(d["temperature_2m_max"], w.temp);
                    if (d.contains("temperature_2m_min")) w.temp_low  = first(d["temperature_2m_min"], w.temp);
                    if (d.contains("precipitation_probability_max")) {
                        const auto& pp = d["precipitation_probability_max"];
                        if (pp.is_array() && !pp.empty() && !pp[0].is_null())
                            w.rain_prob = pp[0].get<int>();
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lk(state_->mtx);
                state_->weather = std::move(w);
            }
        }

        // Short sleep slices so stop()/refresh stay responsive.
        for (int i = 0; i < 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
