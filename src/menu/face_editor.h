#pragma once
// ── face_editor.h ─────────────────────────────────────────────────────────────
// Full-screen pixel editor for face PNGs, opened from Files > Faces > <slot>
// > Edit… when the active backend is MAX7219 or RGB matrix. Mirrors the
// FilePicker + OSK overlay pattern: while is_open() returns true MenuSystem
// forwards input here and replaces the deep-menu body with our draw().
//
// The editor's canvas is derived from the active backend's chain layout —
// the union of every chain's region. Pixels outside the union are grayed
// out so the user sees exactly what'll light up on hardware. The saved PNG
// keeps the canvas at full renderer canvas_w × canvas_h (writes are
// placed at the bbox origin, blanks elsewhere) so the same file works if
// the backend layout changes later.
//
// Mono mode (MAX7219): paint toggles pixels on/off (255,255,255,255 vs
// 0,0,0,0). Color mode (RGB matrix): paint sets the currently selected
// palette colour.

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <opencv2/core.hpp>

#include "overlay.h"

namespace menu {

class FaceEditor : public IOverlay {
public:
    enum class Mode : uint8_t { Mono, Color };
    enum class Tool : uint8_t { Pencil, Eraser, Bucket, Eyedrop, Line, Rect, EyeBox };
    static constexpr int kToolCount = 7;

    // eye_regions are canvas-space closed polygons (one per eye) defining where
    // a blink replaces the open eye with the blink art — authored point-by-point
    // with the EyeBox tool and round-tripped through the face folder's
    // config.json. Each inner vector is the ordered vertex list of one closed
    // shape (the editor implicitly closes last→first).
    using EyePoly  = std::vector<cv::Point>;
    using CommitFn = std::function<void(const cv::Mat& rgba_canvas,
                                        const std::string& abs_path,
                                        const std::vector<EyePoly>& eye_polys)>;
    using CancelFn = std::function<void()>;
    // Push the current canvas onto the physical panels as a transient face
    // for `duration_s` seconds — main.cpp wires this to
    // NativeFaceController::push_transient_face on the active expression.
    using PreviewFn = std::function<void(const cv::Mat& rgba_canvas,
                                          double duration_s)>;
    // Pull the latest rendered (face + material + effects) canvas from the
    // controller for the in-editor "Show Live" overlay. Returns false until
    // the first frame exists.
    using LiveFrameFn = std::function<bool(cv::Mat& out)>;

    // Open the editor for the PNG at abs_path. canvas_w/h are the full
    // renderer canvas dimensions. covered_regions describes the addressable
    // sub-rects; the editor uses their union for its working bbox and grays
    // out the rest. palette is the color-mode palette (ignored in mono).
    void open(std::string title,
              std::string abs_path,
              int canvas_w, int canvas_h,
              std::vector<cv::Rect> covered_regions,
              std::vector<std::string> covered_labels,   // parallel to covered_regions; "" = unlabelled
              int mirror_axis_x,                          // canvas col index used by mirror brush; <0 → bbox centre
              Mode mode,
              std::vector<uint32_t> palette,    // 0xRRGGBB
              std::vector<EyePoly>  eye_polys,  // existing blink eye polygons (canvas px); empty = none
              CommitFn on_commit,
              CancelFn on_cancel = {},
              PreviewFn on_preview = {},
              LiveFrameFn live_frame = {},
              double preview_duration_s = 10.0);
    void close() override;
    bool is_open() const override { return open_; }

    // Input — wired from MenuSystem when is_open() is true.
    void cursor_step(int dx, int dy);    // D-pad / arrow keys (one pixel)
    void primary();                      // A / Space — paint at cursor
    void secondary();                    // X — cycle tool
    void tertiary();                     // Y — toggle mirror
    void back() override;                // B — cancel (close without save)
    void save();                         // Apply + close

    // IOverlay adapters — knob walk is a vertical cursor step, select paints.
    void step(int d) override            { cursor_step(0, d); }
    void move(int dx, int dy) override   { cursor_step(dx, dy); }
    void activate() override             { primary(); }

    void cycle_palette(int dir);         // shoulder buttons / wheel scroll
    void set_tool(Tool t);               // P/E/B/I keys
    Tool tool()         const { return tool_; }
    // Eye polygons authored this session (canvas px). Persisted by the caller on save.
    const std::vector<EyePoly>& eye_polys() const { return eye_polys_; }
    void set_brush_size(int radius);     // 0 = 1px, 1 = 3x3, 2 = 5x5
    int  brush_size()   const { return brush_size_; }
    void undo();
    // V key — push the current canvas onto the physical panels via on_preview
    // for the configured duration. No-op if no preview callback was supplied.
    void preview();
    // T key — toggle live-effects overlay (renders the controller's latest
    // composited frame in the editor pane instead of just the painted PNG).
    // No-op if no live_frame callback was supplied at open().
    void toggle_live();
    bool live_mode() const { return live_mode_; }

    // Full-screen overlay drawn in place of the deep menu while open. Also
    // polls the editor-specific ImGui keys (tools, brush, undo, save, …) and
    // the mouse — they're editor-only, so the polling lives here rather than
    // in MenuSystem's input set.
    void draw(ImDrawList* dl, ImFont* font, float fs,
              float screen_w, float screen_h, ImU32 accent) override;

    // Mouse helpers — caller passes ImGui::GetMousePos relative to the
    // window origin (typically (0,0) at top-left of the framebuffer).
    // mouse_down is the press EDGE (fires primary() once); mouse_drag is a
    // held-button move (paints a stroke for freehand brushes only — two-step
    // and point tools ignore it so a single click can't re-fire primary()).
    void mouse_move (float mouse_x, float mouse_y);
    void mouse_down (float mouse_x, float mouse_y);
    void mouse_drag (float mouse_x, float mouse_y);

    const std::string& current_dir() const { return abs_path_; }

private:
    void apply_at_cursor();
    void push_undo();
    void paint_pixel(int x, int y);          // raw 1-pixel paint (no brush size)
    void paint_brush(int cx, int cy);        // brush_size_-aware square fill
    void flood_fill(int sx, int sy);         // bucket from (sx, sy)
    void eyedrop_at(int x, int y);
    void draw_line(int x0, int y0, int x1, int y1);   // brush-aware, Bresenham
    void draw_rect_filled(int x0, int y0, int x1, int y1);
    bool inside_covered(int x, int y) const;

    bool open_ = false;
    Mode mode_ = Mode::Mono;
    Tool tool_ = Tool::Pencil;
    bool mirror_ = false;
    int  brush_size_ = 0;                    // radius_pixels (0/1/2)

    // Anchor for Line / Rect tools — set by the first primary() at the
    // cursor position, committed (line / rect drawn from anchor to
    // cursor) by the second primary(). Tool changes / back / undo all
    // clear the anchor so it doesn't persist into a different tool's
    // workflow.
    bool anchor_set_ = false;
    int  anchor_x_   = 0;
    int  anchor_y_   = 0;

    // Blink eye polygons (canvas coords). Authored with Tool::EyeBox; preloaded
    // from config.json on open() and handed back to the caller on save(). Each
    // entry is one closed shape's ordered vertices.
    std::vector<EyePoly> eye_polys_;
    // In-progress polygon being placed vertex-by-vertex with the EyeBox tool.
    // Committed into eye_polys_ when the user clicks back on the first vertex
    // (>= 3 points); cleared by back/undo/tool-change.
    EyePoly eye_pts_;

    std::string title_;
    std::string abs_path_;

    // Working canvas (RGBA, sized to canvas_w × canvas_h).
    cv::Mat                canvas_;
    int                    canvas_w_ = 0, canvas_h_ = 0;
    cv::Rect                  bbox_;             // editable bounding box (union of covered)
    std::vector<cv::Rect>     covered_;          // for the grayed-out display
    std::vector<std::string>  covered_labels_;   // parallel to covered_ (may be empty)
    int                       mirror_axis_x_ = -1; // canvas col fence used by mirror brush; <0 → bbox centre

    // Current cursor in CANVAS coordinates (not bbox-local).
    int                    cursor_x_ = 0, cursor_y_ = 0;

    // Palette + selection (color mode).
    std::vector<uint32_t>  palette_;
    int                    palette_idx_ = 0;

    // Undo ring (cv::Mat::clone snapshots; cap at 16).
    std::vector<cv::Mat>   undo_stack_;

    // Layout cached per-draw so mouse_move/down can convert pixel-to-cell.
    float                  grid_origin_x_ = 0.f, grid_origin_y_ = 0.f;
    float                  cell_size_     = 0.f;

    CommitFn               on_commit_;
    CancelFn               on_cancel_;
    PreviewFn              on_preview_;
    LiveFrameFn            live_frame_;
    double                 preview_duration_s_ = 10.0;
    bool                   live_mode_ = false;
};

} // namespace menu
