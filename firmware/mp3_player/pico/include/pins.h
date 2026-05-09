#pragma once

// ── SPI0 (shared: ILI9341 display + SD card) ─────────────────────────────────
static constexpr int PIN_SPI_MOSI  = 19;
static constexpr int PIN_SPI_SCK   = 18;
static constexpr int PIN_SPI_MISO  = 16;
static constexpr int PIN_TFT_CS    = 17;
static constexpr int PIN_TFT_DC    = 20;
static constexpr int PIN_TFT_RST   = 21;
static constexpr int PIN_SD_CS     = 22;

// ── I2C0 (ANO Rotary Navigation Encoder via Stemma QT) ───────────────────────
static constexpr int PIN_I2C_SDA   =  4;
static constexpr int PIN_I2C_SCL   =  5;
static constexpr int PIN_ANO_INT   =  6;  // active-low interrupt
static constexpr int ANO_I2C_ADDR  = 0x49;

// ── UART0 (ESP32 bridge, 2 Mbps) ─────────────────────────────────────────────
static constexpr int PIN_UART_TX   =  0;  // Pico TX → ESP32 RX (GPIO3)
static constexpr int PIN_UART_RX   =  1;  // Pico RX ← ESP32 TX (GPIO1)

// ── Miscellaneous ─────────────────────────────────────────────────────────────
// Pico 2 W built-in LED is on the CYW43 GPIO, not a direct pin.
// Use the Arduino-Pico LED_BUILTIN define (maps to CYW43_WL_GPIO_LED_PIN).
