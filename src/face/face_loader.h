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
    // src_w/src_h describe the full canvas this panel is a slice of (0 = the
    // panel IS the whole image). src_x/src_y are the panel's offset within that
    // canvas. When a face PNG is authored at canvas size (a multi-panel HUB75
    // face drawn across the whole editor canvas), the loader crops this panel's
    // slice instead of squishing the entire image into one panel.
    FaceLoader(const std::string& folder, int width, int height,
               int src_w = 0, int src_h = 0, int src_x = 0, int src_y = 0);

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

    // Force whole-image blink (crossfade the entire frame to blink.png) instead
    // of per-eye region blink. Used for a multi-panel face rendered as one
    // canvas, where any eye regions in config.json were authored for a single
    // panel and would only cover one eye on the wide canvas.
    void    set_whole_face_blink(bool b) { whole_face_blink_ = b; }

private:
    // A blink/mouth region. x,y,w,h is always the (panel-local) bounding box
    // used to clip the blend ROI. When `mask` is non-empty it is a panel-sized
    // CV_8U stencil (255 inside the authored polygon) so non-rectangular eye
    // shapes only swap pixels inside the polygon; legacy rectangle regions leave
    // `mask` empty and fill the whole bounding box.
    struct Region {
        int x = 0, y = 0, w = 0, h = 0;
        bool set = false;
        cv::Mat mask;   // empty = rectangular; else panel-sized polygon stencil
    };

    void load();
    // Load a face PNG sized to this panel: crops our slice when the PNG is
    // authored at canvas size (multi-panel), else resizes the whole image.
    cv::Mat load_img(const std::string& path) const;
    void blend_region(cv::Mat& frame, const cv::Mat& overlay,
                      const Region& region, double t) const;

    std::string folder_;
    int w_, h_;
    int src_w_ = 0, src_h_ = 0, src_x_ = 0, src_y_ = 0;   // canvas this panel slices
    bool whole_face_blink_ = false;

    // Optional placement transform read from the face folder's config.json so a
    // face authored for one panel size scales/positions sensibly on another:
    //   fit:      "stretch" (legacy fill, default), "contain" (aspect-fit +
    //             letterbox), or "cover" (aspect-fill + crop)
    //   scale:    extra uniform multiplier on top of the fit (1.0 = none)
    //   offset_x/y: post-scale nudge in target pixels (canvas px for multi-panel
    //             faces, panel px otherwise)
    // xform_active_ stays false for legacy faces (no fit/scale/offset keys) so
    // their rendering is byte-for-byte unchanged.
    std::string fit_mode_;
    double      user_scale_ = 1.0;
    int         off_x_ = 0, off_y_ = 0;
    bool        xform_active_ = false;

    std::map<std::string, cv::Mat> expressions_;   // name → RGBA (h,w)
    std::vector<std::string>       expr_order_;     // stable insertion order
    cv::Mat  blink_;            // may be empty
    // Viseme overlays keyed by stem (mouth_open / mouth_small / mouth_smile /
    // mouth_round). All optional — missing entries fall back to mouth_open.
    std::map<std::string, cv::Mat> mouth_shapes_;
    Region   eye_left_, eye_right_, mouth_;
};

} // namespace face
