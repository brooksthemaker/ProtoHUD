#include "materials.h"

#include <cstdio>
#include <sys/stat.h>

#include "face_image.h"

namespace face {

static bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

// ── Solid ────────────────────────────────────────────────────────────────────

SolidMaterial::SolidMaterial(int r, int g, int b, int width, int height) {
    // RGB channel order (channel 0 = R), matching the renderer convention.
    frame_ = cv::Mat(height, width, CV_8UC3,
                     cv::Scalar(static_cast<double>(r),
                                static_cast<double>(g),
                                static_cast<double>(b)));
}

// ── Texture ──────────────────────────────────────────────────────────────────

TextureMaterial::TextureMaterial(const std::string& path, int width, int height,
                                 double scroll_x, double scroll_y)
    : w_(width), h_(height), scroll_x_(scroll_x), scroll_y_(scroll_y) {
    cv::Mat img = load_png_rgb_native(path);
    if (img.empty()) return;

    // Tile to >= panel size so a scroll roll wraps cleanly, then crop.
    int tw = std::max(width,  img.cols);
    int th = std::max(height, img.rows);
    cv::Mat tiled(th, tw, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int y = 0; y < th; y += img.rows)
        for (int x = 0; x < tw; x += img.cols) {
            int cw = std::min(img.cols, tw - x);
            int ch = std::min(img.rows, th - y);
            img(cv::Rect(0, 0, cw, ch)).copyTo(tiled(cv::Rect(x, y, cw, ch)));
        }
    base_ = tiled(cv::Rect(0, 0, width, height)).clone();
}

cv::Mat TextureMaterial::get_frame() {
    if (base_.empty()) return cv::Mat(h_, w_, CV_8UC3, cv::Scalar(0, 220, 180));
    int sx = w_ > 0 ? (static_cast<int>(scroll_x_ * t_) % w_) : 0;
    int sy = h_ > 0 ? (static_cast<int>(scroll_y_ * t_) % h_) : 0;
    if (sx == 0 && sy == 0) return base_;
    return roll(base_, sx, sy);
}

// ── Zoned ────────────────────────────────────────────────────────────────────

ZonedMaterial::ZonedMaterial(std::shared_ptr<BaseMaterial> a,
                             std::shared_ptr<BaseMaterial> b,
                             int width, int height, double boundary)
    : a_(std::move(a)), b_(std::move(b)), w_(width), h_(height),
      split_(static_cast<int>(width * boundary)) {}

cv::Mat ZonedMaterial::get_frame() {
    cv::Mat a = a_->get_frame();
    cv::Mat b = b_->get_frame();
    cv::Mat frame(h_, w_, CV_8UC3);
    if (split_ > 0)        a(cv::Rect(0, 0, split_, h_)).copyTo(frame(cv::Rect(0, 0, split_, h_)));
    if (split_ < w_)       b(cv::Rect(split_, 0, w_ - split_, h_))
                               .copyTo(frame(cv::Rect(split_, 0, w_ - split_, h_)));
    return frame;
}

// ── Factory ──────────────────────────────────────────────────────────────────

std::shared_ptr<BaseMaterial> load_material(
    const std::string& name, int width, int height,
    double scroll_x, double scroll_y, const std::string& materials_dir) {

    if (name.rfind("solid:", 0) == 0) {
        int r = 0, g = 0, b = 0;
        std::sscanf(name.c_str() + 6, "%d,%d,%d", &r, &g, &b);
        return std::make_shared<SolidMaterial>(r, g, b, width, height);
    }

    if (name.rfind("zone:", 0) == 0) {
        std::string rest = name.substr(5);
        auto bar = rest.find('|');
        std::string an = bar == std::string::npos ? rest : rest.substr(0, bar);
        std::string bn = bar == std::string::npos ? rest : rest.substr(bar + 1);
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        auto a = load_material(trim(an), width, height, scroll_x, scroll_y, materials_dir);
        auto b = load_material(trim(bn), width, height, scroll_x, scroll_y, materials_dir);
        return std::make_shared<ZonedMaterial>(a, b, width, height);
    }

    for (const std::string& cand : {materials_dir + "/" + name + ".png",
                                    materials_dir + "/" + name}) {
        if (file_exists(cand)) {
            auto tex = std::make_shared<TextureMaterial>(cand, width, height,
                                                         scroll_x, scroll_y);
            if (tex->valid()) return tex;
        }
    }

    std::fprintf(stderr, "[material] '%s' not found — falling back to solid teal\n",
                 name.c_str());
    return std::make_shared<SolidMaterial>(0, 220, 180, width, height);
}

} // namespace face
