#pragma once
// ── neopixel_matrix_chain.h ───────────────────────────────────────────────────
// One daisy-chain of WS2812-based 8x8 LED matrices ("NeoPixel matrix",
// SK6812 variants, the Chinese 1088-with-WS2812-driver boards) acting as a
// pin-compatible drop-in for a MAX7219 face chain — same chip grid, same
// canvas regions, but full RGB per pixel instead of mono.
//
// Each chain owns its own spidev (no CS — WS2812 just listens to MOSI), so
// multi-chain RGB-matrix builds need either multiple hardware SPI buses or
// the kernel's spi-gpio overlay. Bit-banged userspace GPIO is *not*
// supported here: WS2812 needs ~400 ns timing, which the gpio_v2 ioctl
// path can't hit. If you only have one hardware SPI bus, daisy-chain all
// the matrices into one long chain on one spidev.

#include "panel_output.h"   // cv::Mat + namespace face

#include <cstdint>
#include <string>
#include <vector>

namespace face {

class NeoPixelMatrixChain {
public:
    // 8x8 module variants:
    //   AdafruitSerpentine — Adafruit NeoPixel matrix. Pixel 0 = top-left,
    //                        rows snake left-right-left-right.
    //   RowMajor           — Pixel 0 = top-left, every row left-to-right
    //                        with no zigzag (the bare matrix layout some
    //                        Chinese boards use).
    enum class PixelLayout : uint8_t { AdafruitSerpentine, RowMajor };
    // Order of chips along the daisy-chain across the chip grid — same
    // meaning as Max7219Chain::ChainOrder.
    enum class ChainOrder : uint8_t { RowMajor, Serpentine };
    // WS2812 byte order on the wire. WS2812 = GRB, SK6812 = RGB. SK6812
    // RGBW (a 4-byte variant) isn't supported in v1.
    enum class ColorOrder : uint8_t { GRB, RGB };

    struct Config {
        std::string name;
        std::string spi_device  = "/dev/spidev0.0";
        int         speed_hz    = 2'400'000;          // 3 SPI bits per WS2812 bit
        int         cols_chips  = 1;
        int         rows_chips  = 1;
        int         canvas_x    = 0;
        int         canvas_y    = 0;
        PixelLayout pixel_layout = PixelLayout::AdafruitSerpentine;
        ChainOrder  chain_order  = ChainOrder::Serpentine;
        ColorOrder  color_order  = ColorOrder::GRB;
        uint8_t     brightness   = 64;                // 0..255 software scale
        int         reset_bytes  = 64;                // tail zero bytes for ≥50 µs latch
    };

    explicit NeoPixelMatrixChain(Config cfg);
    ~NeoPixelMatrixChain();

    NeoPixelMatrixChain(const NeoPixelMatrixChain&)            = delete;
    NeoPixelMatrixChain& operator=(const NeoPixelMatrixChain&) = delete;

    bool open();
    void close();
    bool is_open() const { return fd_ >= 0; }

    // Crop the configured region from the canvas, walk every chip in the
    // configured layout / order, encode as SPI bits, and clock it out.
    void show(const cv::Mat& rgb_canvas);

    const std::string& name() const { return cfg_.name; }

private:
    // Map a (gc, gr) chip position in the grid to its index along the
    // daisy-chain (matches Max7219Chain's helper one-to-one).
    int chip_chain_index(int gc, int gr) const;
    // Map a (chip-local px, py) coordinate to the linear pixel index within
    // that chip (8x8 = 64 pixels), per pixel_layout.
    int pixel_in_chip(int px, int py) const;

    // Encode 8 bits of WS2812 data → 24 SPI bits = 3 SPI bytes (MSB first).
    void encode_color_byte(uint8_t b, uint8_t* out_3bytes) const;

    Config              cfg_;
    int                 fd_         = -1;
    int                 total_chips_ = 0;
    int                 total_pixels_ = 0;            // 64 × total_chips
    // SPI transmit buffer: 9 bytes per WS2812 pixel (3 channels × 3 SPI
    // bytes each) + reset_bytes of zeros at the tail.
    std::vector<uint8_t> spi_buf_;
};

} // namespace face
