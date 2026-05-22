#include "face_loader.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>

#include "face_image.h"
#include "face_state.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace face {

FaceLoader::FaceLoader(const std::string& folder, int width, int height)
    : folder_(folder), w_(width), h_(height) {
    load();
}

void FaceLoader::load() {
    json cfg = json::object();
    fs::path cfg_path = fs::path(folder_) / "config.json";
    if (fs::exists(cfg_path)) {
        std::ifstream f(cfg_path);
        try { f >> cfg; } catch (...) { cfg = json::object(); }
    }

    // Expression map: name → filename (or scan folder for PNGs).
    std::vector<std::pair<std::string, std::string>> expr_map;
    if (cfg.contains("expressions") && cfg["expressions"].is_object()) {
        for (auto& [name, fn] : cfg["expressions"].items())
            expr_map.emplace_back(name, fn.get<std::string>());
    } else {
        std::vector<std::string> pngs;
        for (auto& e : fs::directory_iterator(folder_))
            if (e.path().extension() == ".png") pngs.push_back(e.path().filename().string());
        std::sort(pngs.begin(), pngs.end());
        for (auto& p : pngs) {
            std::string stem = fs::path(p).stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            if (stem != "blink" && stem != "mouth_open") expr_map.emplace_back(stem, p);
        }
    }

    for (auto& [name, filename] : expr_map) {
        cv::Mat img = load_png_rgba((fs::path(folder_) / filename).string(), w_, h_);
        if (!img.empty()) {
            expressions_[name] = img;
            expr_order_.push_back(name);
        }
    }

    // Fallback neutral (alias to the first expression).
    if (!expressions_.empty() && expressions_.find("neutral") == expressions_.end()) {
        expressions_["neutral"] = expressions_[expr_order_.front()];
    }

    // Blink image.
    std::string blink_file = cfg.value("blink", std::string("blink.png"));
    blink_ = load_png_rgba((fs::path(folder_) / blink_file).string(), w_, h_);

    // Optional mouth-open image.
    fs::path mo = fs::path(folder_) / "mouth_open.png";
    if (fs::exists(mo)) mouth_open_ = load_png_rgba(mo.string(), w_, h_);

    // Region scaling: boxes may be authored at draw_size and scaled to panel px.
    double sx = 1.0, sy = 1.0;
    if (cfg.contains("draw_size") && cfg["draw_size"].is_array() &&
        cfg["draw_size"].size() == 2) {
        double dw = cfg["draw_size"][0].get<double>();
        double dh = cfg["draw_size"][1].get<double>();
        if (dw > 0 && dh > 0) { sx = w_ / dw; sy = h_ / dh; }
    }
    auto parse_region = [&](const json& d) {
        Region r;
        r.x = static_cast<int>(std::lround(d.value("x", 0) * sx));
        r.y = static_cast<int>(std::lround(d.value("y", 0) * sy));
        r.w = std::max(1, static_cast<int>(std::lround(d.value("w", 1) * sx)));
        r.h = std::max(1, static_cast<int>(std::lround(d.value("h", 1) * sy)));
        r.set = true;
        return r;
    };
    if (cfg.contains("eye_left"))  eye_left_  = parse_region(cfg["eye_left"]);
    if (cfg.contains("eye_right")) eye_right_ = parse_region(cfg["eye_right"]);
    if (cfg.contains("mouth"))     mouth_     = parse_region(cfg["mouth"]);
}

cv::Mat FaceLoader::blend_region(const cv::Mat& base, const cv::Mat& overlay,
                                 const Region& region, double t) const {
    cv::Mat out = base.clone();
    int x  = region.x, y = region.y;
    int x2 = std::min(x + region.w, w_), y2 = std::min(y + region.h, h_);
    x = std::max(0, x); y = std::max(0, y);
    if (x2 <= x || y2 <= y) return out;
    cv::Rect roi(x, y, x2 - x, y2 - y);
    cv::addWeighted(base(roi), 1.0 - t, overlay(roi), t, 0.0, out(roi));
    return out;
}

cv::Mat FaceLoader::get_frame(const FaceState& state) {
    if (expressions_.empty())
        return cv::Mat::zeros(h_, w_, CV_8UC4);

    auto get_expr = [&](const std::string& name) -> const cv::Mat& {
        auto it = expressions_.find(name);
        if (it != expressions_.end()) return it->second;
        return expressions_.at("neutral");
    };

    const cv::Mat& cur  = get_expr(state.expression());
    const cv::Mat& prev = get_expr(state.prev_expression());

    // 1. Expression crossfade.
    double t = std::clamp(state.transition_t(), 0.0, 1.0);
    cv::Mat frame;
    if (t >= 1.0 || state.expression() == state.prev_expression())
        frame = cur.clone();
    else
        cv::addWeighted(prev, 1.0 - t, cur, t, 0.0, frame);

    // 2. Blink.
    double bw = std::clamp(state.blink_weight(), 0.0, 1.0);
    if (bw > 0.0 && !blink_.empty()) {
        if (eye_left_.set || eye_right_.set) {
            if (eye_left_.set)  frame = blend_region(frame, blink_, eye_left_,  bw);
            if (eye_right_.set) frame = blend_region(frame, blink_, eye_right_, bw);
        } else {
            cv::addWeighted(frame, 1.0 - bw, blink_, bw, 0.0, frame);
        }
    }

    // 3. Mouth open.
    double mo = std::clamp(state.mouth_open(), 0.0, 1.0);
    if (mo > 0.0 && mouth_.set && !mouth_open_.empty())
        frame = blend_region(frame, mouth_open_, mouth_, mo);

    // 4. Wiggle + gyro sub-pixel shift (edge-clamped, no wrap).
    const WiggleCfg& wc = state.wiggle();
    double tsec = state.time();
    double dx = wc.amplitude_x * std::sin(2.0 * M_PI * wc.speed * tsec);
    double dy = wc.amplitude_y * std::sin(2.0 * M_PI * wc.speed * tsec * 1.3);
    double shift_x = dx + state.gyro_dx();
    double shift_y = dy + state.gyro_dy();
    if (std::abs(shift_x) > 0.01 || std::abs(shift_y) > 0.01) {
        cv::Mat M = (cv::Mat_<double>(2, 3) << 1, 0, shift_x, 0, 1, shift_y);
        cv::Mat shifted;
        cv::warpAffine(frame, shifted, M, frame.size(),
                       cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        frame = shifted;
    }

    return frame;
}

} // namespace face
