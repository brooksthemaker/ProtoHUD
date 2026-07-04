#pragma once
// ── temp_sensors.h ────────────────────────────────────────────────────────────
// Polls a list of temperature sensors and publishes the readings into AppState
// (see AppState::temps) at a fixed rate. A background thread reads each sensor,
// flags warn/crit thresholds, and the HUD / menu / fan controller consume the
// results. Never required — disabled unless cfg.enabled.
//
// v1 sources:
//   ds18b20  — DS18B20 (or any 1-Wire probe) via the kernel w1 driver. Reads
//              /sys/bus/w1/devices/<id>/temperature (milli-°C), falling back to
//              the legacy w1_slave "t=…" line. Many probes share one GPIO; each
//              has a unique 28-XXXXXXXX id (dtoverlay=w1-gpio,gpiopin=<n>).
// The source `type` is stored so max31865 (PT100 via IIO) and i2c chips can be
// added later without touching the callers.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct AppState;

namespace sensor {

struct TempSensorCfg {
    std::string type   = "ds18b20";   // ds18b20 (v1); max31865 | i2c (future)
    std::string id;                   // ds18b20: "28-XXXXXXXX" ("" = first found)
    std::string label  = "Temp";
    double      warn_c = 60.0;
    double      crit_c = 75.0;
};

struct TempSensorsConfig {
    bool enabled = false;
    int  poll_ms = 1000;
    std::vector<TempSensorCfg> sensors;
};

class TempSensors {
public:
    TempSensors(TempSensorsConfig cfg, AppState& state);
    ~TempSensors();

    bool start();   // spawn the poll thread; false if disabled / no sensors
    void stop();
    bool running() const { return running_.load(); }

    // 1-Wire device ids present on the bus (folder names under
    // /sys/bus/w1/devices matching 28-*). For setup / the menu's "detected" list.
    static std::vector<std::string> discover_ds18b20();

private:
    void   loop();
    // °C for one sensor, or NaN on failure. Dispatches on cfg.type.
    double read_one(const TempSensorCfg& s) const;
    double read_ds18b20(const std::string& id) const;

    TempSensorsConfig cfg_;
    AppState&         state_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

}  // namespace sensor
