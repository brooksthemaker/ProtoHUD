#include "neopixel_matrix_chain.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace face {

NeoPixelMatrixChain::NeoPixelMatrixChain(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.cols_chips  < 1) cfg_.cols_chips  = 1;
    if (cfg_.rows_chips  < 1) cfg_.rows_chips  = 1;
    if (cfg_.reset_bytes < 0) cfg_.reset_bytes = 0;
    total_chips_  = cfg_.cols_chips * cfg_.rows_chips;
    total_pixels_ = total_chips_ * 64;
    spi_buf_.assign(static_cast<size_t>(total_pixels_) * 9 + cfg_.reset_bytes, 0);
}

NeoPixelMatrixChain::~NeoPixelMatrixChain() { close(); }

bool NeoPixelMatrixChain::open() {
    if (fd_ >= 0) return true;
    fd_ = ::open(cfg_.spi_device.c_str(), O_WRONLY);
    if (fd_ < 0) {
        std::fprintf(stderr, "[neopixel:%s] cannot open %s: %s\n",
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
        std::fprintf(stderr, "[neopixel:%s] spidev ioctl failed: %s\n",
                     cfg_.name.c_str(), std::strerror(errno));
        close();
        return false;
    }
    // Push a blank frame so power-on garbage doesn't linger.
    cv::Mat blank(cv::Size(cfg_.canvas_x + cfg_.cols_chips * 8,
                           cfg_.canvas_y + cfg_.rows_chips * 8),
                  CV_8UC3, cv::Scalar(0, 0, 0));
    show(blank);
    return true;
}

void NeoPixelMatrixChain::close() {
    if (fd_ < 0) return;
    // Best effort: zero the strip so it doesn't hold the last frame lit.
    std::fill(spi_buf_.begin(), spi_buf_.end(), 0);
    ::write(fd_, spi_buf_.data(), spi_buf_.size());
    ::close(fd_);
    fd_ = -1;
}

int NeoPixelMatrixChain::chip_chain_index(int gc, int gr) const {
    if (cfg_.chain_order == ChainOrder::Serpentine && (gr & 1))
        gc = (cfg_.cols_chips - 1) - gc;
    return gr * cfg_.cols_chips + gc;
}

int NeoPixelMatrixChain::pixel_in_chip(int px, int py) const {
    // Both layouts have pixel 0 = (col 0, row 0). Difference is how rows
    // connect to each other inside a single 8×8 module.
    switch (cfg_.pixel_layout) {
    case PixelLayout::RowMajor:
        return py * 8 + px;
    case PixelLayout::AdafruitSerpentine:
    default:
        return (py & 1) ? (py * 8 + (7 - px)) : (py * 8 + px);
    }
}

void NeoPixelMatrixChain::encode_color_byte(uint8_t b, uint8_t* out) const {
    uint32_t acc = 0;
    for (int i = 7; i >= 0; --i)
        acc = (acc << 3) | ((b >> i) & 1u ? 0b110u : 0b100u);
    out[0] = static_cast<uint8_t>((acc >> 16) & 0xFF);
    out[1] = static_cast<uint8_t>((acc >>  8) & 0xFF);
    out[2] = static_cast<uint8_t>( acc        & 0xFF);
}

void NeoPixelMatrixChain::show(const cv::Mat& rgb_canvas) {
    if (fd_ < 0 || rgb_canvas.empty() || rgb_canvas.type() != CV_8UC3) return;

    const int region_w = cfg_.cols_chips * 8;
    const int region_h = cfg_.rows_chips * 8;
    if (cfg_.canvas_x < 0 || cfg_.canvas_y < 0 ||
        cfg_.canvas_x + region_w > rgb_canvas.cols ||
        cfg_.canvas_y + region_h > rgb_canvas.rows) {
        static bool logged = false;
        if (!logged) {
            std::fprintf(stderr, "[neopixel:%s] canvas region (%d,%d)+(%dx%d) "
                                  "out of bounds for %dx%d canvas\n",
                         cfg_.name.c_str(), cfg_.canvas_x, cfg_.canvas_y,
                         region_w, region_h, rgb_canvas.cols, rgb_canvas.rows);
            logged = true;
        }
        return;
    }

    const uint16_t br = cfg_.brightness;

    // For each chip in the grid → that chip's chain-index → 64 pixels in the
    // chip's wired order. The SPI buffer's pixel slots are addressed by
    // their position along the daisy-chain, which is exactly that index.
    for (int gr = 0; gr < cfg_.rows_chips; ++gr) {
        for (int gc = 0; gc < cfg_.cols_chips; ++gc) {
            const int chip_idx = chip_chain_index(gc, gr);
            for (int py = 0; py < 8; ++py) {
                const cv::Vec3b* row = rgb_canvas.ptr<cv::Vec3b>(
                    cfg_.canvas_y + gr * 8 + py);
                for (int px = 0; px < 8; ++px) {
                    const cv::Vec3b& p = row[cfg_.canvas_x + gc * 8 + px];
                    // Renderer canvas is RGB (channel 0 = R), per
                    // face_image.h's convention.
                    const uint8_t r = static_cast<uint8_t>((p[0] * br) / 255);
                    const uint8_t g = static_cast<uint8_t>((p[1] * br) / 255);
                    const uint8_t b = static_cast<uint8_t>((p[2] * br) / 255);

                    const int pix_idx = chip_idx * 64 + pixel_in_chip(px, py);
                    uint8_t* dst = spi_buf_.data() + pix_idx * 9;

                    // Pack in the chain's native byte order.
                    if (cfg_.color_order == ColorOrder::GRB) {
                        encode_color_byte(g, dst);
                        encode_color_byte(r, dst + 3);
                        encode_color_byte(b, dst + 6);
                    } else {
                        encode_color_byte(r, dst);
                        encode_color_byte(g, dst + 3);
                        encode_color_byte(b, dst + 6);
                    }
                }
            }
        }
    }
    // Tail reset bytes already zero.

    const ssize_t n = ::write(fd_, spi_buf_.data(), spi_buf_.size());
    if (n != static_cast<ssize_t>(spi_buf_.size())) {
        std::fprintf(stderr, "[neopixel:%s] spidev write returned %zd (want %zu): %s\n",
                     cfg_.name.c_str(), n, spi_buf_.size(), std::strerror(errno));
    }
}

} // namespace face
