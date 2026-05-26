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

} // namespace face
