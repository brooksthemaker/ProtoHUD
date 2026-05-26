#include "max7219_chain.h"
#include "max7219_gpio_bus.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

namespace face {

namespace {
// MAX7219 register addresses we care about (datasheet "Table 2").
constexpr uint8_t REG_NOOP         = 0x00;
constexpr uint8_t REG_DIGIT_0      = 0x01;   // +0..7 for the 8 rows
constexpr uint8_t REG_DECODE_MODE  = 0x09;
constexpr uint8_t REG_INTENSITY    = 0x0A;
constexpr uint8_t REG_SCAN_LIMIT   = 0x0B;
constexpr uint8_t REG_SHUTDOWN     = 0x0C;
constexpr uint8_t REG_DISPLAY_TEST = 0x0F;
} // namespace

Max7219Chain::Max7219Chain(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.cols_chips < 1) cfg_.cols_chips = 1;
    if (cfg_.rows_chips < 1) cfg_.rows_chips = 1;
    total_chips_ = cfg_.cols_chips * cfg_.rows_chips;
    tx_buf_.assign(static_cast<size_t>(total_chips_) * 2, 0);
}

Max7219Chain::~Max7219Chain() { close(); }

bool Max7219Chain::open() {
    if (cfg_.transport == Transport::Spidev) {
        if (fd_ >= 0) return true;
        fd_ = ::open(cfg_.spi_device.c_str(), O_WRONLY);
        if (fd_ < 0) {
            std::fprintf(stderr, "[max7219:%s] cannot open %s: %s\n",
                         cfg_.name.c_str(), cfg_.spi_device.c_str(),
                         std::strerror(errno));
            return false;
        }
        uint8_t  mode  = SPI_MODE_0;
        uint8_t  bits  = 8;
        uint32_t speed = static_cast<uint32_t>(cfg_.speed_hz);
        if (ioctl(fd_, SPI_IOC_WR_MODE,          &mode)  < 0 ||
            ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits)  < 0 ||
            ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ,  &speed) < 0) {
            std::fprintf(stderr, "[max7219:%s] spidev ioctl failed: %s\n",
                         cfg_.name.c_str(), std::strerror(errno));
            close();
            return false;
        }
    } else {
        // GPIO transport — the panel output must have given us a bus + a CS
        // pin must be configured. Failure to claim CS leaves cs_line_ closed
        // so is_open() returns false and show()/close() are no-ops.
        if (!gpio_bus_ || !gpio_bus_->is_open() || cfg_.gpio_cs_pin < 0) {
            std::fprintf(stderr, "[max7219:%s] gpio transport missing bus/CS\n",
                         cfg_.name.c_str());
            return false;
        }
        const uint32_t cs_offset = static_cast<uint32_t>(cfg_.gpio_cs_pin);
        if (!cs_line_.open(cfg_.gpio_chip, &cs_offset, 1, "max7219-cs")) {
            std::fprintf(stderr, "[max7219:%s] failed to claim CS GPIO %d on %s\n",
                         cfg_.name.c_str(), cfg_.gpio_cs_pin,
                         cfg_.gpio_chip.c_str());
            return false;
        }
        // Idle CS high (MAX7219 latches on CS rising edge).
        cs_line_.set_values(1, 1);
    }

    // Initial register sweep — same value to every chip via the chain.
    std::vector<uint8_t> all(total_chips_, 0);
    auto bcast = [&](uint8_t reg, uint8_t value) {
        std::fill(all.begin(), all.end(), value);
        write_chain_register(reg, all.data());
    };
    bcast(REG_DISPLAY_TEST, 0x00);                          // out of test mode
    bcast(REG_SCAN_LIMIT,   0x07);                          // all 8 rows
    bcast(REG_DECODE_MODE,  0x00);                          // raw matrix, no BCD
    bcast(REG_INTENSITY,    cfg_.intensity & 0x0F);
    bcast(REG_SHUTDOWN,     0x01);                          // normal operation

    // Blank everything so we don't start on whatever the chip latched at
    // power-on.
    std::fill(all.begin(), all.end(), 0);
    for (uint8_t r = 0; r < 8; ++r) write_chain_register(REG_DIGIT_0 + r, all.data());
    return true;
}

void Max7219Chain::close() {
    if (!is_open()) return;
    // Best effort: blank, then enter shutdown so the matrix doesn't keep
    // the last frame lit if our process exits unexpectedly.
    std::vector<uint8_t> zero(total_chips_, 0);
    for (uint8_t r = 0; r < 8; ++r) write_chain_register(REG_DIGIT_0 + r, zero.data());
    write_chain_register(REG_SHUTDOWN, zero.data());        // bcast 0 → shutdown
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    cs_line_.close();
}

bool Max7219Chain::write_chain_register(uint8_t reg, const uint8_t* per_chip) {
    if (cfg_.transport == Transport::Spidev) {
        if (fd_ < 0) return false;
        // SPI is shifted MSB-first; the last byte written lands in chip 0
        // (closest to the master), so iterate per_chip in reverse.
        uint8_t* dst = tx_buf_.data();
        for (int i = total_chips_ - 1; i >= 0; --i) {
            *dst++ = reg;
            *dst++ = per_chip[i];
        }
        const ssize_t n = ::write(fd_, tx_buf_.data(), tx_buf_.size());
        return n == static_cast<ssize_t>(tx_buf_.size());
    }

    // GPIO bit-bang: CS low for the duration of the write; same MSB-first
    // shift order as the SPI path so chip 0 (closest to the master) ends up
    // with the LAST byte pair we send.
    if (!cs_line_.is_open() || !gpio_bus_ || !gpio_bus_->is_open()) return false;
    cs_line_.set_values(0, 1);                              // CS low (latch start)
    for (int i = total_chips_ - 1; i >= 0; --i) {
        gpio_bus_->shift_byte(reg);
        gpio_bus_->shift_byte(per_chip[i]);
    }
    cs_line_.set_values(1, 1);                              // CS high (latches into chips)
    return true;
}

int Max7219Chain::chip_chain_index(int gc, int gr) const {
    if (cfg_.chain_order == ChainOrder::Serpentine && (gr & 1))
        gc = (cfg_.cols_chips - 1) - gc;
    return gr * cfg_.cols_chips + gc;
}

void Max7219Chain::show(const cv::Mat& rgb_canvas) {
    if (fd_ < 0 || rgb_canvas.empty() || rgb_canvas.type() != CV_8UC3) return;

    const int region_w = cfg_.cols_chips * 8;
    const int region_h = cfg_.rows_chips * 8;
    if (cfg_.canvas_x < 0 || cfg_.canvas_y < 0 ||
        cfg_.canvas_x + region_w > rgb_canvas.cols ||
        cfg_.canvas_y + region_h > rgb_canvas.rows) {
        // Misconfigured region; skip silently rather than risk a SIGSEGV on
        // unrelated frame writes. Operator can read the log once.
        static bool logged = false;
        if (!logged) {
            std::fprintf(stderr, "[max7219:%s] canvas region (%d,%d)+(%dx%d) "
                                  "out of bounds for %dx%d canvas\n",
                         cfg_.name.c_str(), cfg_.canvas_x, cfg_.canvas_y,
                         region_w, region_h, rgb_canvas.cols, rgb_canvas.rows);
            logged = true;
        }
        return;
    }

    // Crop + grayscale + threshold once per frame.
    cv::Mat region = rgb_canvas(cv::Rect(cfg_.canvas_x, cfg_.canvas_y,
                                         region_w, region_h));
    cv::Mat mono;
    cv::cvtColor(region, mono, cv::COLOR_RGB2GRAY);
    cv::threshold(mono, mono, cfg_.threshold, 255, cv::THRESH_BINARY);

    // Shift one chip-register-row at a time across the whole chain. Each
    // row update sends `total_chips * 2` bytes — at 1 MHz SPI that's <32 µs
    // per row, ~256 µs per frame for a 16-chip face. Easily 60 Hz.
    std::vector<uint8_t> per_chip(total_chips_, 0);
    for (int chip_row = 0; chip_row < 8; ++chip_row) {
        for (int gr = 0; gr < cfg_.rows_chips; ++gr) {
            const int canvas_y = gr * 8 + chip_row;
            const uint8_t* row_ptr = mono.ptr<uint8_t>(canvas_y);
            for (int gc = 0; gc < cfg_.cols_chips; ++gc) {
                uint8_t byte = 0;
                if (cfg_.module_type == ModuleType::FC16) {
                    // Direct mapping: bit 7 = leftmost pixel in this chip's
                    // 8-pixel column slice.
                    for (int px = 0; px < 8; ++px) {
                        if (row_ptr[gc * 8 + px]) byte |= (1u << (7 - px));
                    }
                } else {
                    // Generic 1088AS — rows/cols are physically swapped on
                    // these boards. We need the *column* slice instead of a
                    // row slice. Slow path: 8 separate lookups.
                    for (int px = 0; px < 8; ++px) {
                        const int cx = gc * 8 + chip_row;          // chip-local col
                        const int cy = gr * 8 + (7 - px);          // chip-local row, flipped
                        if (cx < region_w && cy < region_h &&
                            mono.ptr<uint8_t>(cy)[cx])
                            byte |= (1u << (7 - px));
                    }
                }
                per_chip[chip_chain_index(gc, gr)] = byte;
            }
        }
        write_chain_register(REG_DIGIT_0 + chip_row, per_chip.data());
    }
}

} // namespace face
