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

        // Which MPR121 electrode (0..11) maps to each zone. -1 disables a
        // zone or marks it as "derived" (BothCheeks is the only derived
        // zone today — its events come from the coalescer, not a direct
        // electrode read, so its slot here is -1 by default). The newer
        // zones (TopHead / MouthTop / MouthBottom) ship unassigned: give
        // each an electrode in the menu once its pad is physically wired,
        // so unwired chips can't false-trigger them.
        std::array<int8_t,  ZoneCount> electrode        = { 0,  1,  2, -1, -1, -1, -1 };
        // Touch/release threshold pairs. Touch must be > release for the
        // chip's hysteresis to behave; defaults are MPR121-typical for skin
        // touch through a couple of mm of plastic. The BothCheeks slot's
        // thresholds are unused (it's derived) but kept for index parity.
        std::array<uint8_t, ZoneCount> touch_threshold   = { 12, 12, 12, 12, 12, 12, 12 };
        std::array<uint8_t, ZoneCount> release_threshold = {  6,  6,  6,  6,  6,  6,  6 };
        std::array<bool,    ZoneCount> zone_enabled      = { true, true, true, true,
                                                             true, true, true };

        double poll_hz      = 30.0;   // sensor sampling rate
        double refractory_s = 0.25;   // min seconds between boops on the same zone

        // Coalesce window for cheek events. When LeftCheek and RightCheek
        // both land touch events within this window, both single-side
        // events are suppressed and a BothCheeks event fires instead. Set
        // to 0 to disable coalescing entirely (single-side events fire
        // immediately, no Both detection). The default of 100 ms feels
        // natural in testing while keeping single-touch latency low.
        double coalesce_window_s = 0.10;
    };

    explicit Mpr121BoopSensor(Config cfg);
    ~Mpr121BoopSensor() override;

    bool start() override;
    void stop()  override;

    void set_zone_enabled  (Zone z, bool enabled)   override;
    void set_zone_threshold(Zone z, uint8_t touch)  override;
    void set_coalesce_window_s(double seconds) override;

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
    std::array<bool,                                    ZoneCount> last_touched_ = {};
    std::array<std::chrono::steady_clock::time_point,   ZoneCount> last_boop_t_;

    // Coalescer state for the two cheek zones. When one cheek's rising
    // edge fires, we hold the event pending for coalesce_window_s; if the
    // other cheek arrives inside the window the held events are dropped
    // and a BothCheeks event fires instead, otherwise the held single-
    // side event releases at expiry. Index 0 = LeftCheek, 1 = RightCheek.
    struct PendingTouch {
        bool                                       active = false;
        std::chrono::steady_clock::time_point      expires_at;
    };
    std::array<PendingTouch, 2> cheek_pending_;
};

} // namespace sensor
