#pragma once
// ── light_sensor.h ────────────────────────────────────────────────────────────
// Ambient-light driver for the squint-reaction trigger. The face renderer
// catches the user transitioning from dark→bright (helmet stepping out into
// sunlight) and fires a boop-style squint expression for a configurable
// duration before reverting.
//
// Two chips are supported, selected by Config::type:
//   BH1750  — cheap, common, opcode-driven continuous-conversion device.
//   OPT3001 — TI register-addressed lux sensor with on-chip auto-ranging
//             (0.01–83k lux) and a true photopic filter, so readings track
//             the human eye much closer than the BH1750 under LED/IR light.
// The Type enum leaves room for TSL2591 / VEML7700 later without changing
// callers.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sensor {

class LightSensor {
public:
    enum class Type : uint8_t { Bh1750 = 0, Opt3001 = 1 };

    // Config-string ↔ Type helpers, shared by main()'s parser and the menu's
    // pin-claim / I²C-tooltip labels so the mapping lives in one place.
    static Type type_from_string(const std::string& s) {
        return (s == "opt3001") ? Type::Opt3001 : Type::Bh1750;
    }
    static const char* chip_name(Type t) {
        return t == Type::Opt3001 ? "OPT3001" : "BH1750";
    }
    // BH1750: 0x23 (0x5C with ADDR high). OPT3001: ADDR pin picks 0x44–0x47
    // (GND=0x44, VDD=0x45, SDA=0x46, SCL=0x47).
    static int default_addr(Type t) {
        return t == Type::Opt3001 ? 0x44 : 0x23;
    }

    struct Config {
        bool        enabled    = false;
        Type        type       = Type::Bh1750;
        std::string i2c_bus    = "/dev/i2c-1";   // GPIO 2/3 on CM5 40-pin header
        int         i2c_addr   = 0x23;            // see default_addr() per chip
        float       poll_hz    = 8.0f;            // both chips convert in ~100-120 ms
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
    bool init_opt3001();
    bool read_opt3001(float& lux);

    Config             cfg_;
    int                fd_      = -1;
    LuxCallback        cb_;
    std::atomic<bool>  running_{false};
    std::atomic<float> latest_lux_{-1.f};
    std::thread        thr_;
};

} // namespace sensor
