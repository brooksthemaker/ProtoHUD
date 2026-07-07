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
