#include "encoder.h"
#include <Wire.h>
#include <Arduino.h>

static constexpr uint32_t ANO_BTN_MASK =
    (1u << ANO_BTN_SELECT) | (1u << ANO_BTN_UP) | (1u << ANO_BTN_DOWN) |
    (1u << ANO_BTN_LEFT)   | (1u << ANO_BTN_RIGHT);

bool AnoEncoder::begin(int int_pin) {
    int_pin_ = int_pin;
    pinMode(int_pin, INPUT_PULLUP);

    if (!ss_.begin(ANO_I2C_ADDR)) return false;

    // Enable pull-ups on all button pins; seesaw handles this internally.
    ss_.pinModeBulk(ANO_BTN_MASK, INPUT_PULLUP);
    ss_.setGPIOInterrupts(ANO_BTN_MASK, true);
    ss_.enableEncoderInterrupt();

    last_pos_  = ss_.getEncoderPosition();
    last_btns_ = static_cast<uint8_t>(ss_.digitalReadBulk(ANO_BTN_MASK) & 0xFF);
    return true;
}

bool AnoEncoder::poll() {
    if (digitalRead(int_pin_) == HIGH) {
        // No interrupt pending — check for long-press timeout only.
        if (select_held_) {
            if ((millis() - select_down_) >= LONG_PRESS_MS) {
                select_held_ = false;
                if (on_select_long) on_select_long();
                return true;
            }
        }
        return false;
    }

    bool fired = false;

    // Read encoder position delta.
    const int32_t new_pos = ss_.getEncoderPosition();
    const int32_t delta   = new_pos - last_pos_;
    if (delta != 0) {
        last_pos_ = new_pos;
        const int steps = delta > 0 ? delta : -delta;
        for (int i = 0; i < steps; ++i) {
            if (on_rotate) on_rotate(delta > 0 ? 1 : -1);
        }
        fired = true;
    }

    // Read button states (active low, pull-up).
    const uint8_t btns = static_cast<uint8_t>(
        ss_.digitalReadBulk(ANO_BTN_MASK) & 0xFF);
    const uint8_t changed = last_btns_ ^ btns;
    last_btns_ = btns;

    if (changed) {
        const uint32_t now = millis();

        auto check = [&](uint8_t pin, auto& cb) {
            if (!(changed & (1u << pin))) return;
            const bool pressed = !(btns & (1u << pin));
            if (pressed) {
                if (pin == ANO_BTN_SELECT) {
                    select_down_ = now;
                    select_held_ = true;
                }
            } else {
                if (pin == ANO_BTN_SELECT && select_held_) {
                    select_held_ = false;
                    const uint32_t held = now - select_down_;
                    if (held < LONG_PRESS_MS && cb) cb();
                } else if (cb) {
                    cb();
                }
            }
        };

        check(ANO_BTN_SELECT, on_select);
        check(ANO_BTN_UP,     on_up);
        check(ANO_BTN_DOWN,   on_down);
        check(ANO_BTN_LEFT,   on_left);
        check(ANO_BTN_RIGHT,  on_right);
        fired = true;
    }

    // Long-press check after reading buttons.
    if (select_held_ && (millis() - select_down_) >= LONG_PRESS_MS) {
        select_held_ = false;
        if (on_select_long) on_select_long();
        fired = true;
    }

    return fired;
}
