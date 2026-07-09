#pragma once
// ── apds9960.h ────────────────────────────────────────────────────────────────
// APDS-9960 gesture + proximity sensor (I²C 0x39). Mounted near the snout it
// gives touchless input: swipe up/down/left/right in front of the face, plus
// a "something is close" proximity edge (a boop that sees it coming).
//
// Gestures are classified from the chip's gesture FIFO (U/D/L/R photodiode
// sets) with the standard first-vs-last delta method — deliberately simple;
// gesture_rotation remaps directions for whatever way the module is mounted.
// Callbacks fire on the poll thread; main marshals them onto the input queue
// like every other button source.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace sensor {

class Apds9960 {
public:
    enum class Gesture : uint8_t { Up = 0, Down, Left, Right };

    struct Config {
        bool        enabled          = false;
        std::string i2c_bus          = "/dev/i2c-1";
        int         i2c_addr         = 0x39;   // fixed on the APDS-9960
        float       poll_hz          = 40.0f;  // FIFO drains fast during a swipe
        int         near_threshold   = 60;     // PDATA 0..255; near-edge fire level
        int         gesture_rotation = 0;      // 0/90/180/270 - mounting remap
    };

    using GestureCallback   = std::function<void(Gesture)>;
    using ProximityCallback = std::function<void(bool near)>;

    explicit Apds9960(const Config& cfg) : cfg_(cfg) {}
    ~Apds9960() { stop(); }

    void set_gesture_callback(GestureCallback cb)     { gest_cb_ = std::move(cb); }
    void set_proximity_callback(ProximityCallback cb) { prox_cb_ = std::move(cb); }

    bool start();
    void stop();
    bool connected() const { return running_.load(); }

private:
    void poll_loop();
    bool init_chip();
    bool wr(uint8_t reg, uint8_t val);
    int  rd(uint8_t reg);                       // -1 on error
    int  rd_block(uint8_t reg, uint8_t* buf, int n);
    Gesture rotate(Gesture g) const;

    Config             cfg_;
    int                fd_ = -1;
    std::thread        thread_;
    std::atomic<bool>  running_{ false };
    GestureCallback    gest_cb_;
    ProximityCallback  prox_cb_;
};

}  // namespace sensor
