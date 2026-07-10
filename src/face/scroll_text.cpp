#include "scroll_text.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>

#include "max_section_content.h"

namespace face {

namespace {
const char* mode_str(ScrollMode m) {
    switch (m) {
        case ScrollMode::Right:  return "right";
        case ScrollMode::Static: return "static";
        case ScrollMode::Bounce: return "bounce";
        case ScrollMode::Left:   default: return "left";
    }
}
ScrollMode mode_from(const std::string& s) {
    if (s == "right")  return ScrollMode::Right;
    if (s == "static") return ScrollMode::Static;
    if (s == "bounce") return ScrollMode::Bounce;
    return ScrollMode::Left;
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
                         (c.enabled && !cfg_.enabled);
    const bool retint  = c.r != cfg_.r || c.g != cfg_.g || c.b != cfg_.b;
    cfg_ = c;
    cfg_.scale = std::clamp(cfg_.scale, 1, 4);
    if (restart) {
        offset_px_ = 0.0;
        done_      = false;
        rebuild_strip_locked();
    } else if (retint && !strip_mask_.empty()) {
        strip_.setTo(cv::Scalar(cfg_.r, cfg_.g, cfg_.b), strip_mask_);
    }
}

ScrollTextConfig ScrollText::config() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

bool ScrollText::active() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_.enabled && !done_ && !cfg_.text.empty();
}

void ScrollText::rebuild_strip_locked() {
    strip_.release();
    strip_mask_.release();
    if (cfg_.text.empty()) return;

    const int tw = max_content::text_width(cfg_.text);
    if (tw <= 0) return;

    // Rasterise at 1× through the shared 5×7 font (paints white on black),
    // then integer-upscale with nearest-neighbour so glyphs stay blocky, and
    // tint via the lit-pixel mask. Canvas order matches draw_text's (RGB).
    cv::Mat mono(7, tw, CV_8UC3, cv::Scalar(0, 0, 0));
    max_content::draw_text(mono, cfg_.text, 0, 0);
    if (cfg_.scale > 1)
        cv::resize(mono, mono, cv::Size(tw * cfg_.scale, 7 * cfg_.scale),
                   0, 0, cv::INTER_NEAREST);
    cv::cvtColor(mono, strip_mask_, cv::COLOR_RGB2GRAY);
    // Bold: grow the lit-pixel mask a step so strokes read heavier.
    if (cfg_.bold) {
        const int k = std::max(2, cfg_.scale);
        cv::dilate(strip_mask_, strip_mask_,
                   cv::getStructuringElement(cv::MORPH_RECT, {k, k}));
    }
    strip_ = cv::Mat(strip_mask_.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    strip_.setTo(cv::Scalar(cfg_.r, cfg_.g, cfg_.b), strip_mask_);
}

void ScrollText::tick(double dt) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty()) return;
    if (cfg_.mode == ScrollMode::Static) return;   // no motion
    offset_px_ += cfg_.speed_px_s * std::max(0.0, dt);
}

void ScrollText::render(cv::Mat& canvas) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty() || canvas.empty()) return;

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
        x = (canvas.cols - strip_.cols) / 2;   // centred, no motion
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

    int y;
    switch (cfg_.vpos) {
    case TextVPos::Top:    y = 0; break;
    case TextVPos::Bottom: y = std::max(0, canvas.rows - strip_.rows); break;
    case TextVPos::Center: default:
        y = std::max(0, (canvas.rows - strip_.rows) / 2); break;
    }

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
    const int w   = std::min(strip_.cols - sx0, canvas.cols - dx0);
    const int h   = std::min(strip_.rows - sy0, canvas.rows - dy0);
    if (w <= 0 || h <= 0) return;

    cv::Mat src  = strip_(cv::Rect(sx0, sy0, w, h));
    cv::Mat mask = strip_mask_(cv::Rect(sx0, sy0, w, h));
    cv::Mat dst  = canvas(cv::Rect(dx0, dy0, w, h));
    src.copyTo(dst, mask);
}

}  // namespace face
