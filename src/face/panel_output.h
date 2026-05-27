#pragma once
// ── panel_output.h ─────────────────────────────────────────────────────────────
// Swappable LED-panel sink for the native face renderer. The renderer produces a
// canvas-sized RGB cv::Mat each frame and hands it to a PanelOutput. Two impls:
//   • ShmPusherOutput  — writes the frame to /dev/shm; a tiny Python shim calls
//                        Piomatter.show() (the safe, proven path).
//   • PiomatterOutput  — (future) drives the panels directly via a vendored
//                        Piomatter C++ core, no Python.

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

    // True if this backend has a sensible pixel grid the editor can target.
    // Decouples editor availability from whether the user has configured
    // chains yet — Max7219 + RGB-matrix backends always return true; HUB75
    // (no per-pixel addressable concept at this layer) stays false.
    virtual bool supports_face_editor() const { return false; }
};

} // namespace face
