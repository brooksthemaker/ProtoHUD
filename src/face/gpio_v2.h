#pragma once
// ── gpio_v2.h ─────────────────────────────────────────────────────────────────
// Thin RAII wrapper around the Linux gpio_v2 line-request ABI for output-only
// bit-banging. Used by Max7219Chain when its `transport` is "gpio" so MAX7219
// chains can run alongside HUB75 (which owns SPI1's pins) and WS2812 (which
// owns SPI0's data line).
//
// Each GpioOutputGroup owns one line-request fd covering up to 64 lines. Bits
// in set_values() are indexed by position in the offsets array passed to
// open() — bit 0 = first offset, bit 1 = second, etc.

#include <cstdint>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace face {

class GpioOutputGroup {
public:
    GpioOutputGroup() = default;
    ~GpioOutputGroup() { close(); }

    GpioOutputGroup(const GpioOutputGroup&)            = delete;
    GpioOutputGroup& operator=(const GpioOutputGroup&) = delete;

    // Claim `n_lines` GPIO offsets on `chip_dev` (e.g. "/dev/gpiochip0") as
    // outputs, all sharing a single request fd. consumer is the human-readable
    // label userspace sees in `gpioinfo`.
    bool open(const std::string& chip_dev, const uint32_t* offsets,
              int n_lines, const char* consumer) {
        if (line_fd_ >= 0) return true;
        if (n_lines <= 0 || n_lines > GPIO_V2_LINES_MAX) return false;

        int chip = ::open(chip_dev.c_str(), O_RDWR | O_CLOEXEC);
        if (chip < 0) return false;

        gpio_v2_line_request req{};
        std::memcpy(req.offsets, offsets, sizeof(uint32_t) * n_lines);
        req.num_lines       = static_cast<uint32_t>(n_lines);
        req.config.flags    = GPIO_V2_LINE_FLAG_OUTPUT;
        if (consumer)
            std::strncpy(req.consumer, consumer, sizeof(req.consumer) - 1);

        if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0 || req.fd < 0) {
            ::close(chip);
            return false;
        }
        line_fd_ = req.fd;
        n_lines_ = n_lines;
        ::close(chip);   // chip fd no longer needed; line fd persists
        return true;
    }

    void close() {
        if (line_fd_ >= 0) {
            ::close(line_fd_);
            line_fd_ = -1;
            n_lines_ = 0;
        }
    }

    bool is_open() const { return line_fd_ >= 0; }

    // Atomically update the lines covered by `mask` to the corresponding bits
    // of `bits`. Bit index = position in the open() offsets array. Returns
    // false on ioctl error (errno preserved).
    bool set_values(uint64_t bits, uint64_t mask) {
        if (line_fd_ < 0) return false;
        gpio_v2_line_values v{};
        v.bits = bits;
        v.mask = mask;
        return ioctl(line_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) == 0;
    }

private:
    int line_fd_ = -1;
    int n_lines_ = 0;
};

} // namespace face
