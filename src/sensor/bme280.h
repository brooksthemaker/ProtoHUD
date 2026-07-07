#pragma once
// ── bme280.h ──────────────────────────────────────────────────────────────────
// Bosch BME280 environment sensor over I²C (0x76, or 0x77 with SDO high):
// temperature, relative humidity and barometric pressure. Polled slowly
// (default every 5 s — the environment doesn't hurry); readings land in
// AppState::env for the System > Temperature menu and future reactions
// (cold+humid fog-breath, pressure-drop storm warnings).
//
// Uses Bosch's integer compensation math (datasheet §4.2.3) with the shared
// t_fine term. Callback fires on the poll thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace sensor {

class Bme280 {
public:
    struct Config {
        bool        enabled  = false;
        std::string i2c_bus  = "/dev/i2c-1";
        int         i2c_addr = 0x76;    // 0x77 if SDO tied high
        double      poll_s   = 5.0;
    };

    struct Reading {
        float temp_c       = 0.f;
        float humidity_pct = 0.f;
        float pressure_hpa = 0.f;
    };
    using Callback = std::function<void(const Reading&)>;

    explicit Bme280(const Config& cfg) : cfg_(cfg) {}
    ~Bme280() { stop(); }

    void set_callback(Callback cb) { cb_ = std::move(cb); }

    bool start();
    void stop();
    bool connected() const { return running_.load(); }

private:
    void poll_loop();
    bool init_chip();
    bool wr(uint8_t reg, uint8_t val);
    int  rd(uint8_t reg);
    int  rd_block(uint8_t reg, uint8_t* buf, int n);

    Config            cfg_;
    int               fd_ = -1;
    std::thread       thread_;
    std::atomic<bool> running_{ false };
    Callback          cb_;

    // Calibration words read once at init (datasheet register map).
    uint16_t T1_ = 0; int16_t T2_ = 0, T3_ = 0;
    uint16_t P1_ = 0; int16_t P2_ = 0, P3_ = 0, P4_ = 0, P5_ = 0,
             P6_ = 0, P7_ = 0, P8_ = 0, P9_ = 0;
    uint8_t  H1_ = 0, H3_ = 0; int16_t H2_ = 0, H4_ = 0, H5_ = 0;
    int8_t   H6_ = 0;
};

}  // namespace sensor
