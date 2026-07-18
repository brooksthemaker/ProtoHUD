#pragma once
// ── boop_sensor.h ─────────────────────────────────────────────────────────────
// Abstract interface for boop-zone proximity / touch sensors. Concrete
// backends (capacitive MPR121 in v1; ToF VL53L1X possible later) report
// per-zone "touch" events through the on_boop() callback, debounced by a
// configurable refractory window so continuous proximity doesn't strobe the
// face's trigger_boop() every poll cycle.
//
// Hardware-specific configuration (I²C bus, addresses, electrode→zone mapping,
// per-electrode thresholds) lives in each backend's own Config struct.
// User-visible behaviour (which expression each zone fires, how long it
// holds, whether the zone is enabled at all) lives in AppState::boop_zones and
// is wired in main.cpp's on_boop callback.

#include <cstdint>
#include <functional>

namespace sensor {

class BoopSensor {
public:
    enum class Zone : uint8_t {
        Snout       = 0,
        LeftCheek   = 1,
        RightCheek  = 2,
        // Derived (not directly measured): fires when both cheek electrodes
        // land touch events within the configured coalesce window. Keeps its
        // historic slot 3 — configs index zones by this number.
        BothCheeks  = 3,
        TopHead     = 4,
        MouthTop    = 5,
        MouthBottom = 6,
    };
    static constexpr uint8_t ZoneCount = 7;

    using BoopCallback = std::function<void(Zone)>;

    virtual ~BoopSensor() = default;

    // Open the I²C bus, configure the chip, and start the polling thread.
    // Returns false on hardware failure; callers should keep running without
    // the sensor in that case (the rest of the HUD is unaffected).
    virtual bool start() = 0;
    virtual void stop()  = 0;

    // Live tuning from the menu — picked up by the next poll cycle. Threshold
    // is the raw MPR121-style touch sensitivity number (lower = more
    // sensitive); ToF backends are free to remap.
    virtual void set_zone_enabled  (Zone, bool)    = 0;
    virtual void set_zone_threshold(Zone, uint8_t) = 0;

    // Coalesce window for combining near-simultaneous left + right cheek
    // touches into a BothCheeks event. Set to 0 to disable.
    virtual void set_coalesce_window_s(double /*seconds*/) {}

    // Fires once per touch-detected transition (not on release). Invoked from
    // the poll thread — callbacks must be thread-safe with whatever state
    // they touch.
    void on_boop(BoopCallback cb) { on_boop_ = std::move(cb); }

protected:
    BoopCallback on_boop_;
};

} // namespace sensor
