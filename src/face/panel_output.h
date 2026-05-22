#pragma once
// ── panel_output.h ─────────────────────────────────────────────────────────────
// Swappable LED-panel sink for the native face renderer. The renderer produces a
// canvas-sized RGB cv::Mat each frame and hands it to a PanelOutput. Two impls:
//   • ShmPusherOutput  — writes the frame to /dev/shm; a tiny Python shim calls
//                        Piomatter.show() (the safe, proven path).
//   • PiomatterOutput  — (future) drives the panels directly via a vendored
//                        Piomatter C++ core, no Python.

#include <opencv2/core.hpp>

namespace face {

class PanelOutput {
public:
    virtual ~PanelOutput() = default;
    virtual bool open() { return true; }
    // Push an (h, w) CV_8UC3 RGB canvas to the panels.
    virtual void show(const cv::Mat& rgb) = 0;
    // Blank the panels (best effort) on shutdown.
    virtual void close() {}
};

} // namespace face
