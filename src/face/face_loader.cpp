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

namespace {
// Place `src` into a (tw × th) RGBA target per a fit mode, with an extra
// uniform `scale` and a centred-plus-offset placement. INTER_NEAREST keeps
// pixel-art crisp. "stretch" reproduces the legacy fill (non-uniform resize to
// the target) so scale 1 / offset 0 / stretch == the old behaviour exactly.
cv::Mat fit_image(const cv::Mat& src, int tw, int th,
                  const std::string& mode, double scale, int ox, int oy) {
    cv::Mat out(std::max(1, th), std::max(1, tw), CV_8UC4, cv::Scalar(0, 0, 0, 0));
    if (src.empty() || tw <= 0 || th <= 0) return out;
    const double sw = src.cols, sh = src.rows;
    double fx, fy;
    if (mode == "contain")    { double s = std::min(tw / sw, th / sh); fx = fy = s; }
    else if (mode == "cover") { double s = std::max(tw / sw, th / sh); fx = fy = s; }
    else                      { fx = tw / sw; fy = th / sh; }   // stretch (non-uniform)
    fx *= scale; fy *= scale;
    const int dw = std::max(1, static_cast<int>(std::lround(sw * fx)));
    const int dh = std::max(1, static_cast<int>(std::lround(sh * fy)));
    cv::Mat scaled;
    cv::resize(src, scaled, cv::Size(dw, dh), 0, 0, cv::INTER_NEAREST);
    const int dx = (tw - dw) / 2 + ox;          // centre, then nudge
    const int dy = (th - dh) / 2 + oy;
    const cv::Rect dst(dx, dy, dw, dh);
    const cv::Rect inter = dst & cv::Rect(0, 0, tw, th);
    if (inter.width > 0 && inter.height > 0)
        scaled(cv::Rect(inter.x - dx, inter.y - dy, inter.width, inter.height))
            .copyTo(out(inter));
    return out;
}
} // namespace

FaceLoader::FaceLoader(const std::string& folder, int width, int height,
                       int src_w, int src_h, int src_x, int src_y)
    : folder_(folder), w_(width), h_(height),
      src_w_(src_w), src_h_(src_h), src_x_(src_x), src_y_(src_y) {
    load();
}

// Load a face PNG sized to this panel. When the panel is a slice of a wider
// canvas (src_w_ > w_) and the PNG was authored at (roughly) canvas width, the
// image is resized to the canvas and this panel's [src_x_,src_y_,w_,h_] rect is
// cropped — so a face drawn across two panels lands on the right halves instead
// of being squished onto each. Otherwise (panel-sized art, legacy faces) the
// whole image is nearest-resized to the panel as before.
cv::Mat FaceLoader::load_img(const std::string& path) const {
    cv::Mat raw = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (raw.empty()) return cv::Mat();
    cv::Mat rgba;
    if      (raw.channels() == 4) cv::cvtColor(raw, rgba, cv::COLOR_BGRA2RGBA);
    else if (raw.channels() == 3) cv::cvtColor(raw, rgba, cv::COLOR_BGR2RGBA);
    else if (raw.channels() == 1) cv::cvtColor(raw, rgba, cv::COLOR_GRAY2RGBA);
    else                          return cv::Mat();

    if (src_w_ > w_ && src_h_ > 0 && rgba.cols > w_) {
        // Canvas-authored multi-panel face → fit into the full canvas, then
        // crop our slice. Legacy faces (no transform) use a plain stretch fill.
        cv::Mat canvas = xform_active_
            ? fit_image(rgba, src_w_, src_h_, fit_mode_, user_scale_, off_x_, off_y_)
            : [&]{ cv::Mat c; cv::resize(rgba, c, cv::Size(src_w_, src_h_), 0, 0,
                                         cv::INTER_NEAREST); return c; }();
        cv::Mat out(h_, w_, canvas.type(), cv::Scalar(0, 0, 0, 0));
        const cv::Rect want(src_x_, src_y_, w_, h_);
        const cv::Rect inter = want & cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (inter.width > 0 && inter.height > 0)
            canvas(inter).copyTo(out(cv::Rect(inter.x - src_x_, inter.y - src_y_,
                                              inter.width, inter.height)));
        return out;
    }
    if (xform_active_)
        return fit_image(rgba, w_, h_, fit_mode_, user_scale_, off_x_, off_y_);
    cv::Mat out;
    cv::resize(rgba, out, cv::Size(w_, h_), 0, 0, cv::INTER_NEAREST);
    return out;
}

void FaceLoader::load() {
    json cfg = json::object();
    fs::path cfg_path = fs::path(folder_) / "config.json";
    if (fs::exists(cfg_path)) {
        std::ifstream f(cfg_path);
        try { f >> cfg; } catch (...) { cfg = json::object(); }
    }

    // Placement transform (optional) — read BEFORE images load so load_img can
    // apply it. Only "active" when at least one of fit/scale/offset is present,
    // so legacy faces render exactly as before.
    if (cfg.contains("fit") && cfg["fit"].is_string())
        fit_mode_ = cfg["fit"].get<std::string>();
    if (cfg.contains("scale") && cfg["scale"].is_number())
        user_scale_ = cfg["scale"].get<double>();
    if (cfg.contains("offset_x") && cfg["offset_x"].is_number())
        off_x_ = static_cast<int>(std::lround(cfg["offset_x"].get<double>()));
    if (cfg.contains("offset_y") && cfg["offset_y"].is_number())
        off_y_ = static_cast<int>(std::lround(cfg["offset_y"].get<double>()));
    if (user_scale_ <= 0.0) user_scale_ = 1.0;                 // guard
    xform_active_ = (!fit_mode_.empty() && fit_mode_ != "stretch")
                 || user_scale_ != 1.0 || off_x_ != 0 || off_y_ != 0;

    // Expression map: name → filename (or scan folder for PNGs).
    std::vector<std::pair<std::string, std::string>> expr_map;
    if (cfg.contains("expressions") && cfg["expressions"].is_object()) {
        for (auto& [name, fn] : cfg["expressions"].items())
            expr_map.emplace_back(name, fn.get<std::string>());
    } else {
        std::vector<std::string> pngs;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(folder_, ec))
            if (e.path().extension() == ".png") pngs.push_back(e.path().filename().string());
        std::sort(pngs.begin(), pngs.end());
        for (auto& p : pngs) {
            std::string stem = fs::path(p).stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            if (stem != "blink" && stem != "mouth_open") expr_map.emplace_back(stem, p);
        }
    }

    for (auto& [name, filename] : expr_map) {
        cv::Mat img = load_img((fs::path(folder_) / filename).string());
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
    blink_ = load_img((fs::path(folder_) / blink_file).string());

    // Viseme overlays — all four are optional. A missing mouth_open simply
    // disables audio-driven mouth blending; missing visemes fall back to
    // mouth_open in get_frame's lookup. Stems match the Files > Faces >
    // Mouth Shapes import slot names.
    for (const char* shape : {"mouth_open", "mouth_small", "mouth_smile", "mouth_round"}) {
        fs::path p = fs::path(folder_) / (std::string(shape) + ".png");
        if (fs::exists(p)) {
            cv::Mat img = load_img(p.string());
            if (!img.empty()) mouth_shapes_[shape] = std::move(img);
        }
    }

    // Region scaling: boxes may be authored at draw_size and scaled to panel px.
    double sx = 1.0, sy = 1.0;
    if (cfg.contains("draw_size") && cfg["draw_size"].is_array() &&
        cfg["draw_size"].size() == 2) {
        double dw = cfg["draw_size"][0].get<double>();
        double dh = cfg["draw_size"][1].get<double>();
        if (dw > 0 && dh > 0) { sx = w_ / dw; sy = h_ / dh; }
    }
    // When this loader is a slice of a wider canvas (multi-panel HUB75 face),
    // eye/mouth regions are authored in CANVAS pixels — the same space the
    // editor paints in. The face image is resized to the full canvas then this
    // panel's [src_x_,src_y_,w_,h_] rect is cropped 1:1 (see load_img), so a
    // canvas-space box maps to panel-local by simply subtracting the slice
    // origin — no scaling. blend_region then clamps boxes that fall off this
    // panel (e.g. the eye that lives on the other panel) to nothing.
    const bool canvas_coords = (src_w_ > w_ && src_h_ > 0);
    const double cx = canvas_coords ? src_w_ * 0.5 : w_ * 0.5;
    const double cy = canvas_coords ? src_h_ * 0.5 : h_ * 0.5;
    // Map one source-space point (config coords) to this panel's local pixels,
    // matching fit_image's placement so region-blink tracks the artwork.
    // (contain/cover's aspect change isn't applied to regions — only the user
    // scale + offset — so they may drift a little under those modes; whole-face
    // blink is unaffected.)
    auto map_pt = [&](double px, double py) -> cv::Point {
        if (!canvas_coords) { px *= sx; py *= sy; }      // draw_size → panel scale
        if (xform_active_) {
            px = cx + (px - cx) * user_scale_ + off_x_;
            py = cy + (py - cy) * user_scale_ + off_y_;
        }
        if (canvas_coords) { px -= src_x_; py -= src_y_; }
        return { static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py)) };
    };
    auto parse_region = [&](const json& d) {
        Region r;
        // New form: a free-form closed polygon {"points":[[x,y],...]}. Build a
        // panel-sized stencil so the blink only swaps pixels inside the shape.
        if (d.contains("points") && d["points"].is_array() && d["points"].size() >= 3) {
            std::vector<cv::Point> poly;
            poly.reserve(d["points"].size());
            for (const auto& pt : d["points"])
                if (pt.is_array() && pt.size() == 2)
                    poly.push_back(map_pt(pt[0].get<double>(), pt[1].get<double>()));
            if (poly.size() >= 3) {
                int minx = poly[0].x, miny = poly[0].y, maxx = poly[0].x, maxy = poly[0].y;
                for (const auto& p : poly) {
                    minx = std::min(minx, p.x); miny = std::min(miny, p.y);
                    maxx = std::max(maxx, p.x); maxy = std::max(maxy, p.y);
                }
                r.x = minx; r.y = miny;
                r.w = std::max(1, maxx - minx + 1);
                r.h = std::max(1, maxy - miny + 1);
                r.mask = cv::Mat::zeros(h_, w_, CV_8U);
                std::vector<std::vector<cv::Point>> fill = { poly };
                cv::fillPoly(r.mask, fill, cv::Scalar(255), cv::LINE_8);
                r.set = true;
            }
            return r;
        }
        // Legacy rectangle {x,y,w,h} — same as before, no stencil.
        double rx, ry, rw, rh;
        if (canvas_coords) {
            rx = static_cast<double>(d.value("x", 0));
            ry = static_cast<double>(d.value("y", 0));
            rw = std::max(1.0, static_cast<double>(d.value("w", 1)));
            rh = std::max(1.0, static_cast<double>(d.value("h", 1)));
        } else {
            rx = d.value("x", 0) * sx; ry = d.value("y", 0) * sy;
            rw = std::max(1.0, d.value("w", 1) * sx);
            rh = std::max(1.0, d.value("h", 1) * sy);
        }
        if (xform_active_) {                        // mirror fit_image's placement
            rx = cx + (rx - cx) * user_scale_ + off_x_;
            ry = cy + (ry - cy) * user_scale_ + off_y_;
            rw *= user_scale_; rh *= user_scale_;
        }
        if (canvas_coords) { rx -= src_x_; ry -= src_y_; }
        r.x = static_cast<int>(std::lround(rx));
        r.y = static_cast<int>(std::lround(ry));
        r.w = std::max(1, static_cast<int>(std::lround(rw)));
        r.h = std::max(1, static_cast<int>(std::lround(rh)));
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
    if (!region.mask.empty() && region.mask.size() == base.size()) {
        // Polygon region: blend the whole ROI, then copy back only the pixels
        // inside the stencil so a non-rectangular eye shape is honoured.
        cv::Mat blended;
        cv::addWeighted(base(roi), 1.0 - t, overlay(roi), t, 0.0, blended);
        blended.copyTo(out(roi), region.mask(roi));
        return out;
    }
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
            // Region blink: cross-fade ONLY the eye box(es) from the open
            // expression to the blink art, so the open eye is replaced (it
            // closes) while the mouth/nose outside the boxes are untouched.
            // Used in both single- and multi-panel mode whenever eye regions
            // are defined — the boxes are mapped to this panel's slice above.
            if (eye_left_.set)  frame = blend_region(frame, blink_, eye_left_,  bw);
            if (eye_right_.set) frame = blend_region(frame, blink_, eye_right_, bw);
        } else {
            // No eye regions defined → fall back to a whole-face alpha
            // composite of the blink canvas over the face using the blink
            // PNG's own alpha (scaled by bw). Transparent areas keep the live
            // face. NOTE: this can't *close* an open eye that extends past the
            // blink art (the blink only adds pixels, never removes the open
            // eye), so define eye regions in the editor for a proper blink.
            // Both images are RGBA at this loader's panel size.
            if (blink_.size() == frame.size() && blink_.type() == CV_8UC4) {
                for (int y = 0; y < frame.rows; ++y) {
                    cv::Vec4b*       fr = frame.ptr<cv::Vec4b>(y);
                    const cv::Vec4b* bl = blink_.ptr<cv::Vec4b>(y);
                    for (int x = 0; x < frame.cols; ++x) {
                        double a = (bl[x][3] / 255.0) * bw;   // overlay coverage
                        if (a <= 0.0) continue;
                        for (int c = 0; c < 3; ++c)
                            fr[x][c] = cv::saturate_cast<uchar>(
                                fr[x][c] * (1.0 - a) + bl[x][c] * a);
                        if (bl[x][3] > fr[x][3]) fr[x][3] = bl[x][3];  // stay opaque on the eye
                    }
                }
            } else {
                cv::addWeighted(frame, 1.0 - bw, blink_, bw, 0.0, frame);  // size mismatch fallback
            }
        }
    }

    // 3. Mouth open.
    double mo = std::clamp(state.mouth_open(), 0.0, 1.0);
    if (mo > 0.0 && mouth_.set) {
        auto it = mouth_shapes_.find(state.mouth_shape());
        if (it == mouth_shapes_.end())
            it = mouth_shapes_.find("mouth_open");   // fallback to AH
        if (it != mouth_shapes_.end() && !it->second.empty())
            frame = blend_region(frame, it->second, mouth_, mo);
    }

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

cv::Mat FaceLoader::get_expression_image(const std::string& name) const {
    auto it = expressions_.find(name);
    if (it == expressions_.end()) return cv::Mat();
    return it->second.clone();
}

bool FaceLoader::set_expression_image(const std::string& name, const cv::Mat& rgba) {
    auto it = expressions_.find(name);
    if (it == expressions_.end() || rgba.empty()) return false;
    it->second = rgba.clone();
    return true;
}

} // namespace face
