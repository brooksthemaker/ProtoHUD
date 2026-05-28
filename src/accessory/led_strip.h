#pragma once
// ── led_strip.h ───────────────────────────────────────────────────────────────
// WS2812 / SK6812 driver speaking to the LEDs through the Pi's hardware SPI
// MOSI line (GPIO 10 / spidev0.0). Each WS2812 bit is encoded as three SPI
// bits clocked at ~2.4 MHz (one 417 ns SPI bit), so:
//
//   WS2812 "0"  →  SPI bits 100  (≈417 ns high, ≈833 ns low)
//   WS2812 "1"  →  SPI bits 110  (≈833 ns high, ≈417 ns low)
//
// All accessory LEDs (cheekhubs + fins) sit on one daisy-chain. Zone slicing
// (which LED indices belong to which physical group) lives in AccessoryLeds;
// this class just owns the buffer + the SPI transport.

#include <cstdint>
#include <string>
#include <vector>

namespace accessory {

class LedStrip {
public:
    enum class ColorOrder : uint8_t { GRB, RGB, BGR };

    struct Config {
        std::string spi_device  = "/dev/spidev0.0";
        int         speed_hz    = 2'400'000;           // 3 SPI bits per WS2812 bit
        int         count       = 0;                   // total LEDs in the chain
        ColorOrder  color_order = ColorOrder::GRB;     // WS2812 default; SK6812 = RGB
        // Reset / latch low period at end of frame. WS2812 datasheet says
        // ≥50 µs; we send 64 bytes of zeros (~213 µs at 2.4 MHz) to be safe.
        int         reset_bytes = 64;
    };

    explicit LedStrip(Config cfg);
    ~LedStrip();

    LedStrip(const LedStrip&)            = delete;
    LedStrip& operator=(const LedStrip&) = delete;

    // Opens spidev and configures mode 0 / 8-bit / cfg.speed_hz. Returns false
    // on failure (kernel module missing, permission denied, etc.). is_open()
    // remains false in that case so callers can drop the feature silently
    // without crashing the rest of the HUD.
    bool open();
    void close();
    bool is_open() const { return fd_ >= 0; }

    int  count() const { return cfg_.count; }

    // Per-pixel logical color (0..255, gamma-uncorrected). Caller indexes
    // into the daisy-chain; no zone awareness here.
    void set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
    void fill     (uint8_t r, uint8_t g, uint8_t b);

    // Master brightness applied at show() time (0..255). Doesn't mutate the
    // stored pixel values — set_global_brightness(0) keeps the next non-zero
    // value's behaviour identical to before.
    void set_global_brightness(uint8_t b) { brightness_ = b; }
    uint8_t global_brightness() const     { return brightness_; }

    // Encode the current pixel buffer and push it via SPI. Safe to call from
    // any single thread; not internally locked. Returns false if the write
    // failed (errno preserved).
    bool show();

private:
    void encode_color_byte(uint8_t b, uint8_t* out_3bytes) const;

    Config              cfg_;
    int                 fd_         = -1;
    uint8_t             brightness_ = 255;

    // Logical pixel store, 3 bytes per LED in the channel order dictated by
    // color_order (so set_pixel knows where to write).
    std::vector<uint8_t> pixels_;
    // SPI transmit buffer: 9 bytes per LED (3 SPI bytes per color byte × 3
    // color bytes) + reset_bytes of zeros at the tail.
    std::vector<uint8_t> spi_buf_;
};

} // namespace accessory
