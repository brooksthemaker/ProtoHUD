#pragma once
// ── pico_pinmap.h ─────────────────────────────────────────────────────────────
// Static Raspberry Pi Pico 2 (RP2350) 40-pin description for the Coprocessor Pin
// visualizer/editor (System > GPIO Buttons > Button Coprocessor > Pins). Mirrors
// the official Pico pinout — physical pin, GPIO (GP) number, alt-function label,
// and a capability bitmask the editor uses to offer only functions a pin allows.
//
// Pure data + constexpr; no allocation. The RUNTIME role of each pin (button /
// LED / voice / MAX / free) is computed by the menu from the live config, not
// stored here.

#include <array>
#include <cstdint>
#include <imgui.h>

#include "gpio_pinmap.h"   // reuse PinKind + pin_kind_color for a shared palette

namespace sys {

// What a pin can be assigned to. Bitmask so the editor filters the pin picker.
enum PicoCap : uint8_t {
    CapNone    = 0,
    CapDigital = 1 << 0,   // button input / LED output (every GP)
    CapPwm     = 1 << 1,   // PWM (every GP on RP2350)
    CapAdc     = 1 << 2,   // analog in — GP26/27/28 only (mic for the voice changer)
    CapI2c     = 1 << 3,   // I²C-capable (DAC control)
    CapSpi     = 1 << 4,   // SPI-capable (MAX7219 bridge)
};

struct PicoPin {
    uint8_t     physical;   // 1..40
    int8_t      gp;         // GP number, or -1 for power / ground / RUN / VREF
    const char* label;      // "GP0" / "GND" / "3V3(OUT)" / "VBUS" …
    const char* alt;        // alt-function hint, or ""
    uint8_t     caps;       // OR of PicoCap (0 for non-GP pins)
    PinKind     kind;       // colour family (reuses the Pi visualizer palette)
};

// Standard Pico / Pico 2 physical layout: pin 1 top-left down to 20 bottom-left,
// 21 bottom-right up to 40 top-right. GP0-22 + GP26-28 are user pins; GP23-25 are
// used internally on the board and not broken out.
inline constexpr std::array<PicoPin, 40> kPico2Pins{{
    { 1,  0, "GP0",  "UART0 TX / I2C0 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 2,  1, "GP1",  "UART0 RX / I2C0 SCL", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 3, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    { 4,  2, "GP2",  "I2C1 SDA / SPI0 SCK", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 5,  3, "GP3",  "I2C1 SCL / SPI0 TX",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 6,  4, "GP4",  "UART1 TX / I2C0 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 7,  5, "GP5",  "UART1 RX / I2C0 SCL", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 8, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    { 9,  6, "GP6",  "I2C1 SDA / SPI0 RX",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {10,  7, "GP7",  "I2C1 SCL / SPI0 TX",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {11,  8, "GP8",  "SPI1 RX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {12,  9, "GP9",  "SPI1 CSn / I2C0 SCL", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {13, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    {14, 10, "GP10", "SPI1 SCK / I2C1 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {15, 11, "GP11", "SPI1 TX / I2C1 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {16, 12, "GP12", "SPI1 RX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {17, 13, "GP13", "SPI1 CSn / I2C0 SCL", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {18, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    {19, 14, "GP14", "SPI1 SCK / I2C1 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {20, 15, "GP15", "SPI1 TX / I2C1 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {21, 16, "GP16", "SPI0 RX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {22, 17, "GP17", "SPI0 CSn / I2C0 SCL", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {23, 18, "GP18", "SPI0 SCK / I2C1 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {24, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    {25, 19, "GP19", "SPI0 TX / I2C1 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {26, 20, "GP20", "I2C0 SDA",            CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {27, 21, "GP21", "I2C0 SCL",            CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {28, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    {29, 22, "GP22", "",                    CapDigital|CapPwm,               PinKind::Gpio },
    {30, -1, "RUN",  "reset",               CapNone,                         PinKind::HatId },
    {31, 26, "GP26", "ADC0 / I2C1 SDA",     CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm  },
    {32, 27, "GP27", "ADC1 / I2C1 SCL",     CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm  },
    {33, -1, "AGND", "",                    CapNone,                         PinKind::Ground },
    {34, 28, "GP28", "ADC2",                CapDigital|CapPwm|CapAdc,        PinKind::Pwm  },
    {35, -1, "ADC_VREF","",                 CapNone,                         PinKind::Power3V3 },
    {36, -1, "3V3(OUT)","",                 CapNone,                         PinKind::Power3V3 },
    {37, -1, "3V3_EN", "",                  CapNone,                         PinKind::Power3V3 },
    {38, -1, "GND",  "",                    CapNone,                         PinKind::Ground },
    {39, -1, "VSYS", "1.8-5.5V in",         CapNone,                         PinKind::Power5V },
    {40, -1, "VBUS", "5V USB",              CapNone,                         PinKind::Power5V },
}};

// Look up a GP number's entry (nullptr if not a broken-out GP).
inline const PicoPin* pico_pin_for_gp(int gp) {
    for (const auto& p : kPico2Pins) if (p.gp == gp) return &p;
    return nullptr;
}

}  // namespace sys
