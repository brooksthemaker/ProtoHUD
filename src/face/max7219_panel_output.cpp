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
    // If any chain wants the GPIO transport, bring up the shared DIN+CLK bus
    // first so chain->open() finds it ready when it claims its CS line.
    bool need_gpio_bus = false;
    for (const auto& cc : cfg_.chains)
        if (cc.transport == Max7219Chain::Transport::Gpio) { need_gpio_bus = true; break; }
    if (need_gpio_bus) {
        if (cfg_.gpio_din_pin < 0 || cfg_.gpio_clk_pin < 0) {
            std::fprintf(stderr,
                "[max7219] gpio transport requested but bus DIN/CLK pins unset\n");
            return false;
        }
        if (!gpio_bus_.open(cfg_.gpio_chip, cfg_.gpio_din_pin, cfg_.gpio_clk_pin)) {
            std::fprintf(stderr,
                "[max7219] gpio bus open failed (chip=%s din=%d clk=%d)\n",
                cfg_.gpio_chip.c_str(), cfg_.gpio_din_pin, cfg_.gpio_clk_pin);
            return false;
        }
        for (auto& ch : chains_) ch->set_gpio_bus(&gpio_bus_);
    }

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
    gpio_bus_.close();
}

std::vector<cv::Rect> Max7219PanelOutput::covered_regions() const {
    // Per-module 8×8 rects so the editor's bbox math (union) works the
    // same whether the chain is rectangular or made up of scattered modules
    // (per-side wiring).
    std::vector<cv::Rect> out;
    for (const auto& cc : cfg_.chains) {
        if (!cc.module_positions.empty()) {
            for (const auto& m : cc.module_positions)
                out.emplace_back(m[0], m[1], 8, 8);
        } else {
            out.emplace_back(cc.canvas_x, cc.canvas_y,
                             cc.cols_chips * 8, cc.rows_chips * 8);
        }
    }
    return out;
}

std::vector<NamedRegion> Max7219PanelOutput::covered_named_regions() const {
    // For scattered-module chains we emit one labelled rect per module
    // ("eye_l#0", "eye_l#1", …) so the editor's overlay can outline each
    // physical panel individually.
    std::vector<NamedRegion> out;
    for (const auto& cc : cfg_.chains) {
        if (!cc.module_positions.empty()) {
            for (size_t i = 0; i < cc.module_positions.size(); ++i) {
                char nm[64];
                std::snprintf(nm, sizeof(nm), "%s#%zu", cc.name.c_str(), i);
                out.push_back({nm,
                               cv::Rect(cc.module_positions[i][0],
                                        cc.module_positions[i][1], 8, 8)});
            }
        } else {
            out.push_back({cc.name,
                           cv::Rect(cc.canvas_x, cc.canvas_y,
                                    cc.cols_chips * 8, cc.rows_chips * 8)});
        }
    }
    return out;
}

} // namespace face
