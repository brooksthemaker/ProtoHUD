#pragma once
// ── max7219_panel_output.h ────────────────────────────────────────────────────
// PanelOutput implementation for MAX7219 8×8 LED matrix daisy-chains. Owns
// one or more Max7219Chain instances — one per functional zone of the face
// (left eye / right eye / nose / mouth) — each with its own SPI bus, grid
// dimensions, and canvas region. show() distributes the renderer's RGB
// canvas across them.

#include "panel_output.h"
#include "max7219_chain.h"

#include <memory>
#include <vector>

namespace face {

class Max7219PanelOutput : public PanelOutput {
public:
    struct Config {
        std::vector<Max7219Chain::Config> chains;
    };

    explicit Max7219PanelOutput(Config cfg);
    ~Max7219PanelOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;

private:
    Config                                       cfg_;
    std::vector<std::unique_ptr<Max7219Chain>>   chains_;
};

} // namespace face
