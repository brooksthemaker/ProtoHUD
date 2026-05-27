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

namespace menu {

class FaceEditor {
public:
    enum class Mode : uint8_t { Mono, Color };
    enum class Tool : uint8_t { Pencil, Eraser, Bucket, Eyedrop, Line, Rect };
    static constexpr int kToolCount = 6;

    using CommitFn = std::function<void(const cv::Mat& rgba_canvas,
                                        const std::string& abs_path)>;
    using CancelFn = std::function<void()>;

    // Open the editor for the PNG at abs_path. canvas_w/h are the full
    // renderer canvas dimensions. covered_regions describes the addressable
    // sub-rects; the editor uses their union for its working bbox and grays
    // out the rest. palette is the color-mode palette (ignored in mono).
    void open(std::string title,
              std::string abs_path,
              int canvas_w, int canvas_h,
              std::vector<cv::Rect> covered_regions,
              std::vector<std::string> covered_labels,   // parallel to covered_regions; "" = unlabelled
              Mode mode,
              std::vector<uint32_t> palette,    // 0xRRGGBB
              CommitFn on_commit,
              CancelFn on_cancel = {});
    void close();
    bool is_open() const { return open_; }

    // Input — wired from MenuSystem when is_open() is true.
    void cursor_step(int dx, int dy);    // D-pad / arrow keys (one pixel)
    void primary();                      // A / Space — paint at cursor
    void secondary();                    // X — cycle tool
    void tertiary();                     // Y — toggle mirror
    void back();                         // B — cancel (close without save)
    void save();                         // Apply + close

    void cycle_palette(int dir);         // shoulder buttons / wheel scroll
    void set_tool(Tool t);               // P/E/B/I keys
    Tool tool()         const { return tool_; }
    void set_brush_size(int radius);     // 0 = 1px, 1 = 3x3, 2 = 5x5
    int  brush_size()   const { return brush_size_; }
    void undo();

    // Full-screen overlay drawn in place of the deep menu while open.
    void draw(ImDrawList* dl, ImFont* font, float fs,
              float screen_w, float screen_h, ImU32 accent);

    // Mouse helpers — caller passes ImGui::GetMousePos relative to the
    // window origin (typically (0,0) at top-left of the framebuffer).
    void mouse_move (float mouse_x, float mouse_y);
    void mouse_down (float mouse_x, float mouse_y);

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

    std::string title_;
    std::string abs_path_;

    // Working canvas (RGBA, sized to canvas_w × canvas_h).
    cv::Mat                canvas_;
    int                    canvas_w_ = 0, canvas_h_ = 0;
    cv::Rect                  bbox_;             // editable bounding box (union of covered)
    std::vector<cv::Rect>     covered_;          // for the grayed-out display
    std::vector<std::string>  covered_labels_;   // parallel to covered_ (may be empty)

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
};

} // namespace menu
