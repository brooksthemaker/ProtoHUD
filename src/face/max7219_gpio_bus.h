#pragma once
// ── max7219_gpio_bus.h ────────────────────────────────────────────────────────
// Shared DIN + CLK bit-banged bus for MAX7219 chains that can't claim a
// hardware SPI bus (because HUB75 owns SPI1 and WS2812 owns SPI0). Owned by
// Max7219PanelOutput; each Max7219Chain references it for byte shifts and
// claims its own CS line separately.
//
// MAX7219 latches DIN on the rising edge of CLK, so the protocol per bit is:
//   1. Set DIN to the bit value while CLK is low.
//   2. Raise CLK high (latches).
// At the end of a byte CLK is left low so the next byte can start fresh.

#include "gpio_v2.h"

#include <cstdint>
#include <string>

namespace face {

class Max7219GpioBus {
public:
    bool open(const std::string& chip_dev, int din_offset, int clk_offset) {
        const uint32_t offsets[2] = {
            static_cast<uint32_t>(din_offset),
            static_cast<uint32_t>(clk_offset),
        };
        if (!lines_.open(chip_dev, offsets, 2, "max7219-bus")) return false;
        // Idle state: both low.
        lines_.set_values(0, 0b11);
        return true;
    }

    void close() { lines_.close(); }
    bool is_open() const { return lines_.is_open(); }

    // Shift one byte MSB-first. CLK ends low after the last bit.
    void shift_byte(uint8_t b) {
        for (int i = 7; i >= 0; --i) {
            const uint64_t din = ((b >> i) & 1u) ? 1ull : 0ull;
            // DIN = bit, CLK low — setup time for the rising edge.
            lines_.set_values(din /* bit 0 */, 0b11);
            // CLK high (rising edge latches DIN into MAX7219 shift register).
            lines_.set_values(din | 0b10, 0b11);
        }
        // Leave CLK low for the inter-byte idle.
        lines_.set_values(0, 0b10);
    }

private:
    GpioOutputGroup lines_;   // [0]=DIN, [1]=CLK
};

} // namespace face
