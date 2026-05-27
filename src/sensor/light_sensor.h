#pragma once
// ── light_sensor.h ────────────────────────────────────────────────────────────
// Ambient-light driver for the squint-reaction trigger. The face renderer
// catches the user transitioning from dark→bright (helmet stepping out into
// sunlight) and fires a boop-style squint expression for a configurable
// duration before reverting.
//
// Initial implementation drives a BH1750 — cheap, common, single-register
// continuous-conversion I²C device. The Type enum + factory leaves room for
// TSL2591 / VEML7700 later without changing callers.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sensor {

class LightSensor {
public:
    enum class Type : uint8_t { Bh1750 = 0 };

    struct Config {
        bool        enabled    = false;
        Type        type       = Type::Bh1750;
        std::string i2c_bus    = "/dev/i2c-1";   // GPIO 2/3 on CM5 40-pin header
        int         i2c_addr   = 0x23;            // BH1750 default; 0x5C if ADDR high
        float       poll_hz    = 8.0f;            // sensor settles in ~120 ms (high-res mode)
    };

    using LuxCallback = std::function<void(float lux)>;

    explicit LightSensor(const Config& cfg);
    ~LightSensor();

    // Receive each lux sample on the worker thread. Caller is responsible for
    // any synchronisation back to the main thread.
    void set_lux_callback(LuxCallback cb) { cb_ = std::move(cb); }

    bool start();
    void stop();
    bool connected() const { return running_.load(); }

    // Latest sampled lux for debug / display. -1 if no sample yet.
    float latest_lux() const { return latest_lux_.load(); }

private:
    void run();
    bool init_bh1750();
    bool read_bh1750(float& lux);

    Config             cfg_;
    int                fd_      = -1;
    LuxCallback        cb_;
    std::atomic<bool>  running_{false};
    std::atomic<float> latest_lux_{-1.f};
    std::thread        thr_;
};

} // namespace sensor
