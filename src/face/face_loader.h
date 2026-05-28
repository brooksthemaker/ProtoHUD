#pragma once
// ── face_loader.h ──────────────────────────────────────────────────────────────
// C++ port of protoface/face.py. Loads a face folder (PNGs + config.json) and
// composites the current frame each tick: expression crossfade, blink (per-eye
// region or whole-face), mouth-open swap, and idle wiggle + gyro sub-pixel shift.
// Output is an (h, w) RGBA cv::Mat (CV_8UC4, channel 0 = R).

#include <map>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace face {

class FaceState;   // fwd

class FaceLoader {
public:
    FaceLoader(const std::string& folder, int width, int height);

    cv::Mat get_frame(const FaceState& state);   // CV_8UC4
    const std::vector<std::string>& expression_names() const { return expr_order_; }
    bool valid() const { return !expressions_.empty(); }

    // Transient-image plumbing for the editor's "Preview to panels" key.
    // get_expression_image returns a deep copy (caller can stash and
    // restore); set_expression_image swaps the named expression's image
    // wholesale (no resize — caller is responsible for sending a Mat sized
    // to (w, h)). Returns false if the name isn't a known expression.
    cv::Mat get_expression_image(const std::string& name) const;
    bool    set_expression_image(const std::string& name, const cv::Mat& rgba);
    int     panel_width()  const { return w_; }
    int     panel_height() const { return h_; }

private:
    struct Region { int x = 0, y = 0, w = 0, h = 0; bool set = false; };

    void load();
    cv::Mat blend_region(const cv::Mat& base, const cv::Mat& overlay,
                         const Region& region, double t) const;

    std::string folder_;
    int w_, h_;

    std::map<std::string, cv::Mat> expressions_;   // name → RGBA (h,w)
    std::vector<std::string>       expr_order_;     // stable insertion order
    cv::Mat  blink_;            // may be empty
    // Viseme overlays keyed by stem (mouth_open / mouth_small / mouth_smile /
    // mouth_round). All optional — missing entries fall back to mouth_open.
    std::map<std::string, cv::Mat> mouth_shapes_;
    Region   eye_left_, eye_right_, mouth_;
};

} // namespace face
