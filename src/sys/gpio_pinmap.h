#pragma once
// ── gpio_pinmap.h ─────────────────────────────────────────────────────────────
// Static Pi 5 / CM5 40-pin GPIO header description used by the GPIO Visualizer
// in the System menu. Each entry mirrors the Raspberry Pi Foundation's official
// pinout chart — physical pin number, BCM line, primary/secondary/tertiary
// function labels, and a coarse "kind" tag the visualizer maps to a colour.
//
// Pure data + constexpr lookups; no runtime allocation. Designed so a colour
// scheme tweak or a new function label is a one-line edit.

#include <array>
#include <cstdint>
#include <string>
#include <imgui.h>

namespace sys {

enum class PinKind : uint8_t {
    Power3V3,    // 3.3V power rail
    Power5V,     // 5V power rail
    Ground,
    Gpio,        // general purpose GPIO with no special primary function
    I2c,         // I²C SDA/SCL
    Spi,         // SPI MISO/MOSI/SCLK/CE
    Uart,        // UART TXD/RXD
    Pwm,         // hardware PWM-capable pin
    Pcm,         // PCM audio (DOUT/DIN/FS/CLK)
    Gpclk,       // GPCLK0/1/2
    HatId,       // ID_SD / ID_SC reserved for HAT EEPROM
};

struct GpioPin {
    uint8_t     physical;    // 1..40 along the header
    int8_t      bcm;         // -1 for power / ground / ID pins
    const char* primary;     // "GPIO 18" or "5V" etc.
    const char* secondary;   // alt function name, or ""
    const char* tertiary;    // alt function name, or ""
    PinKind     kind;
};

// 40-pin header. Order = physical pin order (odd pins on left column,
// even on right when laid out vertically with pin 1 at top).
// Functions follow https://pinout.xyz / Raspberry Pi Foundation conventions.
inline constexpr std::array<GpioPin, 40> kPi40Pins{{
    { 1,  -1, "3.3V",    "",       "",       PinKind::Power3V3 },
    { 2,  -1, "5V",      "",       "",       PinKind::Power5V  },
    { 3,   2, "GPIO 2",  "SDA1",   "",       PinKind::I2c      },
    { 4,  -1, "5V",      "",       "",       PinKind::Power5V  },
    { 5,   3, "GPIO 3",  "SCL1",   "",       PinKind::I2c      },
    { 6,  -1, "GND",     "",       "",       PinKind::Ground   },
    { 7,   4, "GPIO 4",  "GPCLK0", "1WIRE",  PinKind::Gpclk    },
    { 8,  14, "GPIO 14", "TXD0",   "",       PinKind::Uart     },
    { 9,  -1, "GND",     "",       "",       PinKind::Ground   },
    {10,  15, "GPIO 15", "RXD0",   "",       PinKind::Uart     },
    {11,  17, "GPIO 17", "CE1",    "SPI1",   PinKind::Gpio     },
    {12,  18, "GPIO 18", "PCM_CLK","PWM0",   PinKind::Pwm      },
    {13,  27, "GPIO 27", "",       "",       PinKind::Gpio     },
    {14,  -1, "GND",     "",       "",       PinKind::Ground   },
    {15,  22, "GPIO 22", "",       "",       PinKind::Gpio     },
    {16,  23, "GPIO 23", "",       "",       PinKind::Gpio     },
    {17,  -1, "3.3V",    "",       "",       PinKind::Power3V3 },
    {18,  24, "GPIO 24", "",       "",       PinKind::Gpio     },
    {19,  10, "GPIO 10", "MOSI",   "SPI0",   PinKind::Spi      },
    {20,  -1, "GND",     "",       "",       PinKind::Ground   },
    {21,   9, "GPIO 9",  "MISO",   "SPI0",   PinKind::Spi      },
    {22,  25, "GPIO 25", "",       "",       PinKind::Gpio     },
    {23,  11, "GPIO 11", "SCLK",   "SPI0",   PinKind::Spi      },
    {24,   8, "GPIO 8",  "CE0",    "SPI0",   PinKind::Spi      },
    {25,  -1, "GND",     "",       "",       PinKind::Ground   },
    {26,   7, "GPIO 7",  "CE1",    "SPI0",   PinKind::Spi      },
    {27,  -1, "ID_SD",   "EEPROM", "",       PinKind::HatId    },
    {28,  -1, "ID_SC",   "EEPROM", "",       PinKind::HatId    },
    {29,   5, "GPIO 5",  "",       "",       PinKind::Gpio     },
    {30,  -1, "GND",     "",       "",       PinKind::Ground   },
    {31,   6, "GPIO 6",  "",       "",       PinKind::Gpio     },
    {32,  12, "GPIO 12", "PWM0",   "",       PinKind::Pwm      },
    {33,  13, "GPIO 13", "PWM1",   "",       PinKind::Pwm      },
    {34,  -1, "GND",     "",       "",       PinKind::Ground   },
    {35,  19, "GPIO 19", "MISO",   "SPI1",   PinKind::Spi      },
    {36,  16, "GPIO 16", "CE2",    "SPI1",   PinKind::Spi      },
    {37,  26, "GPIO 26", "",       "",       PinKind::Gpio     },
    {38,  20, "GPIO 20", "MOSI",   "SPI1",   PinKind::Spi      },
    {39,  -1, "GND",     "",       "",       PinKind::Ground   },
    {40,  21, "GPIO 21", "SCLK",   "SPI1",   PinKind::Spi      },
}};

// Colour family matched to the Raspberry Pi / pinout.xyz header chart.
inline ImU32 pin_kind_color(PinKind k) {
    switch (k) {
    case PinKind::Power3V3: return IM_COL32(255, 160,  70, 255);   // orange  (3V3)
    case PinKind::Power5V:  return IM_COL32(235,  45,  40, 255);   // red     (5V)
    case PinKind::Ground:   return IM_COL32( 40,  42,  46, 255);   // black   (GND)
    case PinKind::Gpio:     return IM_COL32( 60, 180,  75, 255);   // green   (GPIO)
    case PinKind::I2c:      return IM_COL32( 45, 140, 225, 255);   // blue    (I²C)
    case PinKind::Spi:      return IM_COL32(230, 100, 180, 255);   // pink    (SPI)
    case PinKind::Uart:     return IM_COL32(150, 100, 210, 255);   // purple  (UART)
    case PinKind::Pwm:      return IM_COL32( 90, 190, 170, 255);   // teal-green (PWM)
    case PinKind::Pcm:      return IM_COL32( 80, 195, 200, 255);   // teal    (PCM)
    case PinKind::Gpclk:    return IM_COL32(240, 200,  70, 255);   // yellow  (GPCLK)
    case PinKind::HatId:    return IM_COL32(150, 120, 100, 255);   // brown   (ID EEPROM)
    }
    return IM_COL32(120, 120, 120, 255);
}

inline const char* pin_kind_short(PinKind k) {
    switch (k) {
    case PinKind::Power3V3: return "3.3V";
    case PinKind::Power5V:  return "5V";
    case PinKind::Ground:   return "GND";
    case PinKind::Gpio:     return "GPIO";
    case PinKind::I2c:      return "I2C";
    case PinKind::Spi:      return "SPI";
    case PinKind::Uart:     return "UART";
    case PinKind::Pwm:      return "PWM";
    case PinKind::Pcm:      return "PCM";
    case PinKind::Gpclk:    return "CLK";
    case PinKind::HatId:    return "ID";
    }
    return "";
}

} // namespace sys
