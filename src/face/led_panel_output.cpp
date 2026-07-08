#include "led_panel_output.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace face {

// ── LedPanelChain ─────────────────────────────────────────────────────────────

LedPanelChain::LedPanelChain(Config cfg) : cfg_(std::move(cfg)) {
    cfg_.global_5bit = std::clamp(cfg_.global_5bit, 1, 31);
    cfg_.max_leds    = std::clamp(cfg_.max_leds, 1, 4096);
    load_mapping();
    // APA102 frame: 4 start bytes + 4/LED + end clocks (n/2 bits → n/16 bytes).
    wire_.reserve(4 + map_.size() * 4 + map_.size() / 16 + 4);
}

LedPanelChain::~LedPanelChain() { close(); }

bool LedPanelChain::load_mapping() {
    map_.clear();
    if (!cfg_.map_points.empty()) {
        map_ = cfg_.map_points;
    } else if (!cfg_.map_file.empty()) {
        std::ifstream f(cfg_.map_file);
        if (!f) {
            std::fprintf(stderr, "[ledpanel:%s] cannot open map file %s\n",
                         cfg_.name.c_str(), cfg_.map_file.c_str());
            return false;
        }
        try {
            const nlohmann::json j = nlohmann::json::parse(f);
            for (const auto& p : j.at("leds")) {
                if (!p.is_array() || p.size() < 2) continue;
                map_.push_back({ p[0].get<int>(), p[1].get<int>() });
                if ((int)map_.size() >= cfg_.max_leds) break;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ledpanel:%s] bad map file %s: %s\n",
                         cfg_.name.c_str(), cfg_.map_file.c_str(), e.what());
            map_.clear();
            return false;
        }
    } else if (cfg_.cols > 0 && cfg_.rows > 0) {
        // Rectangular prototype: cols x rows grid, serpentine chain order.
        const int pitch = std::max(1, cfg_.pitch);
        for (int r = 0; r < cfg_.rows; ++r)
            for (int c = 0; c < cfg_.cols; ++c) {
                const int col = (cfg_.serpentine && (r & 1)) ? cfg_.cols - 1 - c : c;
                map_.push_back({ cfg_.canvas_x + col * pitch,
                                 cfg_.canvas_y + r * pitch });
                if ((int)map_.size() >= cfg_.max_leds) break;
            }
    }
    if ((int)map_.size() > cfg_.max_leds) map_.resize(cfg_.max_leds);
    return !map_.empty();
}

cv::Rect LedPanelChain::bounding_rect() const {
    if (map_.empty()) return {};
    int x0 = map_[0][0], y0 = map_[0][1], x1 = x0, y1 = y0;
    for (const auto& p : map_) {
        x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]);
        y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]);
    }
    return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

bool LedPanelChain::open() {
    if (fd_ >= 0) return true;
    if (map_.empty()) {
        std::fprintf(stderr, "[ledpanel:%s] no LED mapping — chain disabled\n",
                     cfg_.name.c_str());
        return false;
    }
    fd_ = ::open(cfg_.spi_device.c_str(), O_WRONLY);
    if (fd_ < 0) {
        std::fprintf(stderr, "[ledpanel:%s] cannot open %s: %s\n",
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
        std::fprintf(stderr, "[ledpanel:%s] spidev ioctl failed: %s\n",
                     cfg_.name.c_str(), std::strerror(errno));
        close();
        return false;
    }
    std::fprintf(stderr, "[ledpanel:%s] %d LEDs on %s @ %.1f MHz (~%.1f ms/frame)\n",
                 cfg_.name.c_str(), led_count(), cfg_.spi_device.c_str(),
                 cfg_.speed_hz / 1e6,
                 (4.0 + map_.size() * 4.0 + map_.size() / 16.0) * 8.0 /
                     cfg_.speed_hz * 1000.0);
    return true;
}

void LedPanelChain::close() {
    if (fd_ < 0) return;
    // Blank the panel so it doesn't hold the last frame lit.
    cv::Mat blank(1, 1, CV_8UC3, cv::Scalar(0, 0, 0));
    std::vector<uint8_t> off;
    encode_frame(blank, off);          // all sample points clamp to the black px
    (void)!::write(fd_, off.data(), off.size());
    ::close(fd_);
    fd_ = -1;
}

double LedPanelChain::encode_frame(const cv::Mat& rgb, std::vector<uint8_t>& out) const {
    const int n = static_cast<int>(map_.size());
    out.clear();
    out.reserve(4 + n * 4 + n / 16 + 4);

    // Pass 1: sample + estimate current for the power cap. Model: ~20 mA per
    // fully-lit color channel (60 mA/LED white), scaled by software
    // brightness and the APA102 5-bit global.
    static thread_local std::vector<uint8_t> px;
    px.assign(static_cast<size_t>(n) * 3, 0);
    double chan_sum = 0.0;                              // Σ channel/255
    for (int i = 0; i < n; ++i) {
        const int x = std::clamp(map_[i][0], 0, rgb.cols - 1);
        const int y = std::clamp(map_[i][1], 0, rgb.rows - 1);
        const cv::Vec3b& p = rgb.at<cv::Vec3b>(y, x);   // canvas is RGB
        const uint8_t r = (uint16_t)p[0] * cfg_.brightness / 255;
        const uint8_t g = (uint16_t)p[1] * cfg_.brightness / 255;
        const uint8_t b = (uint16_t)p[2] * cfg_.brightness / 255;
        px[i * 3 + 0] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b;
        chan_sum += (r + g + b) / 255.0;
    }
    const double global_scale = cfg_.global_5bit / 31.0;
    const double est_a = chan_sum * 0.020 * global_scale;
    double scale = 1.0;
    if (cfg_.power_limit_a > 0.0 && est_a > cfg_.power_limit_a)
        scale = cfg_.power_limit_a / est_a;

    // Pass 2: wire format. Start frame, per-LED [0xE0|global, B, G, R]
    // (APA102 wire order), end clocks (≥ n/2 extra clock edges).
    out.push_back(0x00); out.push_back(0x00);
    out.push_back(0x00); out.push_back(0x00);
    const uint8_t hdr = static_cast<uint8_t>(0xE0 | cfg_.global_5bit);
    for (int i = 0; i < n; ++i) {
        out.push_back(hdr);
        out.push_back(static_cast<uint8_t>(px[i * 3 + 2] * scale));  // B
        out.push_back(static_cast<uint8_t>(px[i * 3 + 1] * scale));  // G
        out.push_back(static_cast<uint8_t>(px[i * 3 + 0] * scale));  // R
    }
    const int end_bytes = n / 16 + 1;
    for (int i = 0; i < end_bytes; ++i) out.push_back(0x00);
    return scale;
}

void LedPanelChain::show(const cv::Mat& rgb) {
    if (fd_ < 0 || rgb.empty() || rgb.type() != CV_8UC3) return;
    encode_frame(rgb, wire_);
    (void)!::write(fd_, wire_.data(), wire_.size());
}

// ── LedPanelOutput ────────────────────────────────────────────────────────────

LedPanelOutput::LedPanelOutput(Config cfg) {
    for (auto& c : cfg.chains)
        chains_.push_back(std::make_unique<LedPanelChain>(std::move(c)));
}

LedPanelOutput::~LedPanelOutput() { close(); }

bool LedPanelOutput::open() {
    bool any = false;
    for (auto& c : chains_) any = c->open() || any;
    return any;
}

void LedPanelOutput::show(const cv::Mat& rgb) {
    for (auto& c : chains_) c->show(rgb);
}

void LedPanelOutput::close() {
    for (auto& c : chains_) c->close();
}

std::vector<cv::Rect> LedPanelOutput::covered_regions() const {
    std::vector<cv::Rect> out;
    for (const auto& c : chains_) {
        const cv::Rect r = c->bounding_rect();
        if (r.width > 0) out.push_back(r);
    }
    return out;
}

std::vector<NamedRegion> LedPanelOutput::covered_named_regions() const {
    std::vector<NamedRegion> out;
    for (const auto& c : chains_) {
        const cv::Rect r = c->bounding_rect();
        if (r.width > 0) out.push_back({ c->config().name, r });
    }
    return out;
}

} // namespace face
