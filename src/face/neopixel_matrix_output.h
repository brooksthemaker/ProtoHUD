#pragma once
// ── neopixel_matrix_output.h ──────────────────────────────────────────────────
// PanelOutput backend for builds that swap MAX7219 modules for pin-compatible
// WS2812-based 8x8 RGB matrices. Owns one or more NeoPixelMatrixChain
// instances — one per zone (eye-L / eye-R / nose / mouth) — and distributes
// the renderer's RGB canvas across them.

#include "panel_output.h"
#include "neopixel_matrix_chain.h"

#include <memory>
#include <vector>

namespace face {

class NeoPixelMatrixOutput : public PanelOutput {
public:
    struct Config {
        std::vector<NeoPixelMatrixChain::Config> chains;
    };

    explicit NeoPixelMatrixOutput(Config cfg);
    ~NeoPixelMatrixOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;
    std::vector<cv::Rect> covered_regions() const override;
    std::vector<NamedRegion> covered_named_regions() const override;
    bool supports_face_editor() const override { return true; }

private:
    Config                                          cfg_;
    std::vector<std::unique_ptr<NeoPixelMatrixChain>> chains_;
};

} // namespace face
