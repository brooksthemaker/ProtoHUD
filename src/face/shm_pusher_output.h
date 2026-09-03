#pragma once
// ── shm_pusher_output.h ────────────────────────────────────────────────────────
// PanelOutput that writes the rendered RGB canvas to a POSIX shared-memory
// segment in the exact format ProtoHUD's ShmFrameReader already understands:
//   byte 0        uint8 sequence counter (wraps at 256)
//   bytes 1..N    W×H RGB, row-major (R G B ...)
// A tiny companion Python script (scripts/panel_driver.py) reads this and calls
// Piomatter.show(), keeping the proven driver while ProtoHUD owns the rendering.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
    //
    // `rect` is where the panel sits on the RENDERER CANVAS — i.e. where it sits
    // on the helmet, gaps and all. `dst` is where its pixels sit in the physical
    // framebuffer, where the panels are butted together in chain order. They're
    // identical until the user nudges panels apart; from then on show() gathers
    // rect → dst so each panel keeps displaying the slice of the face that lines
    // up with its real-world position. A default-constructed (empty) dst means
    // "no mapping known" and falls back to pushing the canvas verbatim.
    // `angle` (degrees) tilts the slice this panel reads out of the canvas, for
    // a panel mounted a few degrees off square: the sample is taken along a
    // rotated rect about rect's centre, so a line crossing the seam stays
    // straight in the real world instead of kinking at the panel edge. Zero
    // takes the plain rect copy path.
    struct Panel {
        std::string name;
        cv::Rect    rect;
        cv::Rect    dst;
        double      angle  = 0.0;
        bool        flip_x = false;   // applied to the sampled tile, not the canvas
        bool        flip_y = false;
    };

    // A row of panels flipped as one strip (0 = top, 1 = bottom). `rect` is in
    // FRAMEBUFFER space — the panels butted together in chain order — because
    // "this row is mounted rotated" is a fact about the physical panels, not
    // about where they sit on the helmet.
    struct Half {
        cv::Rect rect;
        bool     flip_x = false;
        bool     flip_y = false;
    };

    // width/height are the PHYSICAL framebuffer dimensions — what panel_driver.py
    // hands to piomatter — which is not the canvas size once panels are nudged
    // apart. The canvas arrives in show().
    explicit ShmPusherOutput(int width = 128, int height = 32,
                             std::vector<Panel> panels = {},
                             std::vector<Half> halves = {},
                             std::string path = "/dev/shm/protoface_frame");

    // Whole-output mirror for a panel set mounted rotated/mirrored as a unit.
    // Applied to the assembled framebuffer (after the gather), so it maps whole
    // panels onto each other the way a 180° mount physically does.
    void set_output_flip(bool flip_x, bool flip_y) override {
        flip_x_ = flip_x;
        flip_y_ = flip_y;
    }

    // Live mounting adjustments. Guarded because show() reads the panel list on
    // the render thread while the menu writes here.
    void set_panel_angles(const std::vector<double>& angles) override;
    // Nearest-neighbour instead of bilinear for the rotated sample. Bilinear
    // is the better default — it anti-aliases the diagonal edges a few degrees
    // of correction introduces — but it also softens every hard-edged pixel on
    // that panel, which is very visible on text. Nearest keeps glyphs exact at
    // the cost of stair-stepping the face.
    void set_sharp_rotation(bool on) override { sharp_rot_ = on; }
    void set_panel_flips(const std::vector<std::array<bool, 2>>& flips) override;
    void set_half_flips(const std::vector<std::array<bool, 2>>& halves) override;
    ~ShmPusherOutput() override;

    bool open() override;
    void show(const cv::Mat& rgb) override;
    void close() override;
    std::vector<cv::Rect>    covered_regions()       const override;
    std::vector<NamedRegion> covered_named_regions() const override;
    bool supports_face_editor() const override { return !panels_.empty(); }

private:
    // True when every panel carries a dst slot, i.e. show() can assemble the
    // framebuffer panel-by-panel instead of pushing the canvas verbatim.
    bool has_panel_map() const;

    int                w_, h_;
    // Read by show() on the render thread, written by set_panel_angles() from
    // the menu thread. Uncontended in practice — one short lock per frame.
    mutable std::mutex panels_mtx_;
    std::vector<Panel> panels_;
    std::vector<Half>  halves_;
    std::string        path_;
    int                fd_   = -1;
    uint8_t*           map_  = nullptr;
    size_t             size_ = 0;
    uint8_t            seq_  = 0;
    std::atomic<bool>  flip_x_{false};
    std::atomic<bool>  flip_y_{false};
    std::atomic<bool>  sharp_rot_{false};
    cv::Mat            fb_;    // scratch framebuffer, reused across frames
};

} // namespace face
