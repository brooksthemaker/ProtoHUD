#pragma once
// ── tee_panel_output.h ───────────────────────────────────────────────────────
// A PanelOutput that fans one rendered canvas out to several backends, so e.g. a
// HUB75 face and a coproc-driven MAX7219 "section" run from the same renderer at
// the same time. The face renderer stays single-output; this just multiplexes.

#include "panel_output.h"

#include <memory>
#include <utility>
#include <vector>

namespace face {

class TeePanelOutput : public PanelOutput {
public:
    explicit TeePanelOutput(std::vector<std::unique_ptr<PanelOutput>> outs)
        : outs_(std::move(outs)) {}

    // Open all; succeed if ANY came up — a dead MAX chain must not take the
    // HUB75 face down with it.
    bool open() override {
        bool any = false;
        for (auto& o : outs_) if (o && o->open()) any = true;
        return any;
    }
    void show(const cv::Mat& rgb) override {
        for (auto& o : outs_) if (o) o->show(rgb);
    }
    void close() override {
        for (auto& o : outs_) if (o) o->close();
    }
    void set_output_flip(bool flip_x, bool flip_y) override {
        for (auto& o : outs_) if (o) o->set_output_flip(flip_x, flip_y);
    }
    void set_panel_angles(const std::vector<double>& angles) override {
        for (auto& o : outs_) if (o) o->set_panel_angles(angles);
    }
    void set_panel_flips(const std::vector<std::array<bool, 2>>& flips) override {
        for (auto& o : outs_) if (o) o->set_panel_flips(flips);
    }
    void set_half_flips(const std::vector<std::array<bool, 2>>& halves) override {
        for (auto& o : outs_) if (o) o->set_half_flips(halves);
    }
    void set_sharp_rotation(bool on) override {
        for (auto& o : outs_) if (o) o->set_sharp_rotation(on);
    }

    std::vector<cv::Rect> covered_regions() const override {
        std::vector<cv::Rect> all;
        for (const auto& o : outs_) if (o) {
            const auto r = o->covered_regions();
            all.insert(all.end(), r.begin(), r.end());
        }
        return all;
    }
    std::vector<NamedRegion> covered_named_regions() const override {
        std::vector<NamedRegion> all;
        for (const auto& o : outs_) if (o) {
            const auto r = o->covered_named_regions();
            all.insert(all.end(), r.begin(), r.end());
        }
        return all;
    }
    bool supports_face_editor() const override {
        for (const auto& o : outs_) if (o && o->supports_face_editor()) return true;
        return false;
    }

private:
    std::vector<std::unique_ptr<PanelOutput>> outs_;
};

} // namespace face
