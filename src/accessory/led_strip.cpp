#include "led_strip.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace accessory {

namespace {
constexpr uint8_t SPI_MODE = SPI_MODE_0;
constexpr uint8_t BITS_PER_WORD = 8;
} // namespace

LedStrip::LedStrip(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.count < 0)        cfg_.count = 0;
    if (cfg_.reset_bytes < 0)  cfg_.reset_bytes = 0;
    pixels_.assign(static_cast<size_t>(cfg_.count) * 3, 0);
    spi_buf_.assign(static_cast<size_t>(cfg_.count) * 9 + cfg_.reset_bytes, 0);
}

LedStrip::~LedStrip() { close(); }

bool LedStrip::open() {
    if (fd_ >= 0) return true;
    fd_ = ::open(cfg_.spi_device.c_str(), O_WRONLY);
    if (fd_ < 0) {
        std::fprintf(stderr, "[led] cannot open %s: %s\n",
                     cfg_.spi_device.c_str(), std::strerror(errno));
        return false;
    }
    uint8_t  mode  = SPI_MODE;
    uint8_t  bits  = BITS_PER_WORD;
    uint32_t speed = static_cast<uint32_t>(cfg_.speed_hz);
    if (ioctl(fd_, SPI_IOC_WR_MODE,          &mode)  < 0 ||
        ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits)  < 0 ||
        ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ,  &speed) < 0) {
        std::fprintf(stderr, "[led] spidev ioctl failed: %s\n",
                     std::strerror(errno));
        close();
        return false;
    }
    return true;
}

void LedStrip::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void LedStrip::set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx < 0 || idx >= cfg_.count) return;
    const size_t base = static_cast<size_t>(idx) * 3;
    // Store in the chain's native byte order so encoding is a straight walk.
    switch (cfg_.color_order) {
        case ColorOrder::GRB: pixels_[base+0] = g; pixels_[base+1] = r; pixels_[base+2] = b; break;
        case ColorOrder::RGB: pixels_[base+0] = r; pixels_[base+1] = g; pixels_[base+2] = b; break;
        case ColorOrder::BGR: pixels_[base+0] = b; pixels_[base+1] = g; pixels_[base+2] = r; break;
    }
}

void LedStrip::fill(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < cfg_.count; ++i) set_pixel(i, r, g, b);
}

// Encode 8 LED bits → 24 SPI bits = 3 SPI bytes, MSB first.
//
// Packing into a uint32 first lets us drop the per-bit branches into the
// inner loop and write 3 bytes out at the end.
void LedStrip::encode_color_byte(uint8_t b, uint8_t* out) const {
    uint32_t acc = 0;
    for (int i = 7; i >= 0; --i)
        acc = (acc << 3) | ((b >> i) & 1u ? 0b110u : 0b100u);
    out[0] = static_cast<uint8_t>((acc >> 16) & 0xFF);
    out[1] = static_cast<uint8_t>((acc >>  8) & 0xFF);
    out[2] = static_cast<uint8_t>( acc        & 0xFF);
}

bool LedStrip::show() {
    if (fd_ < 0 || cfg_.count <= 0) return false;

    // Apply master brightness on the fly while encoding so the stored
    // pixel values stay at their full-range logical magnitude.
    const uint16_t br = brightness_;
    uint8_t* dst = spi_buf_.data();
    for (size_t i = 0; i < pixels_.size(); ++i) {
        const uint8_t scaled = static_cast<uint8_t>(
            (static_cast<uint16_t>(pixels_[i]) * br) / 255);
        encode_color_byte(scaled, dst);
        dst += 3;
    }
    // Reset bytes already zero-initialised and untouched here.

    const ssize_t n = ::write(fd_, spi_buf_.data(), spi_buf_.size());
    if (n != static_cast<ssize_t>(spi_buf_.size())) {
        std::fprintf(stderr, "[led] spidev write returned %zd (want %zu): %s\n",
                     n, spi_buf_.size(), std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace accessory
