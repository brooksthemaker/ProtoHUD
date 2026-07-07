#include "scroll_text.h"

#include <algorithm>

#include <opencv2/imgproc.hpp>

#include "max_section_content.h"

namespace face {

nlohmann::json ScrollTextConfig::to_json() const {
    return {
        {"enabled",    enabled},
        {"text",       text},
        {"speed_px_s", speed_px_s},
        {"scale",      scale},
        {"y",          y},
        {"color",      {r, g, b}},
        {"loop",       loop},
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
    strip_ = cv::Mat(mono.size(), CV_8UC3, cv::Scalar(0, 0, 0));
    strip_.setTo(cv::Scalar(cfg_.r, cfg_.g, cfg_.b), strip_mask_);
}

void ScrollText::tick(double dt) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty()) return;
    offset_px_ += cfg_.speed_px_s * std::max(0.0, dt);
}

void ScrollText::render(cv::Mat& canvas) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!cfg_.enabled || done_ || strip_.empty() || canvas.empty()) return;

    // One pass = the strip entering at the right edge and leaving at the left.
    const double travel = canvas.cols + strip_.cols;
    if (offset_px_ >= travel) {
        if (cfg_.loop) {
            offset_px_ = std::fmod(offset_px_, travel);
        } else {
            done_ = true;
            return;
        }
    }

    const int x  = canvas.cols - static_cast<int>(offset_px_);
    const int y  = (cfg_.y >= 0) ? cfg_.y
                                 : std::max(0, (canvas.rows - strip_.rows) / 2);

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
