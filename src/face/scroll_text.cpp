#include "scroll_text.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>

#include "max_section_content.h"
#include "tiny_font.h"

namespace face {

namespace {
const char* mode_str(ScrollMode m) {
    switch (m) {
        case ScrollMode::Right:  return "right";
        case ScrollMode::Static: return "static";
        case ScrollMode::Bounce: return "bounce";
        case ScrollMode::Blink:  return "blink";
        case ScrollMode::Wipe:   return "wipe";
        case ScrollMode::Up:     return "up";
        case ScrollMode::Down:   return "down";
        case ScrollMode::Left:   default: return "left";
    }
}
ScrollMode mode_from(const std::string& s) {
    if (s == "right")  return ScrollMode::Right;
    if (s == "static") return ScrollMode::Static;
    if (s == "bounce") return ScrollMode::Bounce;
    if (s == "blink")  return ScrollMode::Blink;
    if (s == "wipe")   return ScrollMode::Wipe;
    if (s == "up")     return ScrollMode::Up;
    if (s == "down")   return ScrollMode::Down;
    return ScrollMode::Left;
}
const char* halign_str(TextHAlign h) {
    switch (h) {
        case TextHAlign::Left:  return "left";
        case TextHAlign::Right: return "right";
        case TextHAlign::Center: default: return "center";
    }
}
TextHAlign halign_from(const std::string& s) {
    if (s == "left")  return TextHAlign::Left;
    if (s == "right") return TextHAlign::Right;
    return TextHAlign::Center;
}
const char* font_str(TextFont f) {
    return f == TextFont::Small ? "small" : "standard";
}
TextFont font_from(const std::string& s) {
    return s == "small" ? TextFont::Small : TextFont::Standard;
}

// The two bitmap fonts behind one interface, so the rasteriser below doesn't
// branch on the font at every step.
int font_height(TextFont f) {
    return f == TextFont::Small ? tiny_font::kH : 7;
}
int font_text_width(TextFont f, const std::string& s) {
    return f == TextFont::Small ? tiny_font::text_width(s)
                                : max_content::text_width(s);
}
void font_draw(TextFont f, cv::Mat& m, const std::string& s, int x, int y) {
    if (f == TextFont::Small) tiny_font::draw_text(m, s, x, y);
    else                      max_content::draw_text(m, s, x, y);
}

// Hue (0..1) → RGB at full saturation/value. Small local helper so the rainbow
// doesn't drag in a cvtColor round-trip for one pixel's worth of maths.
cv::Scalar hue_rgb(double h) {
    h -= std::floor(h);
    const double s = h * 6.0;
    const int    i = static_cast<int>(s) % 6;
    const double f = s - std::floor(s);
    const double q = 1.0 - f;
    double r = 0, g = 0, b = 0;
    switch (i) {
        case 0: r = 1; g = f; break;
        case 1: r = q; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = q; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1;  b = q; break;
    }
    return cv::Scalar(r * 255.0, g * 255.0, b * 255.0);
}
const char* vpos_str(TextVPos v) {
    switch (v) {
        case TextVPos::Top:    return "top";
        case TextVPos::Bottom: return "bottom";
        case TextVPos::Center: default: return "center";
    }
}
TextVPos vpos_from(const std::string& s) {
    if (s == "top")    return TextVPos::Top;
    if (s == "bottom") return TextVPos::Bottom;
    return TextVPos::Center;
}
}  // namespace

// Field order for the diagnostics readout, and where the stacked version wants
// its line breaks. Pairs that read well together (CPU + temp, FPS + battery)
// share a row; the rest get their own.
const std::vector<DiagFieldInfo>& diag_fields() {
    static const std::vector<DiagFieldInfo> kFields = {
        {DIAG_CPU,    "CPU Load",        true },
        {DIAG_TEMP,   "CPU Temperature", false},
        {DIAG_GPU,    "GPU Load",        true },
        {DIAG_RAM,    "RAM",             true },
        {DIAG_FPS,    "Frame Rate",      true },
        {DIAG_BATT,   "Battery",         false},
        {DIAG_UPTIME, "Uptime",          true },
        {DIAG_IMU,    "IMU Source",      false},
        {DIAG_WIFI,   "Wi-Fi",           true },
        {DIAG_AUDIO,  "Audio",           false},
        {DIAG_CAM,    "Cameras",         false},
    };
    return kFields;
}

nlohmann::json ScrollTextConfig::to_json() const {
    return {
        {"enabled",    enabled},
        {"text",       text},
        {"speed_px_s", speed_px_s},
        {"scale",      scale},
        {"y",          y},
        {"color",      {r, g, b}},
        {"loop",       loop},
        {"mode",       mode_str(mode)},
        {"vpos",       vpos_str(vpos)},
        {"bold",       bold},
        {"bg",         bg},
        {"bg_alpha",   bg_alpha},
        {"halign",        halign_str(halign)},
        {"font",          font_str(font)},
        {"outline",       outline},
        {"rainbow",       rainbow},
        {"rainbow_speed", rainbow_speed},
        {"blink_hz",      blink_hz},
        {"wipe_hold_s",   wipe_hold_s},
        {"off_x",         off_x},
        {"off_y",         off_y},
        {"win_enabled",   win_enabled},
        {"win",           {win_x, win_y, win_w, win_h}},
        {"diag",          diag},
        {"diag_fields",   diag_fields},
        {"stack",         stack},
        {"line_gap",      line_gap},
    };
}

ScrollTextConfig ScrollTextConfig::from_json(const nlohmann::json& j) {
    ScrollTextConfig c;
    c.enabled    = j.value("enabled",    c.enabled);
    c.text       = j.value("text",       c.text);
    c.speed_px_s = j.value("speed_px_s", c.speed_px_s);
    c.scale      = j.value("scale",      c.scale);
    c.y          = j.value("y",          c.y);
    c.loop       = j.value("loop",       c.loop);
    c.mode       = mode_from(j.value("mode", "left"));
    c.vpos       = vpos_from(j.value("vpos", "center"));
    c.bold       = j.value("bold",     c.bold);
    c.bg         = j.value("bg",       c.bg);
    c.bg_alpha   = j.value("bg_alpha", c.bg_alpha);
    c.halign        = halign_from(j.value("halign", "center"));
    c.font          = font_from(j.value("font", "standard"));
    c.outline       = j.value("outline",       c.outline);
    c.rainbow       = j.value("rainbow",       c.rainbow);
    c.rainbow_speed = j.value("rainbow_speed", c.rainbow_speed);
    c.blink_hz      = j.value("blink_hz",      c.blink_hz);
    c.wipe_hold_s   = j.value("wipe_hold_s",   c.wipe_hold_s);
    c.off_x         = j.value("off_x",         c.off_x);
    c.off_y         = j.value("off_y",         c.off_y);
    c.win_enabled   = j.value("win_enabled",   c.win_enabled);
    c.diag          = j.value("diag",          c.diag);
    c.diag_fields   = j.value("diag_fields",   c.diag_fields);
    c.stack         = j.value("stack",         c.stack);
    c.line_gap      = j.value("line_gap",      c.line_gap);
    if (j.contains("win") && j["win"].is_array() && j["win"].size() == 4) {
        c.win_x = j["win"][0].get<int>();
        c.win_y = j["win"][1].get<int>();
        c.win_w = j["win"][2].get<int>();
        c.win_h = j["win"][3].get<int>();
    }
    if (j.contains("color") && j["color"].is_array() && j["color"].size() == 3) {
        c.r = j["color"][0].get<uint8_t>();
        c.g = j["color"][1].get<uint8_t>();
        c.b = j["color"][2].get<uint8_t>();
    }
    return c;
}

void ScrollText::set_config(const ScrollTextConfig& c) {
    std::lock_guard<std::mutex> lk(mtx_);
    const bool restart = c.text != cfg_.text || c.scale != cfg_.scale ||
                         c.bold != cfg_.bold || c.mode != cfg_.mode ||
                         c.outline != cfg_.outline || c.diag != cfg_.diag ||
                         c.stack != cfg_.stack || c.line_gap != cfg_.line_gap ||
                         c.font != cfg_.font ||
                         c.halign != cfg_.halign ||   // stacked: bakes into the block
                         (c.enabled && !cfg_.enabled);
    const bool retint  = c.r != cfg_.r || c.g != cfg_.g || c.b != cfg_.b ||
                         c.rainbow != cfg_.rainbow;
    cfg_ = c;
    cfg_.scale = std::clamp(cfg_.scale, 1, 4);
    if (restart) {
        offset_px_ = 0.0;
        t_         = 0.0;
        done_      = false;
        rebuild_strip_locked();
    } else if (retint && !strip_mask_.empty()) {
        retint_locked();
    }
}

const std::vector<std::string>& ScrollText::token_names() {
    static const std::vector<std::string> kNames = {
        "time", "time24", "date", "day", "batt", "phone", "cpu",
        "cputemp", "temp", "weather", "heading", "compass", "expr", "name",
    };
    return kNames;
}

void ScrollText::set_tokens(std::map<std::string, std::string> values) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (values == tokens_) return;
    tokens_ = std::move(values);
    // Only rebuild when the substituted result actually changed — a {time}
    // banner then re-rasterises once a minute rather than every frame. The
    // scroll offset is deliberately left alone so a marquee doesn't jump back
    // to the start each time a value ticks over.
    if (expand_locked() != expanded_) rebuild_strip_locked();
}

std::string ScrollText::expand_locked() const {
    // Diagnostics mode swaps the source string, leaving cfg_.text untouched so
    // the user's message returns intact when they switch back.
    static const std::string kDiag = "{diag}";
    const std::string& in = cfg_.diag ? kDiag : cfg_.text;
    if (in.find('{') == std::string::npos) return in;
    std::string out;
    out.reserve(in.size() + 16);
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '{') { out += in[i++]; continue; }
        const size_t end = in.find('}', i + 1);
        if (end == std::string::npos) { out += in[i++]; continue; }
        const std::string key = in.substr(i + 1, end - i - 1);
        const auto it = tokens_.find(key);
        // Unknown token → leave the literal in place, so a typo shows up on
        // the panels instead of the message quietly losing a word.
        out += (it != tokens_.end()) ? it->second : in.substr(i, end - i + 1);
        i = end + 1;
    }
    return out;
}

ScrollTextConfig ScrollText::config() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

bool ScrollText::active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_.enabled && !done_ && !cfg_.text.empty();
}

void ScrollText::retint_locked() {
    if (strip_mask_.empty()) return;
    if (strip_.size() != strip_mask_.size() || strip_.type() != CV_8UC3)
        strip_ = cv::Mat(strip_mask_.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    else
        strip_.setTo(cv::Scalar(0, 0, 0));
    const cv::Scalar col = cfg_.rainbow
        ? hue_rgb(t_ * cfg_.rainbow_speed)
        : cv::Scalar(cfg_.r, cfg_.g, cfg_.b);
    strip_.setTo(col, strip_mask_);
}

void ScrollText::rebuild_strip_locked() {
    strip_.release();
    strip_mask_.release();
    outline_mask_.release();
    expanded_ = expand_locked();
    if (expanded_.empty()) return;

    // A real newline is the line break — that's what the keyboard now enters.
    // '|' is accepted as an equivalent, both for messages typed before the
    // keyboard could hold newlines and because the diagnostics readout is
    // assembled as one flat string. Stacked, each piece becomes its own row;
    // flat, either separator renders as a gap, so one string reads correctly
    // whichever way it's shown.
    auto is_break = [](char c) { return c == '\n' || c == '|'; };
    std::vector<std::string> lines;
    if (cfg_.stack) {
        std::string cur;
        for (char c : expanded_) {
            if (is_break(c)) { lines.push_back(cur); cur.clear(); }
            else             cur += c;
        }
        lines.push_back(cur);
    } else {
        std::string flat = expanded_;
        for (char& c : flat) if (is_break(c)) c = ' ';
        lines.push_back(std::move(flat));
    }

    const int fh = font_height(cfg_.font);
    int tw = 0;
    for (const auto& ln : lines)
        tw = std::max(tw, font_text_width(cfg_.font, ln));
    if (tw <= 0) return;

    // Rasterise at 1× through the shared 5×7 font (paints white on black),
    // then integer-upscale with nearest-neighbour so glyphs stay blocky, and
    // tint via the lit-pixel mask. Canvas order matches draw_text's (RGB).
    // One pixel of padding all round leaves the outline halo somewhere to go.
    const int pad  = cfg_.outline ? 1 : 0;
    const int gap  = std::clamp(cfg_.line_gap, 0, 8);
    const int n    = static_cast<int>(lines.size());
    const int th   = n * fh + (n - 1) * gap;
    cv::Mat mono(th, tw, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int i = 0; i < n; ++i) {
        // Each line is aligned inside the block by the same control that places
        // the block on the face, so a stacked readout reads as one column.
        const int lw = font_text_width(cfg_.font, lines[i]);
        int lx = 0;
        switch (cfg_.halign) {
        case TextHAlign::Left:  lx = 0; break;
        case TextHAlign::Right: lx = tw - lw; break;
        case TextHAlign::Center: default: lx = (tw - lw) / 2; break;
        }
        font_draw(cfg_.font, mono, lines[i], lx, i * (fh + gap));
    }
    if (cfg_.scale > 1)
        cv::resize(mono, mono, cv::Size(tw * cfg_.scale, th * cfg_.scale),
                   0, 0, cv::INTER_NEAREST);
    if (pad > 0)
        cv::copyMakeBorder(mono, mono, pad, pad, pad, pad,
                           cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    cv::cvtColor(mono, strip_mask_, cv::COLOR_RGB2GRAY);
    // Bold: grow the lit-pixel mask a step so strokes read heavier.
    if (cfg_.bold) {
        const int k = std::max(2, cfg_.scale);
        cv::dilate(strip_mask_, strip_mask_,
                   cv::getStructuringElement(cv::MORPH_RECT, {k, k}));
    }
    // Outline: the ring one pixel outside the glyphs. Painted black under the
    // text so the message reads over a busy face without the full-width dark
    // band `bg` uses — the expression stays visible either side of it.
    if (cfg_.outline) {
        cv::Mat grown;
        cv::dilate(strip_mask_, grown,
                   cv::getStructuringElement(cv::MORPH_RECT, {3, 3}));
        cv::subtract(grown, strip_mask_, outline_mask_);
    }
    retint_locked();
}

void ScrollText::tick(double dt) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty()) return;
    dt = std::max(0.0, dt);
    t_ += dt;
    // Rainbow repaints every frame; a fixed colour was baked in at rebuild.
    if (cfg_.rainbow) retint_locked();
    // Static and Blink hold position; Wipe and the scroll modes advance.
    if (cfg_.mode == ScrollMode::Static || cfg_.mode == ScrollMode::Blink) return;
    offset_px_ += cfg_.speed_px_s * dt;
}

void ScrollText::render(cv::Mat& full_canvas) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty() || full_canvas.empty()) return;

    // The banner lives inside a window on the canvas — the whole face by
    // default. Taking a view of it here means every measurement below (align,
    // scroll travel, bounce span, the bg band, the final clip) is against the
    // window without a second coordinate system to keep in step: writes to the
    // view land in the canvas underneath.
    cv::Rect win(0, 0, full_canvas.cols, full_canvas.rows);
    if (cfg_.win_enabled) {
        const int wx = std::clamp(cfg_.win_x, 0, full_canvas.cols - 1);
        const int wy = std::clamp(cfg_.win_y, 0, full_canvas.rows - 1);
        // 0 = run to the edge, so a window can be set by its origin alone.
        const int ww = (cfg_.win_w > 0) ? cfg_.win_w : full_canvas.cols - wx;
        const int wh = (cfg_.win_h > 0) ? cfg_.win_h : full_canvas.rows - wy;
        win = cv::Rect(wx, wy, ww, wh) &
              cv::Rect(0, 0, full_canvas.cols, full_canvas.rows);
        if (win.width <= 0 || win.height <= 0) return;
    }
    cv::Mat canvas = full_canvas(win);

    // Horizontal position per mode. Only the scroll modes have a "pass" that
    // can finish (loop / one-shot); static and bounce run continuously.
    int x = 0;
    switch (cfg_.mode) {
    case ScrollMode::Left: {
        const double travel = canvas.cols + strip_.cols;
        if (offset_px_ >= travel) {
            if (cfg_.loop) offset_px_ = std::fmod(offset_px_, travel);
            else { done_ = true; return; }
        }
        x = canvas.cols - static_cast<int>(offset_px_);
        break;
    }
    case ScrollMode::Right: {
        const double travel = canvas.cols + strip_.cols;
        if (offset_px_ >= travel) {
            if (cfg_.loop) offset_px_ = std::fmod(offset_px_, travel);
            else { done_ = true; return; }
        }
        x = static_cast<int>(offset_px_) - strip_.cols;
        break;
    }
    case ScrollMode::Static:
    case ScrollMode::Blink:
    case ScrollMode::Wipe:
    // Vertical scrolls take their horizontal placement from Align, the mirror
    // of how the horizontal scrolls take their vertical placement from Position.
    case ScrollMode::Up:
    case ScrollMode::Down:
        switch (cfg_.halign) {
        case TextHAlign::Left:  x = 0; break;
        case TextHAlign::Right: x = canvas.cols - strip_.cols; break;
        case TextHAlign::Center: default:
            x = (canvas.cols - strip_.cols) / 2; break;
        }
        break;
    case ScrollMode::Bounce: {
        const int    span   = canvas.cols - strip_.cols;   // <0 if text wider
        const double range  = std::abs(span);
        if (range < 1.0) { x = span / 2; break; }
        const double period = 2.0 * range;                 // there and back
        const double ph     = std::fmod(offset_px_, period);
        const double tri    = (ph <= range) ? ph : (period - ph);
        x = std::min(0, span) + static_cast<int>(tri);
        break;
    }
    }

    // Blink: a fixed label flashing on/off. Skipping the blit (rather than
    // dimming) keeps the face underneath fully visible on the off beat.
    if (cfg_.mode == ScrollMode::Blink) {
        const double hz = std::max(0.1, cfg_.blink_hz);
        if (std::fmod(t_ * hz, 1.0) >= 0.5) return;
    }

    // Wipe: reveal left→right at Speed, hold the whole message, then repeat
    // (or stop, when Loop is off). offset_px_ is the reveal front.
    int reveal_cols = strip_.cols;
    if (cfg_.mode == ScrollMode::Wipe) {
        const double hold_px = std::max(0.0, cfg_.wipe_hold_s) *
                               std::max(1.0, cfg_.speed_px_s);
        const double period  = strip_.cols + hold_px;
        if (offset_px_ >= period) {
            if (cfg_.loop) offset_px_ = std::fmod(offset_px_, period);
            else { done_ = true; return; }
        }
        reveal_cols = std::clamp(static_cast<int>(offset_px_), 0, strip_.cols);
        if (reveal_cols <= 0) return;
    }

    int y;
    if (cfg_.mode == ScrollMode::Up || cfg_.mode == ScrollMode::Down) {
        // Vertical marquee — the same travel/loop rule as the horizontal ones,
        // measured down the window instead of across it. Reads as a credits
        // roll, which is what makes it the natural pairing for stacked lines.
        const double travel = canvas.rows + strip_.rows;
        if (offset_px_ >= travel) {
            if (cfg_.loop) offset_px_ = std::fmod(offset_px_, travel);
            else { done_ = true; return; }
        }
        y = (cfg_.mode == ScrollMode::Up)
          ? canvas.rows - static_cast<int>(offset_px_)
          : static_cast<int>(offset_px_) - strip_.rows;
    } else {
        switch (cfg_.vpos) {
        case TextVPos::Top:    y = 0; break;
        case TextVPos::Bottom: y = std::max(0, canvas.rows - strip_.rows); break;
        case TextVPos::Center: default:
            y = std::max(0, (canvas.rows - strip_.rows) / 2); break;
        }
    }

    // Fine placement on top of the anchor the mode/vpos picked.
    x += cfg_.off_x;
    y += cfg_.off_y;

    // Optional dark band behind the text (full width, glyph-band height) so it
    // stays legible over a busy face.
    if (cfg_.bg) {
        const int by0 = std::clamp(y, 0, canvas.rows);
        const int by1 = std::clamp(y + strip_.rows, 0, canvas.rows);
        if (by1 > by0) {
            cv::Mat band = canvas(cv::Rect(0, by0, canvas.cols, by1 - by0));
            band.convertTo(band, -1, 1.0 - cfg_.bg_alpha / 255.0, 0.0);  // darken
        }
    }

    // Clip the strip to the canvas and blit only lit glyph pixels, leaving the
    // face visible between letters.
    const int sx0 = std::max(0, -x);
    const int sy0 = std::max(0, -y);
    const int dx0 = std::max(0, x);
    const int dy0 = std::max(0, y);
    int w = std::min(strip_.cols - sx0, canvas.cols - dx0);
    const int h = std::min(strip_.rows - sy0, canvas.rows - dy0);
    if (w <= 0 || h <= 0) return;
    // Wipe stops the blit at the reveal front.
    w = std::min(w, std::max(0, reveal_cols - sx0));
    if (w <= 0) return;

    const cv::Rect src_roi(sx0, sy0, w, h);
    cv::Mat dst = canvas(cv::Rect(dx0, dy0, w, h));
    // Halo first, then the glyphs over it.
    if (cfg_.outline && !outline_mask_.empty())
        cv::Mat(dst.size(), CV_8UC3, cv::Scalar(0, 0, 0))
            .copyTo(dst, outline_mask_(src_roi));
    strip_(src_roi).copyTo(dst, strip_mask_(src_roi));
}

}  // namespace face
