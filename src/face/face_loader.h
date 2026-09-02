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

#include "face/eye_lid.h"
#include "face/face_config.h"   // WiggleCfg

namespace face {

class FaceState;   // fwd

// Upper bound on animated-blink frames. Kept small on purpose: the whole blink
// is ~0.15 s, so past a handful of frames each one is on screen for a single
// panel refresh and the extra art buys nothing visible.
inline constexpr int kBlinkAnimMaxFrames = 8;

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
    // Per-face wiggle, from this face folder's own config.json. Absent means
    // the face has no opinion and the panel default stands.
    bool    has_wiggle() const { return has_wiggle_; }
    const WiggleCfg& wiggle_cfg() const { return wiggle_; }
    const std::string& folder() const { return folder_; }

    // ── Animated blink ──────────────────────────────────────────────────────
    // Frames live in a `blink/` SUBFOLDER of the face folder (1.png, 2.png, …),
    // and that placement is load-bearing: the expression scan below walks the
    // face folder with a non-recursive directory_iterator and turns every PNG
    // stem into an expression, excluding only the exact names "blink" and
    // "mouth_open". Frames named blink_1.png alongside the others would each
    // become a bogus expression; inside a subfolder the scan simply cannot see
    // them, so there is no exclusion rule to keep in sync.
    // Config lives in the face folder's config.json as
    //   "blink_anim": { "enabled": true, "frames": 4 }
    bool blink_anim_enabled() const { return blink_anim_on_; }
    bool blink_anim_whole()   const { return blink_anim_whole_; }
    int  blink_anim_frames()  const { return blink_anim_frames_; }
    // How many frames actually LOADED. Can be under blink_anim_frames() when
    // art is missing; 0 means fall back to the single blink.png.
    int  blink_frame_count() const { return static_cast<int>(blink_frames_.size()); }

    // Per-EXPRESSION sequences, in `blink/<expression>/` (1.png, 2.png, …), so
    // a happy blink and an angry blink can differ. Config:
    //   "blink_anim": { …, "expressions": { "happy": {"enabled":true,
    //                                                 "frames":3} } }
    // An expression absent from that map inherits the face-wide sequence; one
    // present with enabled=false does not animate at all.
    enum class BlinkMode : uint8_t { Inherit, Own, None };
    BlinkMode blink_mode_for(const std::string& expr) const;
    int       blink_frames_cfg_for(const std::string& expr) const;
    int       blink_frame_count_for(const std::string& expr) const;
    bool      blink_whole_for(const std::string& expr) const;

    // How a sequence lands on the panel.
    //   false (default) — masked to the face's eye_left/eye_right polygons,
    //                     exactly like the single-image blink.
    //   true  ("whole") — the frame REPLACES the whole composited face for
    //                     that tick, because the art is a complete face.
    // ⚠ Two ways to handle differing eye layouts: Whole replaces the entire
    // face so the polygons never matter, OR the expression authors its own
    // polygons (see eye_regions below) and keeps the region blink. The shared
    // pair alone would clip eyes that sit outside it and leave them staring.

    // Panel-sized CV_8U stencil of the blink eye regions — 255 inside
    // eye_left / eye_right (polygon masks honoured, legacy rectangles
    // filled). Empty when the face defines no eye regions. Used by the
    // Animated Eyes overlay's blackout option.
    const cv::Mat& eye_region_mask() const { return eye_mask_; }

    // Per-column LOWER edge of those same eye regions — the closed-lid line.
    // Derived from eye_mask_ at load (regions never change afterwards, and every
    // face reload builds a fresh FaceLoader, so this needs no invalidation).
    // Empty when the face defines no eye regions. Used by the Crying animation
    // so its tears fall from the eye the artist actually drew.
    const EyeLidLine& eye_lid_line() const { return eye_lid_; }

    // Per-EXPRESSION eye regions. An expression can carry its own polygons in
    // the face folder's config.json —
    //   "eye_regions": { "<expr>": { "eye_left": {…}, "eye_right": {…} } }
    // (same region format and canvas/draw_size mapping as the face-wide pair)
    // — so a face whose eyes sit elsewhere can still region-blink instead of
    // needing Cover: Whole Face. These accessors return that expression's
    // mask / lid line when authored, falling back to the face-wide pair.
    const cv::Mat&    eye_region_mask(const std::string& expr) const;
    const EyeLidLine& eye_lid_line(const std::string& expr) const;

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

    // One expression's own eye regions plus the derived mask/lid (mirroring
    // the face-wide eye_left_/eye_right_/eye_mask_/eye_lid_ quartet).
    struct EyeSet {
        Region     left, right;
        cv::Mat    mask;
        EyeLidLine lid;
    };
    // nullptr when the expression has no override (use the face-wide pair).
    const EyeSet* eye_set_for(const std::string& expr) const;

    void load();
    // Load a face PNG sized to this panel: crops our slice when the PNG is
    // authored at canvas size (multi-panel), else resizes the whole image.
    cv::Mat load_img(const std::string& path) const;
    void blend_region(cv::Mat& frame, const cv::Mat& overlay,
                      const Region& region, double t) const;

    std::string folder_;
    // Sharp-motion smoothing. get_frame() takes a CONST FaceState, so this
    // per-panel state lives on the loader. sm_f* is the low-passed target
    // position; sm_i* is the whole-pixel position actually being drawn.
    double      sm_fx_ = 0.0, sm_fy_ = 0.0;
    int         sm_ix_ = 0,   sm_iy_ = 0;
    bool        has_wiggle_ = false;
    WiggleCfg   wiggle_;
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
    // Animated-blink frames, index 0 = barely closed … back = fully shut.
    // Empty (or the feature off) falls back to the single blink_ crossfade.
    std::vector<cv::Mat> blink_frames_;
    bool     blink_anim_on_     = false;
    int      blink_anim_frames_ = 0;
    bool     blink_anim_whole_  = false;   // face-wide Cover mode
    struct BlinkExpr {
        bool                 enabled = true;   // false = this expression never animates
        bool                 whole   = false;  // Cover: whole face vs eye regions
        int                  frames  = 0;      // configured count
        std::vector<cv::Mat> art;              // loaded frames (may be short/empty)
    };
    std::map<std::string, BlinkExpr> blink_expr_;
    // Which sequence a given expression blinks with, and how it lands.
    // seq == nullptr means fall back to the single-image crossfade.
    struct BlinkPick { const std::vector<cv::Mat>* seq = nullptr; bool whole = false; };
    BlinkPick blink_pick_for(const std::string& expr) const;
    // Viseme overlays keyed by stem (mouth_open / mouth_small / mouth_smile /
    // mouth_round). All optional — missing entries fall back to mouth_open.
    std::map<std::string, cv::Mat> mouth_shapes_;
    Region   eye_left_, eye_right_, mouth_;
    cv::Mat  eye_mask_;         // union stencil of the eye regions (may be empty)
    EyeLidLine eye_lid_;        // per-column lower edge of eye_mask_ (may be empty)
    std::map<std::string, EyeSet> eye_sets_;   // per-expression overrides
};

} // namespace face
