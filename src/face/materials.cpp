#include "materials.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

// ── Gradient ───────────────────────────────────────────────────────────────────

GradientMaterial::GradientMaterial(std::vector<cv::Vec3b> colors,
                                   int width, int height,
                                   bool horizontal, bool smooth, double speed,
                                   bool mirror)
    : w_(width), h_(height), horizontal_(horizontal), speed_(speed) {
    if (colors.empty()) colors.push_back(cv::Vec3b(0, 220, 180));  // teal fallback
    const int N    = static_cast<int>(colors.size());
    const int span = std::max(1, horizontal ? width : height);
    // Ramp resolution: the whole axis normally, or just the half when mirroring
    // (the other half is the reflection, built below).
    const int L = mirror ? (span + 1) / 2 : span;

    // 1-D colour ramp along the scroll axis. Non-mirror: N cyclic segments,
    // segment i goes colors[i] → colors[(i+1)%N] so position L wraps back to 0
    // (a scroll stays seamless). Mirror: non-cyclic, colors[0] at the edge (p=0)
    // → colors[N-1] at the centre (p=L-1), so no wrap-around seam at the fold.
    std::vector<cv::Vec3b> axis(static_cast<size_t>(L));
    for (int p = 0; p < L; ++p) {
        const double f   = mirror ? (L <= 1 ? 0.0
                                            : static_cast<double>(p) / (L - 1) * (N - 1))
                                  : static_cast<double>(p) / L * N;
        const int    seg = std::min(N - 1, static_cast<int>(f));
        if (!smooth || N == 1) {
            axis[p] = colors[seg];
        } else {
            const double local = f - seg;                    // 0 .. 1 in segment
            const cv::Vec3b& a = colors[seg];
            const cv::Vec3b& b = mirror ? colors[std::min(N - 1, seg + 1)]
                                        : colors[(seg + 1) % N];
            for (int c = 0; c < 3; ++c)
                axis[p][c] = static_cast<uint8_t>(
                    a[c] * (1.0 - local) + b[c] * local + 0.5);
        }
    }

    // Broadcast the ramp across the perpendicular axis. When mirroring, fold the
    // coordinate about the axis centre (min(p, span-1-p)) so both halves reflect.
    base_ = cv::Mat(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        cv::Vec3b* row = base_.ptr<cv::Vec3b>(y);
        for (int x = 0; x < width; ++x) {
            const int p   = horizontal ? x : y;
            const int idx = mirror ? std::min(p, span - 1 - p) : (p % L);
            row[x] = axis[idx];
        }
    }
}

cv::Mat GradientMaterial::get_frame() {
    if (base_.empty()) return cv::Mat(h_, w_, CV_8UC3, cv::Scalar(0, 220, 180));
    if (speed_ == 0.0) return base_;
    const int L = horizontal_ ? w_ : h_;
    if (L <= 0) return base_;
    const int off = static_cast<int>(speed_ * t_);
    if (off % L == 0) return base_;
    return horizontal_ ? roll(base_, off, 0) : roll(base_, 0, off);
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

    // gradient:<dir>:<mode>:<speed>:<RRGGBB>-<RRGGBB>-...
    //   dir  = h | v | hm | vm   (…m = mirror the ramp about the axis centre)
    //   mode = s (smooth) | b (banded)          speed = px/s (int)
    // e.g. "gradient:hm:s:20:00DCB4-0064FF-B41EDC"
    if (name.rfind("gradient:", 0) == 0) {
        std::vector<std::string> parts;     // dir, mode, speed, hexlist
        size_t start = 9;                   // after "gradient:"
        for (int f = 0; f < 3; ++f) {
            size_t colon = name.find(':', start);
            parts.push_back(name.substr(start, colon == std::string::npos
                                                   ? std::string::npos
                                                   : colon - start));
            if (colon == std::string::npos) { start = name.size(); break; }
            start = colon + 1;
        }
        const std::string hexlist = start <= name.size() ? name.substr(start)
                                                          : std::string();
        const std::string dir  = parts.empty() ? std::string("h") : parts[0];
        const bool horizontal  = dir.empty() || dir[0] != 'v';
        const bool mirror      = dir.size() >= 2 && dir[1] == 'm';   // "hm" / "vm"
        const bool smooth      = parts.size() < 2 || parts[1] != "b";
        const double speed     = parts.size() < 3 ? 0.0 : std::atof(parts[2].c_str());

        std::vector<cv::Vec3b> colors;
        size_t cs = 0;
        while (cs < hexlist.size()) {
            size_t dash = hexlist.find('-', cs);
            std::string tok = hexlist.substr(cs, dash == std::string::npos
                                                    ? std::string::npos
                                                    : dash - cs);
            if (tok.size() >= 6) {
                unsigned v = static_cast<unsigned>(std::strtoul(
                    tok.substr(0, 6).c_str(), nullptr, 16));
                colors.emplace_back(static_cast<uint8_t>((v >> 16) & 0xFF),
                                    static_cast<uint8_t>((v >> 8) & 0xFF),
                                    static_cast<uint8_t>(v & 0xFF));
            }
            if (dash == std::string::npos) break;
            cs = dash + 1;
        }
        return std::make_shared<GradientMaterial>(std::move(colors), width, height,
                                                  horizontal, smooth, speed, mirror);
    }

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
