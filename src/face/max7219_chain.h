#pragma once
// ── max7219_chain.h ───────────────────────────────────────────────────────────
// One daisy-chain of MAX7219 8×8 matrix drivers, addressed over Linux spidev.
// A chain forms a `cols_chips × rows_chips` 2D grid covering an
// (cols_chips * 8) × (rows_chips * 8) pixel region of the renderer's canvas
// at (canvas_x, canvas_y). Multiple chains can run in parallel — one per
// functional zone of the face (left eye / right eye / nose / mouth) — each
// with its own SPI bus + CS line. Max7219PanelOutput stitches them together.
//
// Wiring variants supported in v1:
//   ModuleType::FC16        — most common off-the-shelf 4-in-1 / 8-in-1 board.
//                             8-bit row = canvas row, MSB = leftmost pixel.
//   ModuleType::Generic1088 — bare 1088AS modules, rows/cols swapped. Each
//                             chip's pixel data is transposed before packing.
//
// Daisy-chain pixel→chip order:
//   RowMajor    — chip 0 = top-left, scan right then drop to next row left.
//   Serpentine  — like RowMajor but every other row is reversed (most PCB
//                 layouts; saves the long return wire on each row).

#include "panel_output.h"   // for the cv::Mat include + namespace face
#include "gpio_v2.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace face {

class Max7219GpioBus;   // shared DIN+CLK bus when transport is "gpio"

class Max7219Chain {
public:
    enum class ModuleType : uint8_t { FC16, Generic1088 };
    enum class ChainOrder : uint8_t { RowMajor, Serpentine };
    // SPI transport — kernel spidev (default; clean, fast) or bit-banged GPIO
    // (slower, more pins, but available when HUB75 owns SPI1 and WS2812 owns
    // SPI0). GPIO transport shares DIN+CLK across all chains via a single
    // Max7219GpioBus instance owned by Max7219PanelOutput; the chain itself
    // owns its CS line.
    enum class Transport : uint8_t { Spidev, Gpio };

    struct Config {
        std::string name;              // "eye_l" / "eye_r" / "nose" / "mouth" …
        Transport   transport   = Transport::Spidev;

        // Spidev transport
        std::string spi_device  = "/dev/spidev0.1";
        int         speed_hz    = 1'000'000;  // datasheet max ≈ 10 MHz

        // GPIO transport — chip device + per-chain CS pin only; DIN/CLK come
        // from the shared bus configured at the Max7219PanelOutput level.
        std::string gpio_chip   = "/dev/gpiochip0";
        int         gpio_cs_pin = -1;          // BCM pin number; -1 disables

        int         cols_chips  = 1;          // chips wide
        int         rows_chips  = 1;          // chips tall
        // Rectangular region of the renderer's RGB canvas this chain
        // mirrors when module_positions is empty: (cols_chips*8) ×
        // (rows_chips*8) pixels starting at (canvas_x, canvas_y).
        int         canvas_x    = 0;
        int         canvas_y    = 0;
        // Optional per-module canvas positions in daisy order from DIN
        // (each entry is the {x, y} top-left of an 8×8 module). When
        // non-empty this overrides cols_chips / rows_chips / canvas_x /
        // canvas_y / chain_order — each module can sit anywhere on the
        // canvas, so a single chain can cover non-rectangular face
        // regions (e.g. one chain per side carrying eye + nose share +
        // mouth half).
        std::vector<std::array<int, 2>> module_positions;
        ModuleType  module_type = ModuleType::FC16;
        ChainOrder  chain_order = ChainOrder::Serpentine;
        uint8_t     intensity   = 4;          // 0..15
        uint8_t     threshold   = 80;         // luminance threshold for mono
    };

    explicit Max7219Chain(Config cfg);
    ~Max7219Chain();

    Max7219Chain(const Max7219Chain&)            = delete;
    Max7219Chain& operator=(const Max7219Chain&) = delete;

    // For GPIO transport, the shared DIN+CLK bus must be set BEFORE open()
    // is called. No-op for spidev transport. Owned externally
    // (Max7219PanelOutput) — chain only borrows the pointer.
    void set_gpio_bus(Max7219GpioBus* bus) { gpio_bus_ = bus; }

    // Open the chosen transport (spidev or gpio) and initialise every chip
    // (exit shutdown, no decode, scan limit 7, test off, intensity set).
    // Returns false on failure; is_open() stays false so callers can drop
    // the chain silently.
    bool open();
    void close();
    bool is_open() const {
        return (cfg_.transport == Transport::Spidev) ? (fd_ >= 0)
                                                     : cs_line_.is_open();
    }

    // Convert the configured canvas region to per-chip 8×8 mono bitmaps and
    // shift them out one register row at a time (8 SPI transfers per frame).
    // No-op when the chain isn't open or the canvas is too small to cover
    // the region.
    void show(const cv::Mat& rgb_canvas);

    const std::string& name() const { return cfg_.name; }

private:
    // Write (reg, value_for_chip_i) packets for every chip in one SPI write
    // (kernel auto-pulses CS at end). Caller passes the per-chip data array
    // ordered chip 0..N-1; we shift in reverse so chip 0 ends up at the
    // front of the chain.
    bool write_chain_register(uint8_t reg, const uint8_t* per_chip_data);

    // chip_index_in_chain[grid_col + grid_row * cols_chips] = order along the
    // daisy chain.
    int  chip_chain_index(int grid_col, int grid_row) const;

    Config cfg_;
    int    fd_         = -1;          // spidev fd (Spidev transport)
    int    total_chips_ = 0;          // cols_chips × rows_chips
    std::vector<uint8_t> tx_buf_;     // 2 bytes × total_chips (Spidev path)

    // GPIO transport (when cfg_.transport == Gpio)
    Max7219GpioBus*  gpio_bus_ = nullptr;
    GpioOutputGroup  cs_line_;        // per-chain CS line
};

} // namespace face
