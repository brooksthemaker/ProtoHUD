#pragma once
#include <Adafruit_seesaw.h>
#include <cstdint>
#include <functional>

// Adafruit ANO Rotary Navigation Encoder via Stemma QT / I2C seesaw.
// Encoder seesaw chip: ATSAMD09, I2C address 0x49.
// INT pin is active-low — asserted when any button or encoder changes.
//
// 5 directional buttons (seesaw GPIO) + rotary encoder (24 detents / rev).
// Call begin() from setup(), then poll() from the encoder task.

enum class EncoderEvent : uint8_t {
    ROTATE_CW,       // clockwise  (+1 detent)
    ROTATE_CCW,      // counter-CW (-1 detent)
    BTN_SELECT,      // centre press
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_SELECT_LONG, // centre held >700 ms
};

// ANO encoder seesaw GPIO pin mapping.
static constexpr uint8_t ANO_BTN_SELECT = 1;
static constexpr uint8_t ANO_BTN_UP     = 2;
static constexpr uint8_t ANO_BTN_DOWN   = 3;
static constexpr uint8_t ANO_BTN_LEFT   = 4;
static constexpr uint8_t ANO_BTN_RIGHT  = 5;

class AnoEncoder {
public:
    // int_pin: Pico GPIO connected to the ANO INT line.
    bool begin(int int_pin);

    // Call regularly from Core 0 (after INT fires or on timer).
    // Returns true if any event was posted.
    bool poll();

    // Callbacks — set before begin() or any time.
    std::function<void(int delta)> on_rotate;  // delta: +1 or -1
    std::function<void()> on_select;
    std::function<void()> on_select_long;
    std::function<void()> on_up;
    std::function<void()> on_down;
    std::function<void()> on_left;
    std::function<void()> on_right;

private:
    void dispatch_button(uint8_t pin, bool pressed, uint32_t held_ms);

    Adafruit_seesaw ss_;
    int             int_pin_     = -1;
    int32_t         last_pos_    = 0;
    uint32_t        select_down_ = 0;  // millis() when select was pressed
    uint8_t         last_btns_   = 0xFF;
    bool            select_held_ = false;
    static constexpr uint32_t LONG_PRESS_MS = 700;
};
