#pragma once
// ── materials.h ────────────────────────────────────────────────────────────────
// C++ port of protoface/material.py. A material is the colour/pattern painted
// onto the face pixels. get_frame() returns an (h, w) RGB cv::Mat (CV_8UC3).

#include <memory>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace face {

class BaseMaterial {
public:
    virtual ~BaseMaterial() = default;
    virtual void update(double /*dt*/) {}
    virtual cv::Mat get_frame() = 0;   // (h, w) CV_8UC3 RGB
};

// Flat colour.
class SolidMaterial : public BaseMaterial {
public:
    SolidMaterial(int r, int g, int b, int width, int height);
    cv::Mat get_frame() override { return frame_; }
private:
    cv::Mat frame_;
};

// PNG tiled to panel size, optionally scrolling.
class TextureMaterial : public BaseMaterial {
public:
    TextureMaterial(const std::string& path, int width, int height,
                    double scroll_x = 0.0, double scroll_y = 0.0);
    void update(double dt) override { t_ += dt; }
    cv::Mat get_frame() override;
    bool valid() const { return !base_.empty(); }
private:
    int    w_, h_;
    double scroll_x_, scroll_y_, t_ = 0.0;
    cv::Mat base_;   // (h, w) CV_8UC3 RGB, already cropped to panel size
};

// Multi-colour gradient, optionally scrolling. The colour stops are laid out
// cyclically along one axis (so a scroll wraps seamlessly): smooth = linear
// blend between adjacent stops; banded = hard equal-width stripes. speed is the
// scroll rate in px/s along the axis (0 = static, sign = direction).
class GradientMaterial : public BaseMaterial {
public:
    GradientMaterial(std::vector<cv::Vec3b> colors, int width, int height,
                     bool horizontal, bool smooth, double speed);
    void update(double dt) override { t_ += dt; }
    cv::Mat get_frame() override;
private:
    int     w_, h_;
    bool    horizontal_;
    double  speed_, t_ = 0.0;
    cv::Mat base_;   // (h, w) CV_8UC3 RGB cyclic gradient
};

// Horizontal split: left of boundary uses mat_a, right uses mat_b.
class ZonedMaterial : public BaseMaterial {
public:
    ZonedMaterial(std::shared_ptr<BaseMaterial> a, std::shared_ptr<BaseMaterial> b,
                  int width, int height, double boundary = 0.5);
    void update(double dt) override { a_->update(dt); b_->update(dt); }
    cv::Mat get_frame() override;
private:
    std::shared_ptr<BaseMaterial> a_, b_;
    int w_, h_, split_;
};

// Factory mirroring material.py load_material():
//   "solid:r,g,b" | "zone:a|b" | "<name>" → materials/<name>.png
// Falls back to solid teal if a PNG can't be found.
std::shared_ptr<BaseMaterial> load_material(
    const std::string& name, int width, int height,
    double scroll_x = 0.0, double scroll_y = 0.0,
    const std::string& materials_dir = "materials");

} // namespace face
