#pragma once
// ── panel_output.h ─────────────────────────────────────────────────────────────
// Swappable LED-panel sink for the native face renderer. The renderer produces a
// canvas-sized RGB cv::Mat each frame and hands it to a PanelOutput. Two impls:
//   • ShmPusherOutput  — writes the frame to /dev/shm; a tiny Python shim calls
//                        Piomatter.show() (the safe, proven path).
//   • PiomatterOutput  — (future) drives the panels directly via a vendored
//                        Piomatter C++ core, no Python.

#include <array>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace face {

// A covered sub-rectangle of the renderer canvas, carrying the chain's
// configured name ("eye_l" / "eye_r" / "nose" / "mouth" …) so the editor
// can label each zone for the user.
struct NamedRegion {
    std::string name;
    cv::Rect    rect;
};

class PanelOutput {
public:
    virtual ~PanelOutput() = default;
    virtual bool open() { return true; }
    // Push an (h, w) CV_8UC3 RGB canvas to the panels.
    virtual void show(const cv::Mat& rgb) = 0;
    // Blank the panels (best effort) on shutdown.
    virtual void close() {}

    // Sub-rectangles of the renderer canvas that this backend actually
    // lights up. The face editor unions these to pick its editable region
    // and grays out anything not covered. Empty (the ShmPusherOutput
    // default) means "I don't know — the whole canvas may be shown" and
    // the editor stays hidden for that backend.
    virtual std::vector<cv::Rect> covered_regions() const { return {}; }

    // Same rects as covered_regions() but with each chain's configured
    // name. The editor uses these to label the eye / nose / mouth zones
    // inside its grid. Default returns empty; override in any backend
    // that wants editor labels.
    virtual std::vector<NamedRegion> covered_named_regions() const { return {}; }

    // Mirror the whole physical output — for a panel set mounted rotated or
    // mirrored as a unit. Done here rather than on the canvas so it maps whole
    // panels onto each other (the way a 180° mount does) and so the in-HUD
    // preview, which reads the canvas, stays the right way up. Backends that
    // can't express it ignore it.
    virtual void set_output_flip(bool /*flip_x*/, bool /*flip_y*/) {}

    // Per-panel mounting rotation in degrees, angles[i] for panel i. Live so
    // the setup slider can be turned against the panels; the canvas box that
    // feeds the tilted sample only grows on a layout rebuild, so a large angle
    // may read black at the corners until the layout is re-applied. Backends
    // without a panel inventory ignore it.
    virtual void set_panel_angles(const std::vector<double>& /*angles*/) {}

    // Mounting flips, applied to the physical output rather than to the canvas.
    // flips[i] = {flip_x, flip_y} for panel i; halves[j] = the same for half j
    // (0 = top, 1 = bottom) flipped as one strip. These belong here, not in the
    // renderer: the canvas is helmet space, and a rotated panel samples past its
    // own rect, so a flip applied to a canvas region would leak into whatever
    // neighbour a tilted edge reaches into.
    virtual void set_panel_flips(const std::vector<std::array<bool, 2>>& /*flips*/) {}
    virtual void set_half_flips (const std::vector<std::array<bool, 2>>& /*halves*/) {}

    // Sample rotated panels with nearest-neighbour rather than bilinear —
    // exact pixels (crisp text) at the cost of stair-stepped diagonals.
    virtual void set_sharp_rotation(bool /*on*/) {}

    // True if this backend has a sensible pixel grid the editor can target.
    // Decouples editor availability from whether the user has configured
    // chains yet — Max7219 + RGB-matrix backends always return true; HUB75
    // (no per-pixel addressable concept at this layer) stays false.
    virtual bool supports_face_editor() const { return false; }
};

} // namespace face
