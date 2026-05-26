#pragma once
// ── max7219_panel_output.h ────────────────────────────────────────────────────
// PanelOutput implementation for MAX7219 8×8 LED matrix daisy-chains. Owns
// one or more Max7219Chain instances — one per functional zone of the face
// (left eye / right eye / nose / mouth) — each with its own SPI bus, grid
// dimensions, and canvas region. show() distributes the renderer's RGB
// canvas across them.

#include "panel_output.h"
#include "max7219_chain.h"
#include "max7219_gpio_bus.h"

#include <memory>
#include <string>
#include <vector>

namespace face {

class Max7219PanelOutput : public PanelOutput {
public:
    struct Config {
        // Optional shared DIN+CLK GPIO bus — used by any chain whose
        // transport is Gpio. When unset (din_pin < 0) and any chain wants
        // the GPIO path, open() returns false. Hardware-SPI chains ignore
        // this entirely.
        std::string gpio_chip = "/dev/gpiochip0";
        int         gpio_din_pin = -1;        // BCM pin number
        int         gpio_clk_pin = -1;

        std::vector<Max7219Chain::Config> chains;
    };

    explicit Max7219PanelOutput(Config cfg);
    ~Max7219PanelOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;
    std::vector<cv::Rect> covered_regions() const override;

private:
    Config                                       cfg_;
    Max7219GpioBus                               gpio_bus_;   // only used when ≥1 chain is Gpio
    std::vector<std::unique_ptr<Max7219Chain>>   chains_;
};

} // namespace face
