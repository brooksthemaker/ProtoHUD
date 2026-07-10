#pragma once
// ── scroll_text.h ─────────────────────────────────────────────────────────────
// Scrolling text banner across the composited face canvas (marquee). Reuses the
// 5×7 bitmap font from max_section_content (draw_text / text_width), upscaled
// by an integer factor and tinted, sliding right→left across the full canvas —
// so it spans every panel the canvas covers, mirrored halves included, and
// rides above the face/effects/glitch layers.
//
// Threading: same contract as GlitchEffect — one instance owned and driven by
// the NativeFaceController render thread (tick + render); the config is set
// from other threads through set_config(), copied under an internal mutex.

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace face {

// How the banner moves.
enum class ScrollMode : uint8_t {
    Left   = 0,   // scroll right→left (classic marquee)
    Right  = 1,   // scroll left→right
    Static = 2,   // centred, no motion (a fixed label)
    Bounce = 3,   // ping-pong between the edges
};
// Vertical placement of the band.
enum class TextVPos : uint8_t { Center = 0, Top = 1, Bottom = 2 };

struct ScrollTextConfig {
    bool        enabled    = false;
    std::string text;                  // glyphs the 5×7 font lacks render blank
    double      speed_px_s = 24.0;     // scroll speed, canvas px/s
    int         scale      = 2;        // integer font upscale (2 → 10×14 glyphs)
    int         y          = -1;       // legacy top row; superseded by vpos
    uint8_t     r = 255, g = 255, b = 255;
    bool        loop       = true;     // false = one pass, then auto-disables
    ScrollMode  mode       = ScrollMode::Left;
    TextVPos    vpos       = TextVPos::Center;
    bool        bold       = false;    // dilate glyphs one px for a heavier stroke
    bool        bg         = false;    // dark band behind the text for contrast
    uint8_t     bg_alpha   = 150;      // band opacity when bg is on

    nlohmann::json to_json() const;
    static ScrollTextConfig from_json(const nlohmann::json& j);
};

class ScrollText {
public:
    // Restarts the pass when the text/scale changes; speed/color apply live.
    void set_config(const ScrollTextConfig& c);
    ScrollTextConfig config() const;

    bool active() const;               // enabled and (looping or mid-pass)

    // Render-thread only.
    void tick(double dt);
    void render(cv::Mat& canvas);      // canvas: composited CV_8UC3 face frame

private:
    void rebuild_strip_locked();       // rasterise text → tinted strip + mask

    mutable std::mutex mtx_;
    ScrollTextConfig   cfg_;
    double             offset_px_ = 0.0;   // distance scrolled this pass
    bool               done_      = false; // one-pass mode finished

    // Cached rasterisation of cfg_.text at cfg_.scale, rebuilt on change.
    cv::Mat strip_;        // CV_8UC3, tinted glyph pixels on black
    cv::Mat strip_mask_;   // CV_8U, 255 where a glyph pixel is lit
};

}  // namespace face
