#include "max7219_chain.h"
#include "max7219_gpio_bus.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <termios.h>
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
    // module_positions, when populated, fully owns the chip count — each
    // entry is one daisy-chained MAX7219 placed anywhere on the canvas.
    total_chips_ = cfg_.module_positions.empty()
        ? cfg_.cols_chips * cfg_.rows_chips
        : static_cast<int>(cfg_.module_positions.size());
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
    } else if (cfg_.transport == Transport::Coproc) {
        if (coproc_fd_ >= 0) return true;
        coproc_fd_ = ::open(cfg_.coproc_device.c_str(), O_WRONLY | O_NOCTTY);
        if (coproc_fd_ < 0) {
            std::fprintf(stderr, "[max7219:%s] cannot open coproc %s: %s\n",
                         cfg_.name.c_str(), cfg_.coproc_device.c_str(),
                         std::strerror(errno));
            return false;
        }
        termios tio{};
        if (tcgetattr(coproc_fd_, &tio) == 0) {   // raw: no NL→CRNL mangling
            cfmakeraw(&tio);
            tcsetattr(coproc_fd_, TCSANOW, &tio);
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
    if (coproc_fd_ >= 0) { ::close(coproc_fd_); coproc_fd_ = -1; }
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

    if (cfg_.transport == Transport::Coproc) {
        if (coproc_fd_ < 0) return false;
        // Same byte order as the spidev path (chip N-1 first … chip 0 last),
        // then ship it as one "SPI <cs> <hex>" line for the coproc to shift out.
        uint8_t* dst = tx_buf_.data();
        for (int i = total_chips_ - 1; i >= 0; --i) { *dst++ = reg; *dst++ = per_chip[i]; }
        static const char H[] = "0123456789ABCDEF";
        std::string line = "SPI " + std::to_string(cfg_.coproc_cs) + " ";
        line.reserve(line.size() + tx_buf_.size() * 2 + 1);
        for (uint8_t b : tx_buf_) { line.push_back(H[b >> 4]); line.push_back(H[b & 0x0F]); }
        line.push_back('\n');
        const ssize_t n = ::write(coproc_fd_, line.data(), line.size());
        return n == static_cast<ssize_t>(line.size());
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
    // is_open() handles both transports — fd_ is only set under Spidev,
    // so the original fd_ < 0 guard silently disabled GPIO-bit-bang chains.
    if (!is_open() || rgb_canvas.empty() || rgb_canvas.type() != CV_8UC3) return;

    // Build the per-module canvas-origin list. Rectangular chains derive
    // it from cols_chips/rows_chips/chain_order; non-rectangular chains
    // use module_positions verbatim.
    std::vector<std::array<int, 2>> mods;
    if (!cfg_.module_positions.empty()) {
        mods = cfg_.module_positions;
    } else {
        mods.reserve(static_cast<size_t>(total_chips_));
        // Walk the grid in chip_chain_index order so daisy index N lands at
        // the same canvas spot the legacy serpentine/row-major math used.
        for (int idx = 0; idx < total_chips_; ++idx) {
            int gc, gr;
            if (cfg_.chain_order == ChainOrder::Serpentine) {
                gr = idx / cfg_.cols_chips;
                int col = idx % cfg_.cols_chips;
                gc = (gr & 1) ? (cfg_.cols_chips - 1 - col) : col;
            } else {
                gr = idx / cfg_.cols_chips;
                gc = idx % cfg_.cols_chips;
            }
            mods.push_back({cfg_.canvas_x + gc * 8, cfg_.canvas_y + gr * 8});
        }
    }

    // Bounds-check every module; any out-of-range module disables the
    // frame (better silent skip than SIGSEGV on a stray write). Logged
    // once so operators can spot a misconfigured layout in the journal.
    for (const auto& m : mods) {
        if (m[0] < 0 || m[1] < 0 ||
            m[0] + 8 > rgb_canvas.cols || m[1] + 8 > rgb_canvas.rows) {
            static bool logged = false;
            if (!logged) {
                std::fprintf(stderr, "[max7219:%s] module (%d,%d) "
                                      "out of bounds for %dx%d canvas\n",
                             cfg_.name.c_str(), m[0], m[1],
                             rgb_canvas.cols, rgb_canvas.rows);
                logged = true;
            }
            return;
        }
    }

    // Grayscale + threshold the whole canvas once; we'll sample 8x8
    // windows per module below. Cheap relative to the SPI shift.
    cv::Mat mono;
    cv::cvtColor(rgb_canvas, mono, cv::COLOR_RGB2GRAY);
    cv::threshold(mono, mono, cfg_.threshold, 255, cv::THRESH_BINARY);

    // Shift one chip-register-row at a time across the whole chain. Each
    // row update sends `total_chips * 2` bytes — at 1 MHz SPI that's
    // <32 µs per row, ~256 µs per frame for a 16-chip face. Easily 60 Hz.
    std::vector<uint8_t> per_chip(total_chips_, 0);
    for (int chip_row = 0; chip_row < 8; ++chip_row) {
        for (size_t i = 0; i < mods.size(); ++i) {
            const int mx = mods[i][0];
            const int my = mods[i][1];
            uint8_t byte = 0;
            if (cfg_.module_type == ModuleType::FC16) {
                const uint8_t* row_ptr = mono.ptr<uint8_t>(my + chip_row);
                for (int px = 0; px < 8; ++px) {
                    if (row_ptr[mx + px]) byte |= (1u << (7 - px));
                }
            } else {
                // Generic 1088AS — rows/cols swapped on the PCB; sample
                // the column slice instead.
                for (int px = 0; px < 8; ++px) {
                    const int cx = mx + chip_row;
                    const int cy = my + (7 - px);
                    if (mono.ptr<uint8_t>(cy)[cx])
                        byte |= (1u << (7 - px));
                }
            }
            per_chip[i] = byte;
        }
        write_chain_register(REG_DIGIT_0 + chip_row, per_chip.data());
    }
}

} // namespace face
