#pragma once
// ── mpr121_boop_sensor.h ──────────────────────────────────────────────────────
// MPR121 12-electrode capacitive-touch driver speaking to the chip directly
// over Linux i2c-dev (no external library). One chip handles all three boop
// zones (snout + left/right cheek) from a single I²C address — capacitive
// sensing reaches *through* the non-conductive visor / shell, so the pads
// bond to the inside surface and the user touches the outside.
//
// Falls back to ToF (VL53L1X) cleanly by sharing the BoopSensor interface; a
// future Vl53l1xBoopSensor can drop in without touching the rest of the
// codebase.

#include "boop_sensor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace sensor {

class Mpr121BoopSensor : public BoopSensor {
public:
    struct Config {
        bool        enabled  = false;
        std::string i2c_bus  = "/dev/i2c-1";   // GPIO 2/3 on CM5 40-pin header
        int         i2c_addr = 0x5A;           // MPR121 default; alt 0x5B/5C/5D via ADDR pin

        // Which MPR121 electrode (0..11) maps to each zone (Snout, LeftCheek,
        // RightCheek). -1 disables that zone.
        std::array<int8_t,  ZoneCount> electrode        = { 0,  1,  2 };
        // Touch/release threshold pairs. Touch must be > release for the
        // chip's hysteresis to behave; defaults are MPR121-typical for skin
        // touch through a couple of mm of plastic.
        std::array<uint8_t, ZoneCount> touch_threshold  = { 12, 12, 12 };
        std::array<uint8_t, ZoneCount> release_threshold = { 6,  6,  6 };
        std::array<bool,    ZoneCount> zone_enabled     = { true, true, true };

        double poll_hz     = 30.0;   // sensor sampling rate
        double refractory_s = 0.25;  // min seconds between boops on the same zone
    };

    explicit Mpr121BoopSensor(Config cfg);
    ~Mpr121BoopSensor() override;

    bool start() override;
    void stop()  override;

    void set_zone_enabled  (Zone z, bool enabled)   override;
    void set_zone_threshold(Zone z, uint8_t touch)  override;

private:
    bool open_bus();
    bool init_chip_locked();   // caller holds bus_mtx_
    bool write_reg_locked(uint8_t reg, uint8_t val);
    bool read_regs_locked (uint8_t reg, uint8_t* buf, size_t len);
    void apply_zone_threshold_locked(uint8_t zone_idx);

    void poll_loop();

    Config            cfg_;
    int               i2c_fd_ = -1;
    std::mutex        bus_mtx_;   // serialises all I²C traffic + cfg_ access from the poll thread

    std::atomic<bool> running_{false};
    std::thread       thread_;

    // Last per-zone touched state and time-of-last-boop, used to debounce.
    std::array<bool,                                    ZoneCount> last_touched_ = { false, false, false };
    std::array<std::chrono::steady_clock::time_point,   ZoneCount> last_boop_t_;
};

} // namespace sensor
