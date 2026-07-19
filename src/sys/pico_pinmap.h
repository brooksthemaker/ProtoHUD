#pragma once
// ── pico_pinmap.h ─────────────────────────────────────────────────────────────
// Static Raspberry Pi Pico 2 (RP2350) 40-pin description for the Coprocessor Pin
// visualizer/editor (GPIO > RP2350 GPIO Expander > Pins). Mirrors
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
    {29, 22, "GP22", "I2C1 SDA",            CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {30, -1, "RUN",  "reset",               CapNone,                         PinKind::HatId },
    {31, 26, "GP26", "ADC0 / I2C1 SDA",     CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm  },
    {32, 27, "GP27", "ADC1 / I2C1 SCL",     CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm  },
    {33, -1, "AGND", "",                    CapNone,                         PinKind::Ground },
    {34, 28, "GP28", "ADC2 / I2C0 SDA",     CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm  },
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

// Pimoroni Pico LiPo 2 XL W physical layout: two rows of 30, pin 1 top-left
// down to 30 bottom-left, 31 bottom-right up to 60 top-right (USB-C up) —
// transcribed from Pimoroni's pinout diagram. Header pins GP26-28 double as
// A0-A2 through internal 1 kΩ links to the RP2350B's real ADC pins GP40-42
// (which is why the firmware's mic ADC lives on GP40 while the wire lands on
// the pin marked 26/A0). GP23-25/29 belong to the RM2 wireless module and
// GP40-42 sit behind those 1 kΩ links, so none of them reach a header.
inline constexpr std::array<PicoPin, 60> kPicoLipo2XlWPins{{
    { 1,  0, "GP0",  "UART0 TX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 2,  1, "GP1",  "UART0 RX / I2C0 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 3, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    { 4,  2, "GP2",  "I2C1 SDA / SPI0 SCK",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 5,  3, "GP3",  "I2C1 SCL / SPI0 TX",   CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 6,  4, "GP4",  "UART1 TX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 7,  5, "GP5",  "UART1 RX / I2C0 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    { 8, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    { 9,  6, "GP6",  "I2C1 SDA / SPI0 SCK",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {10,  7, "GP7",  "I2C1 SCL / SPI0 TX",   CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {11,  8, "GP8",  "UART1 TX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {12,  9, "GP9",  "UART1 RX / I2C0 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {13, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {14, 10, "GP10", "SPI1 SCK / I2C1 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {15, 11, "GP11", "SPI1 TX / I2C1 SCL",   CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {16, 12, "GP12", "HSTX / SPI1 RX",       CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {17, 13, "GP13", "HSTX / SPI1 CSn",      CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {18, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {19, 14, "GP14", "HSTX / SPI1 SCK",      CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {20, 15, "GP15", "HSTX / SPI1 TX",       CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {21, -1, "3V3",  "out",                  CapNone,                         PinKind::Power3V3 },
    {22, 31, "GP31", "UART0 RX / I2C1 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {23, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {24, 32, "GP32", "SP/CE TX / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {25, 33, "GP33", "SP/CE RX / I2C0 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {26, 34, "GP34", "SP/CE SCK / I2C1 SDA", CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {27, 35, "GP35", "SP/CE MOSI / I2C1 SCL",CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {28, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {29, 36, "GP36", "SP/CE BL / I2C0 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {30, 37, "GP37", "I2C0 SCL",             CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {31, 38, "GP38", "I2C1 SDA",             CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {32, 39, "GP39", "I2C1 SCL",             CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {33, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {34, 43, "GP43", "A3 / VBAT sense (BtS Cut)", CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm },
    {35, 44, "GP44", "A4 / I2C0 SDA",        CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm },
    {36, 45, "GP45", "A5 / I2C0 SCL",        CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm },
    {37, 46, "GP46", "A6 / I2C1 SDA",        CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm },
    {38, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {39, 47, "GP47", "A7 / PSRAM CS (PSRAM Cut)", CapDigital|CapPwm|CapAdc|CapI2c, PinKind::Pwm },
    {40, 30, "GP30", "BOOT / user button",   CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {41, 16, "GP16", "SPI0 RX / I2C0 SDA",   CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {42, 17, "GP17", "SPI0 CSn / I2C0 SCL",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {43, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {44, 18, "GP18", "SPI0 SCK / I2C1 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {45, 19, "GP19", "SPI0 TX / I2C1 SCL",   CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {46, 20, "GP20", "I2C0 SDA",             CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {47, 21, "GP21", "I2C0 SCL",             CapDigital|CapPwm|CapI2c,        PinKind::Gpio },
    {48, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {49, 22, "GP22", "SPI0 SCK / I2C1 SDA",  CapDigital|CapPwm|CapI2c|CapSpi, PinKind::Gpio },
    {50, -1, "RUN",  "reset",                CapNone,                         PinKind::HatId },
    {51, 26, "GP26", "A0 via 1k\xE2\x86\x92GP40 / I2C1 SDA", CapDigital|CapPwm|CapI2c, PinKind::Pwm },
    {52, 27, "GP27", "A1 via 1k\xE2\x86\x92GP41 / I2C1 SCL", CapDigital|CapPwm|CapI2c, PinKind::Pwm },
    {53, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {54, 28, "GP28", "A2 via 1k\xE2\x86\x92GP42 / I2C0 SDA", CapDigital|CapPwm|CapI2c, PinKind::Pwm },
    {55, -1, "ADC_VREF","",                  CapNone,                         PinKind::Power3V3 },
    {56, -1, "3V3",  "out",                  CapNone,                         PinKind::Power3V3 },
    {57, -1, "3V3_EN","",                    CapNone,                         PinKind::Power3V3 },
    {58, -1, "GND",  "",                     CapNone,                         PinKind::Ground },
    {59, -1, "VSYS", "1.8-5.5V in",          CapNone,                         PinKind::Power5V },
    {60, -1, "VBUS", "5V USB",               CapNone,                         PinKind::Power5V },
}};

// ── Coprocessor board variants ────────────────────────────────────────────────
// The pin visualizer/editor adapts to the board the RP2350 coprocessor runs on.
// RP2350A (Pico 2) exposes GP0-29 with ADC on GP26-28 and gets its real 40-pin
// header; the Pico LiPo 2 XL W gets its real 60-pin header (kPicoLipo2XlWPins);
// Pico Plus 2 gets a logical GP grid. "Raw" is a board-agnostic GP0-47 view.
enum class PicoVariant : uint8_t { Rp2350a = 0, PicoPlus2, PicoLipo2XlW, Raw };

inline const char* pico_variant_name(PicoVariant v) {
    switch (v) {
        case PicoVariant::Rp2350a:      return "RP2350 (Pico 2)";
        case PicoVariant::PicoPlus2:    return "Pimoroni Pico Plus 2";
        case PicoVariant::PicoLipo2XlW: return "Pimoroni Pico LiPo 2 XL W";
        case PicoVariant::Raw:          return "Raw GPIO (GP0-47)";
    }
    return "?";
}
inline const char* pico_variant_id(PicoVariant v) {
    switch (v) {
        case PicoVariant::PicoPlus2:    return "pico_plus_2";
        case PicoVariant::PicoLipo2XlW: return "pico_lipo2_xl_w";
        case PicoVariant::Raw:          return "raw";
        default:                        return "rp2350a";
    }
}
inline PicoVariant pico_variant_from_id(const std::string& s) {
    if (s == "pico_plus_2")     return PicoVariant::PicoPlus2;
    if (s == "pico_lipo2_xl_w") return PicoVariant::PicoLipo2XlW;
    if (s == "raw")             return PicoVariant::Raw;
    return PicoVariant::Rp2350a;
}

// Highest GP number the variant exposes (RP2350A = 29, RP2350B boards + raw = 47).
inline int pico_variant_max_gp(PicoVariant v) {
    return v == PicoVariant::Rp2350a ? 29 : 47;
}
// True if the variant actually breaks this GP out to a header the user can wire.
// The Pico 2 keeps GP23-25 and GP29 internal (regulator / LED / VSYS sense); the
// Pico LiPo 2 XL W gives GP23-25/29 to the RM2 wireless module and hides GP40-42
// behind the internal 1 kΩ ADC links from header pins GP26-28. None of those may
// ever be offered as assignable pins. Pico Plus 2 and the raw view expose the
// full GP0-47 range.
inline bool pico_variant_gp_exists(PicoVariant v, int gp) {
    if (gp < 0 || gp > pico_variant_max_gp(v)) return false;
    if (v == PicoVariant::Rp2350a) return pico_pin_for_gp(gp) != nullptr;
    if (v == PicoVariant::PicoLipo2XlW) {
        if (gp == 23 || gp == 24 || gp == 25 || gp == 29) return false;  // RM2
        if (gp >= 40 && gp <= 42) return false;   // internal ADC (1k from GP26-28)
    }
    return true;
}
// ADC-capable GP for the variant (RP2350A: GP26-28; RP2350B: GP40-47).
inline bool pico_gp_is_adc(PicoVariant v, int gp) {
    return v == PicoVariant::Rp2350a ? (gp >= 26 && gp <= 28)
                                     : (gp >= 40 && gp <= 47);
}
// Board-reserved GP → onboard-peripheral label, or nullptr if free. These are
// informational (the editor flags them); several are cuttable on the board.
inline const char* pico_gp_reserved(PicoVariant v, int gp) {
    switch (v) {
        case PicoVariant::Rp2350a:
            if (gp == 25) return "LED";
            break;
        case PicoVariant::PicoPlus2:
            if (gp == 25) return "LED";
            if (gp == 45) return "BOOT";
            break;
        case PicoVariant::PicoLipo2XlW:
            if (gp == 30) return "BOOT";
            if (gp == 43) return "VBAT";     // battery sense (cuttable)
            if (gp == 47) return "PSRAM";    // (cuttable)
            break;
        case PicoVariant::Raw:
            break;
    }
    return nullptr;
}

// ── I²C mux ───────────────────────────────────────────────────────────────────
// The RP2350 (like the RP2040) has a FIXED I²C pin mux: every GP can do I²C, and
// which controller + SDA/SCL role is set by GP % 4 — uniform across GP0-47.
//   %4==0 → I2C0 SDA   %4==1 → I2C0 SCL   %4==2 → I2C1 SDA   %4==3 → I2C1 SCL
inline int  pico_gp_i2c_instance(int gp) { return (gp & 2) ? 1 : 0; }   // 0 or 1
inline bool pico_gp_i2c_is_sda(int gp)   { return (gp & 1) == 0; }
inline const char* pico_gp_i2c(int gp) {
    if (gp < 0) return "";
    switch (gp & 3) {
        case 0:  return "I2C0 SDA";
        case 1:  return "I2C0 SCL";
        case 2:  return "I2C1 SDA";
        default: return "I2C1 SCL";
    }
}
// True if (sda_gp, scl_gp) is a usable bus: same controller, SDA on one, SCL on
// the other, and two distinct pins.
inline bool pico_i2c_pair_ok(int sda_gp, int scl_gp) {
    return sda_gp >= 0 && scl_gp >= 0 && sda_gp != scl_gp &&
           pico_gp_i2c_instance(sda_gp) == pico_gp_i2c_instance(scl_gp) &&
           pico_gp_i2c_is_sda(sda_gp) && !pico_gp_i2c_is_sda(scl_gp);
}

}  // namespace sys
