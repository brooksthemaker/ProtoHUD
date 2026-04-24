#pragma once
#include "serial_port.h"
#include "../app_state.h"
#include <functional>
#include <string>
#include <vector>

// Callback fired on knob position change (direction: -1/+1, detent index)
using KnobMoveCallback  = std::function<void(int8_t direction, int detent_index)>;
using KnobWakeCallback  = std::function<void()>;
// Callback fired on status messages from SmartKnob (calibration ready, sleep/wake)
using KnobStatusCallback = std::function<void(uint8_t status, uint8_t param)>;

class SmartKnob {
public:
    SmartKnob(const std::string& port, int baud, AppState& state);

    bool start();
    void stop();
    bool connected() const;

    // Configure haptic detents. Positions are evenly distributed if positions
    // is empty; otherwise explicit angles in millidegrees are used.
    void set_detents(int count, const std::vector<int16_t>& positions = {});
    void wake();
    void set_sleep_timeout(uint16_t seconds);
    void set_haptic(uint8_t amplitude, uint8_t frequency, uint8_t detent_strength);

    void on_move(KnobMoveCallback cb)   { move_cb_   = std::move(cb); }
    void on_wake(KnobWakeCallback cb)   { wake_cb_   = std::move(cb); }
    void on_status(KnobStatusCallback cb) { status_cb_ = std::move(cb); }

private:
    void on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len);

    SerialPort      port_;
    AppState&       state_;
    KnobMoveCallback move_cb_;
    KnobWakeCallback wake_cb_;
    KnobStatusCallback status_cb_;
};
