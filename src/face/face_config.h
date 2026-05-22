#pragma once
// ── face_config.h ──────────────────────────────────────────────────────────────
// Plain config structs for the native face renderer (a C++ port of the
// Protoface Python daemon). main.cpp builds these from config.json's
// "protoface" section; the renderer library itself is config-format agnostic.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace face {

struct WiggleCfg {
    double speed       = 0.3;   // oscillation rate (Hz-ish)
    double amplitude_x = 2.0;   // px
    double amplitude_y = 1.0;   // px
};

struct BlinkCfg {
    double interval_min = 3.0;  // s between blinks (min)
    double interval_max = 7.0;  // s between blinks (max)
    double duration     = 0.15; // s for a full close+open
};

struct FaceCfg {
    std::string active          = "main";  // faces/<active>/ folder
    WiggleCfg   wiggle;
    BlinkCfg    blink;
    double      expression_fade = 0.3;     // crossfade seconds
    double      mouth_sensitivity = 0.5;
};

struct MaterialCfg {
    std::string active   = "teal";  // "teal" | "solid:r,g,b" | "zone:a|b"
    double      scroll_x = 0.0;     // px/s
    double      scroll_y = 0.0;
};

// One physical panel region inside the logical canvas.
struct PanelCfg {
    std::string name;
    int         x = 0, y = 0, w = 64, h = 32;   // region in canvas pixels
    std::string mirror_of;                       // empty = renders its own face
    FaceCfg     face;
    MaterialCfg material;
    std::string particles = "none";              // effect/preset shorthand (Phase 3)
};

// Top-level renderer config (mirror of Protoface config.yaml panel/display).
struct RenderConfig {
    int                  canvas_w   = 128;
    int                  canvas_h   = 32;
    int                  fps        = 30;
    std::array<uint8_t,3> background = {0, 0, 0};   // RGB
    std::string          faces_dir     = "faces";
    std::string          materials_dir = "materials";
    std::string          gifs_dir      = "gifs";
    std::vector<PanelCfg> panels;
};

} // namespace face
