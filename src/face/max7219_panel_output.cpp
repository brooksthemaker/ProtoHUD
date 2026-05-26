#include "max7219_panel_output.h"

#include <cstdio>

namespace face {

Max7219PanelOutput::Max7219PanelOutput(Config cfg) : cfg_(std::move(cfg)) {
    chains_.reserve(cfg_.chains.size());
    for (const auto& cc : cfg_.chains)
        chains_.push_back(std::make_unique<Max7219Chain>(cc));
}

Max7219PanelOutput::~Max7219PanelOutput() { close(); }

bool Max7219PanelOutput::open() {
    bool any_ok = false;
    for (auto& ch : chains_) {
        if (ch->open()) any_ok = true;
        // A chain that fails to open stays "not-open" — show() short-circuits
        // for it. Other chains still drive their zone.
    }
    if (!any_ok && !chains_.empty()) {
        std::fprintf(stderr, "[max7219] no chains came up — backend unavailable\n");
        return false;
    }
    return true;
}

void Max7219PanelOutput::show(const cv::Mat& rgb) {
    for (auto& ch : chains_) ch->show(rgb);
}

void Max7219PanelOutput::close() {
    for (auto& ch : chains_) ch->close();
}

} // namespace face
