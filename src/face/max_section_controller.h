#pragma once
// ── max_section_controller.h ─────────────────────────────────────────────────
// Drives the MAX7219 "section" panels with independent, triggerable content —
// a symbol, a short text string, or a generated pattern — rendered by
// max_section_content and pushed over the coproc SPI bridge (its PanelOutput).
// Independent of the face renderer: the HUB75 face keeps running; these panels
// show whatever was last triggered (via a max_* GpioFunc, a menu item, or a
// `max_symbol:`/`max_text:`/`max_pattern:` command on the input FIFO).
//
// A light refresh thread re-pushes at a few fps so animated patterns move and a
// dropped SPI frame self-heals (the MAX chips otherwise hold their last frame).

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

#include "panel_output.h"

namespace face {

class MaxSectionController {
public:
    MaxSectionController(std::unique_ptr<PanelOutput> out, int canvas_w, int canvas_h);
    ~MaxSectionController();

    bool start();   // open the output + start the refresh thread
    void stop();

    // Content setters (thread-safe). Each replaces what's shown.
    void set_symbol (const std::string& name);
    void set_text   (const std::string& text);
    void set_pattern(const std::string& name);
    void clear();
    void cycle(int dir);   // step through the built-in symbols (+1 / -1)

    std::string current() const;   // "kind:value" for the menu label

private:
    void render_locked();          // rebuild canvas_ from the current state (holds mtx_)
    void loop();                   // refresh thread body

    std::unique_ptr<PanelOutput> out_;
    cv::Mat                      canvas_;      // section-local RGB (CV_8UC3)
    mutable std::mutex           mtx_;
    std::string                  kind_  = "blank";   // symbol | text | pattern | blank
    std::string                  value_;
    int                          sym_idx_ = 0;
    std::atomic<int>             phase_{0};
    std::atomic<bool>            dirty_{true};
    std::atomic<bool>            running_{false};
    std::thread                  thread_;
};

}  // namespace face
