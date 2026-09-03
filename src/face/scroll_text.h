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

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace face {

// Most lines a hand-typed message may hold. Enforced at ENTRY (the on-screen
// keyboard and the Add Line Break row), deliberately NOT in the renderer — the
// diagnostics readout is assembled with '|' breaks of its own and must stay
// free to grow as fields are toggled on.
inline constexpr size_t kScrollTextMaxLines = 5;

// How the banner moves.
enum class ScrollMode : uint8_t {
    Left   = 0,   // scroll right→left (classic marquee)
    Right  = 1,   // scroll left→right
    Static = 2,   // no motion (a fixed label)
    Bounce = 3,   // ping-pong between the edges
    Blink  = 4,   // fixed label, flashing on/off at blink_hz
    Wipe   = 5,   // typewriter: revealed left→right, held, then repeated
    Up     = 6,   // scroll bottom→top (credits roll; suits stacked lines)
    Down   = 7,   // scroll top→bottom
};
// Vertical placement of the band.
enum class TextVPos : uint8_t { Center = 0, Top = 1, Bottom = 2 };
// Horizontal placement for the modes that don't move (Static / Blink / Wipe).
enum class TextHAlign : uint8_t { Center = 0, Left = 1, Right = 2 };
// Which bitmap font the banner rasterises through. Small trades legibility for
// density — see tiny_font.h. Size (`scale`) multiplies whichever is picked, so
// Small at 2x is bigger than Standard at 1x but a different shape.
enum class TextFont : uint8_t { Standard = 0, Small = 1 };

// Fields the system-diagnostics readout can include. The app assembles {diag}
// from whichever bits are set; the order here is the order they appear.
enum DiagField : uint32_t {
    DIAG_CPU    = 1u << 0,   // CPU load %
    DIAG_TEMP   = 1u << 1,   // CPU package temperature
    DIAG_RAM    = 1u << 2,   // RAM used / total
    DIAG_FPS    = 1u << 3,   // render frame rate
    DIAG_BATT   = 1u << 4,   // headset battery
    DIAG_UPTIME = 1u << 5,   // time since boot
    DIAG_IMU    = 1u << 6,   // which IMU is reporting
    DIAG_WIFI   = 1u << 7,
    DIAG_AUDIO  = 1u << 8,
    DIAG_CAM    = 1u << 9,
    DIAG_GPU    = 1u << 10,  // GPU load % + temperature
};
// Bit, menu label, and whether it starts a new stacked line.
struct DiagFieldInfo { uint32_t bit; const char* name; bool line_break; };
const std::vector<DiagFieldInfo>& diag_fields();

struct ScrollTextConfig {
    bool        enabled    = false;
    std::string text;                  // glyphs the 5×7 font lacks render blank
    double      speed_px_s = 24.0;     // scroll speed, canvas px/s
    int         scale      = 2;        // integer font upscale (2 → 10×14 glyphs)
    TextFont    font       = TextFont::Standard;
    int         y          = -1;       // legacy top row; superseded by vpos
    uint8_t     r = 255, g = 255, b = 255;
    bool        loop       = true;     // false = one pass, then auto-disables
    ScrollMode  mode       = ScrollMode::Left;
    TextVPos    vpos       = TextVPos::Center;
    bool        bold       = false;    // dilate glyphs one px for a heavier stroke
    bool        bg         = false;    // dark band behind the text for contrast
    uint8_t     bg_alpha   = 150;      // band opacity when bg is on
    TextHAlign  halign     = TextHAlign::Center;
    // Dark halo one pixel around every glyph. Keeps text legible over a busy
    // face without the full-width dark band `bg` paints, so the expression
    // stays visible either side of the message.
    bool        outline    = false;
    // Cycle the text through the hue wheel instead of holding one colour.
    // Ignores r/g/b while on; rainbow_speed is hue revolutions per second.
    bool        rainbow    = false;
    double      rainbow_speed = 0.35;
    double      blink_hz   = 1.5;      // Blink mode flash rate
    // Wipe mode: seconds the fully-revealed message is held before repeating.
    double      wipe_hold_s = 1.5;
    // Fine placement, in canvas pixels, on top of whatever vpos/halign chose.
    int         off_x      = 0;
    int         off_y      = 0;
    // Window: the sub-rectangle of the canvas the banner lives in. Everything —
    // alignment, scroll travel, the bg band, the final blit — is measured
    // against this rather than the whole face, so a marquee can be confined to
    // one panel or a strip across the mouth instead of running the full width.
    // Disabled (the default) means the whole canvas. w/h of 0 mean "to the
    // edge", so a window can be set by its origin alone.
    bool        win_enabled = false;
    int         win_x = 0, win_y = 0, win_w = 0, win_h = 0;
    // Diagnostics mode: show a live system readout instead of `text`, without
    // disturbing the user's own message — it comes straight back when this goes
    // off. Every other setting (motion, colour, placement, window) still
    // applies, so the readout can be a ticker across the mouth or a fixed label
    // on one panel. The readout itself is assembled by the app and arrives as
    // the {diag} token.
    bool        diag       = false;
    // Which fields the diagnostics readout includes (bitmask of DiagField).
    // Defaults to everything; the app builds {diag} from whatever is set, and
    // drops the line break with the field so turning things off doesn't leave
    // blank rows behind.
    uint32_t    diag_fields = 0xFFFFFFFFu;
    // Stack the message over several lines instead of running it out as one
    // row. '|' in the text is the line break: with stacking off it renders as a
    // gap, so a single string reads correctly either way and the diagnostics
    // readout doesn't need two formats. Lines are aligned by `halign` within
    // the block, and the block as a whole is placed by vpos/off_y as before.
    bool        stack      = false;
    // Blank rows between stacked lines, in unscaled font pixels — so it grows
    // with Size, the same way the glyphs do.
    int         line_gap   = 1;

    nlohmann::json to_json() const;
    static ScrollTextConfig from_json(const nlohmann::json& j);
};

// One event-driven message. Carries a WHOLE ScrollTextConfig rather than a
// trimmed "look", so an event owns its text and every banner property — font,
// colour, motion, placement, window — and the same menu builders edit it.
//
// The trigger itself is NOT stored here: it lives in the shared trigger map
// under "textev_<n>" (see kTextEventKeyPrefix in custom_expression.h), so the
// recipe editor and the whole event pipeline are reused unchanged.
struct TextEvent {
    bool             used = false;   // false = empty slot, hidden from the menu
    std::string      name;           // menu label, e.g. "Boot greeting"
    ScrollTextConfig cfg;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["used"] = used;
        j["name"] = name;
        j["cfg"]  = cfg.to_json();
        return j;
    }
    static TextEvent from_json(const nlohmann::json& j) {
        TextEvent e;
        if (!j.is_object()) return e;
        e.used = j.value("used", false);
        e.name = j.value("name", std::string());
        if (j.contains("cfg")) e.cfg = ScrollTextConfig::from_json(j["cfg"]);
        return e;
    }
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

    // Live values for {token} placeholders in the text — {time}, {batt} and so
    // on. Pushed in from the app rather than pulled from here: resolving them
    // on the render thread would mean reaching into AppState's lock from inside
    // this one. The strip is re-rasterised only when a substitution actually
    // changes the rendered string, so a {time} banner rebuilds once a minute,
    // not once a frame. Unknown tokens are left as literal text so a typo is
    // visible on the panels instead of silently vanishing.
    void set_tokens(std::map<std::string, std::string> values);
    // Token names this build substitutes, for the menu's help text.
    static const std::vector<std::string>& token_names();

private:
    void rebuild_strip_locked();       // rasterise text → tinted strip + mask
    void retint_locked();              // repaint strip_ from strip_mask_
    std::string expand_locked() const; // cfg_.text with {tokens} substituted

    mutable std::mutex mtx_;
    ScrollTextConfig   cfg_;
    double             offset_px_ = 0.0;   // distance scrolled this pass
    double             t_         = 0.0;   // seconds since the last restart
    bool               done_      = false; // one-pass mode finished

    std::map<std::string, std::string> tokens_;
    std::string expanded_;   // cfg_.text after substitution — what strip_ shows

    // Cached rasterisation of `expanded_` at cfg_.scale, rebuilt on change.
    cv::Mat strip_;          // CV_8UC3, tinted glyph pixels on black
    cv::Mat strip_mask_;     // CV_8U, 255 where a glyph pixel is lit
    cv::Mat outline_mask_;   // CV_8U, 255 on the halo ring (outline only)
};

}  // namespace face
