#pragma once
// ── face_config.h ──────────────────────────────────────────────────────────────
// Plain config structs for the native face renderer (a C++ port of the
// Protoface Python daemon). main.cpp builds these from config.json's
// "protoface" section; the renderer library itself is config-format agnostic.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
    // Per-panel orientation: flip the panel's composited region to match how
    // the physical HUB75 panel is mounted/wired. flip_x mirrors left-right,
    // flip_y top-bottom; both set = 180° rotation. Applied after compositing
    // (and after mirror copies) so it works for self-rendered and mirror panels.
    bool        flip_x = false;
    bool        flip_y = false;
    FaceCfg     face;
    MaterialCfg material;
    // Particle config in any form ParticleSystem accepts: a string effect name,
    // {"preset": ...}, {"effect": ...}, or {"layers": [...]}. Defaults to none.
    nlohmann::json particles = "none";
};

// Top-level renderer config (mirror of Protoface config.yaml panel/display).
struct RenderConfig {
    int                  canvas_w   = 128;
    int                  canvas_h   = 32;
    // 60 fps halves the visible motion step vs the old 30 — affordable now
    // that the compositor reuses its buffers instead of allocating per frame.
    // Override with protoface.fps in config.json.
    int                  fps        = 60;
    std::array<uint8_t,3> background = {0, 0, 0};   // RGB
    std::string          faces_dir     = "faces";
    std::string          materials_dir = "materials";
    std::string          gifs_dir      = "gifs";
    double               gif_auto_release = 5.0;   // s after play_gif before reverting (0 = loop forever)
    std::string          state_path;               // auto-saved live look (empty = disabled)
    // When false, the controller skips creating per-panel ParticleSystems and
    // makes set_effect() a no-op. Used by the MAX7219 / RGB-matrix backends
    // where particle effects don't read on a handful of 8x8 modules.
    bool                 effects_enabled = true;
    // When true, mirror_of panels are built as full self-rendering panels (their
    // own face + ParticleSystem) with the face layer flipped to preserve the
    // mirrored look — so canvas-space effects (water) render continuously across
    // both eyes instead of one eye being a flipped copy. Costs a second render.
    bool                 continuous_effects = false;
    std::vector<PanelCfg> panels;

    // Physical-panel output regions for a multi-panel face rendered as ONE
    // logical canvas-wide panel. When non-empty, the controller composites the
    // whole face once (so blink / material / effects are continuous across the
    // seam) and then flips each physical panel's slice in place at output time —
    // so per-panel mounting flips apply to everything, not just the face PNG.
    struct OutputPanel { int x = 0, y = 0, w = 64, h = 32; bool flip_x = false, flip_y = false; };
    std::vector<OutputPanel> output_panels;
};

} // namespace face
