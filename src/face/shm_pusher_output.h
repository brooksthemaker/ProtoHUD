#pragma once
// ── shm_pusher_output.h ────────────────────────────────────────────────────────
// PanelOutput that writes the rendered RGB canvas to a POSIX shared-memory
// segment in the exact format ProtoHUD's ShmFrameReader already understands:
//   byte 0        uint8 sequence counter (wraps at 256)
//   bytes 1..N    W×H RGB, row-major (R G B ...)
// A tiny companion Python script (scripts/panel_driver.py) reads this and calls
// Piomatter.show(), keeping the proven driver while ProtoHUD owns the rendering.

#include <cstdint>
#include <string>
#include <vector>

#include "panel_output.h"

namespace face {

class ShmPusherOutput : public PanelOutput {
public:
    // Optional panel inventory used by the in-HUD face editor: when populated
    // the editor knows which canvas rects correspond to physical HUB75 panels
    // (so it can outline them and write face PNGs sized to the panel set).
    // Empty = legacy daemon-mode behaviour, editor stays hidden.
    struct Panel {
        std::string name;
        cv::Rect    rect;
    };

    explicit ShmPusherOutput(int width = 128, int height = 32,
                             std::vector<Panel> panels = {},
                             std::string path = "/dev/shm/protoface_frame");
    ~ShmPusherOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;
    std::vector<cv::Rect>    covered_regions()       const override;
    std::vector<NamedRegion> covered_named_regions() const override;
    bool supports_face_editor() const override { return !panels_.empty(); }

private:
    int                w_, h_;
    std::vector<Panel> panels_;
    std::string        path_;
    int                fd_   = -1;
    uint8_t*           map_  = nullptr;
    size_t             size_ = 0;
    uint8_t            seq_  = 0;
};

} // namespace face
