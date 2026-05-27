#include "neopixel_matrix_output.h"

#include <cstdio>

namespace face {

NeoPixelMatrixOutput::NeoPixelMatrixOutput(Config cfg) : cfg_(std::move(cfg)) {
    chains_.reserve(cfg_.chains.size());
    for (const auto& cc : cfg_.chains)
        chains_.push_back(std::make_unique<NeoPixelMatrixChain>(cc));
}

NeoPixelMatrixOutput::~NeoPixelMatrixOutput() { close(); }

bool NeoPixelMatrixOutput::open() {
    bool any_ok = false;
    for (auto& ch : chains_) if (ch->open()) any_ok = true;
    if (!any_ok && !chains_.empty()) {
        std::fprintf(stderr, "[neopixel] no chains came up — backend unavailable\n");
        return false;
    }
    return true;
}

void NeoPixelMatrixOutput::show(const cv::Mat& rgb) {
    for (auto& ch : chains_) ch->show(rgb);
}

void NeoPixelMatrixOutput::close() {
    for (auto& ch : chains_) ch->close();
}

std::vector<cv::Rect> NeoPixelMatrixOutput::covered_regions() const {
    std::vector<cv::Rect> out;
    out.reserve(cfg_.chains.size());
    for (const auto& cc : cfg_.chains) {
        out.emplace_back(cc.canvas_x, cc.canvas_y,
                         cc.cols_chips * 8, cc.rows_chips * 8);
    }
    return out;
}

std::vector<NamedRegion> NeoPixelMatrixOutput::covered_named_regions() const {
    std::vector<NamedRegion> out;
    out.reserve(cfg_.chains.size());
    for (const auto& cc : cfg_.chains) {
        out.push_back({cc.name,
                       cv::Rect(cc.canvas_x, cc.canvas_y,
                                cc.cols_chips * 8, cc.rows_chips * 8)});
    }
    return out;
}

} // namespace face
