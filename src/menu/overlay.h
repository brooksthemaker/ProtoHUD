#pragma once
// ── overlay.h ─────────────────────────────────────────────────────────────────
// Common interface for MenuSystem's full-screen overlays (file picker, face
// editor, color picker). While an overlay is open MenuSystem routes
// navigate/select/back to step/activate/back and draw_fullscreen hands the
// frame to draw() — one dispatch point instead of per-class if-chains.

#include <imgui.h>

namespace menu {

class IOverlay {
public:
    virtual ~IOverlay() = default;

    virtual bool is_open() const = 0;
    virtual void close() = 0;

    virtual void step(int d) = 0;             // knob rotate / vertical walk
    // D-pad 2D move; default maps dy to step() and ignores dx.
    virtual void move(int dx, int dy) { (void)dx; if (dy != 0) step(dy); }
    virtual void activate() = 0;              // select / enter
    virtual void back() = 0;                  // back / cancel-or-backspace

    // Full-screen draw; also polls its own extra ImGui keys if it has any.
    virtual void draw(ImDrawList* dl, ImFont* font, float fs, float W, float H,
                      ImU32 accent) = 0;
};

} // namespace menu
