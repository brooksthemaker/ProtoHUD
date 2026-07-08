#pragma once
// ── led_panel_output.h ────────────────────────────────────────────────────────
// PanelOutput backend for LARGE custom face panels built from clocked
// addressable LEDs — APA102 / SK9822 ("DotStar") — driven straight from the
// CM5's hardware SPI. Scale target: up to ~2000 LEDs per panel, 4 panels
// (8000 LEDs) at 30-60+ fps.
//
// Why APA102 for the face (and not WS2812): WS2812's data rate is fixed at
// 800 kbit/s, so a 2000-LED chain takes 60 ms to refresh — 16 fps ceiling no
// matter what drives it. APA102 is CLOCKED: at 12 MHz SPI a 2000-LED chain
// refreshes in ~5 ms, all four panels comfortably beat 60 fps, and there are
// no realtime timing constraints at all (spidev from userspace is fine).
// WS2812 stays supported for small 8x8-module builds via the existing
// "rgb_matrix" backend.
//
// Custom panels aren't grids: each chain carries a PER-LED MAPPING — LED i
// (chain order) → a canvas sample point (x, y). Ship a JSON map file
// ({"leds": [[x, y], ...]}), or let the built-in grid generator cover
// rectangular prototypes. The renderer composites the SAME canvas as every
// other backend (expressions, water, frost, reactions all just work); this
// backend samples it per LED and streams the wire frames.
//
// POWER: 8000 RGB LEDs can theoretically draw ~480 A at full white — wiring
// and PSUs cannot be an afterthought. Every chain enforces power_limit_a in
// software: the frame's estimated current is computed per show() and global
// brightness is scaled down when a frame would exceed the cap.

#include "panel_output.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace face {

class LedPanelChain {
public:
    struct Config {
        std::string name;                        // "left_cheek", "muzzle", …
        std::string spi_device = "/dev/spidev0.0";
        int         speed_hz   = 12'000'000;     // APA102 happily takes 8-24 MHz
        // Per-LED canvas sample points, in CHAIN ORDER. Three sources, first
        // match wins:
        //   1. map_points (already-parsed pairs — tests / generated configs)
        //   2. map_file — JSON {"leds": [[x, y], ...]} in canvas pixels
        //   3. grid fallback — cols x rows at canvas_x/y, pitch px, serpentine
        std::vector<std::array<int, 2>> map_points;
        std::string map_file;
        int  cols = 0, rows = 0;
        int  canvas_x = 0, canvas_y = 0;
        int  pitch = 1;                          // canvas px between grid LEDs
        bool serpentine = true;
        uint8_t brightness   = 64;               // software scale 0-255
        int     global_5bit  = 31;               // APA102 per-LED global 1-31
        double  power_limit_a = 8.0;             // per-chain current cap (5 V rail)
        int     max_leds      = 2048;            // sanity cap per chain
    };

    explicit LedPanelChain(Config cfg);
    ~LedPanelChain();

    bool open();
    void show(const cv::Mat& rgb);               // sample canvas + stream frame
    void close();

    int  led_count() const { return static_cast<int>(map_.size()); }
    const Config& config() const { return cfg_; }
    cv::Rect bounding_rect() const;              // mapping extent, for the editor

    // Wire-format encoder, exposed for the off-target harness: samples the
    // canvas through the mapping and fills `out` with the full APA102 frame
    // (start frame, per-LED 0xE0|global B G R, end clocks). Returns the
    // brightness scale actually applied after the power cap (1.0 = uncapped).
    double encode_frame(const cv::Mat& rgb, std::vector<uint8_t>& out) const;

private:
    bool load_mapping();

    Config cfg_;
    int    fd_ = -1;
    std::vector<std::array<int, 2>> map_;        // resolved LED → canvas point
    mutable std::vector<uint8_t> wire_;          // reused frame buffer
};

// One or more chains (one per physical panel), fed from the shared canvas.
class LedPanelOutput : public PanelOutput {
public:
    struct Config {
        std::vector<LedPanelChain::Config> chains;
    };

    explicit LedPanelOutput(Config cfg);
    ~LedPanelOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;
    std::vector<cv::Rect> covered_regions() const override;
    std::vector<NamedRegion> covered_named_regions() const override;
    bool supports_face_editor() const override { return true; }

private:
    std::vector<std::unique_ptr<LedPanelChain>> chains_;
};

} // namespace face
