#include "face_editor.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace menu {

namespace {
// Default palette — 16 colour set tuned for the protogen "look" + a few
// neutrals. Stored as 0xRRGGBB.
constexpr uint32_t kDefaultPalette[] = {
    0xffffff, 0x000000,
    0xff2030, 0xff8030, 0xffd030,
    0x30c850, 0x20c0c0, 0x3060ff,
    0x9030c0, 0xc030a0, 0xff60ff,
    0x808080, 0x404040,
    0x00ffd8, 0xd0e8ff, 0xa0d000,
};

uint32_t imu32_from_hex(uint32_t hex_rgb) {
    const uint32_t r = (hex_rgb >> 16) & 0xFF;
    const uint32_t g = (hex_rgb >>  8) & 0xFF;
    const uint32_t b =  hex_rgb        & 0xFF;
    return IM_COL32(r, g, b, 255);
}
} // namespace

void FaceEditor::open(std::string title,
                     std::string abs_path,
                     int canvas_w, int canvas_h,
                     std::vector<cv::Rect> covered_regions,
                     std::vector<std::string> covered_labels,
                     int mirror_axis_x,
                     Mode mode,
                     std::vector<uint32_t> palette,
                     std::vector<EyePoly>  eye_polys,
                     CommitFn on_commit,
                     CancelFn on_cancel,
                     PreviewFn on_preview,
                     LiveFrameFn live_frame,
                     double preview_duration_s) {
    if (canvas_w <= 0 || canvas_h <= 0) return;

    title_           = std::move(title);
    abs_path_        = std::move(abs_path);
    canvas_w_        = canvas_w;
    canvas_h_        = canvas_h;
    covered_         = std::move(covered_regions);
    covered_labels_  = std::move(covered_labels);
    if (covered_labels_.size() != covered_.size()) covered_labels_.assign(covered_.size(), "");
    mirror_axis_x_   = mirror_axis_x;
    eye_polys_       = std::move(eye_polys);
    eye_pts_.clear();
    mode_      = mode;
    tool_      = Tool::Pencil;
    mirror_    = false;
    on_commit_  = std::move(on_commit);
    on_cancel_  = std::move(on_cancel);
    on_preview_ = std::move(on_preview);
    live_frame_ = std::move(live_frame);
    preview_duration_s_ = (preview_duration_s > 0.0) ? preview_duration_s : 10.0;
    live_mode_  = false;

    // Default palette fallback for color mode.
    palette_.clear();
    if (mode_ == Mode::Color) {
        if (palette.empty())
            for (uint32_t c : kDefaultPalette) palette_.push_back(c);
        else
            palette_ = std::move(palette);
    }
    palette_idx_ = 0;

    // Compute bbox = union of covered regions, clamped to the canvas. If
    // covered_ is empty, the editor's input methods stay no-op (caller
    // shouldn't have opened us, but we don't crash).
    bbox_ = cv::Rect();
    if (!covered_.empty()) {
        bbox_ = covered_.front();
        for (size_t i = 1; i < covered_.size(); ++i) bbox_ |= covered_[i];
        bbox_ &= cv::Rect(0, 0, canvas_w_, canvas_h_);
    }

    // Load existing image if present; else start with a fully transparent
    // canvas at canvas_w × canvas_h. Resize-fit if the on-disk image is at
    // an unexpected size.
    canvas_ = cv::Mat();
    cv::Mat existing = cv::imread(abs_path_, cv::IMREAD_UNCHANGED);
    if (!existing.empty()) {
        // Promote to RGBA so paint operations work uniformly.
        cv::Mat rgba;
        if (existing.channels() == 4)      rgba = existing;
        else if (existing.channels() == 3) cv::cvtColor(existing, rgba, cv::COLOR_BGR2RGBA);
        else if (existing.channels() == 1) cv::cvtColor(existing, rgba, cv::COLOR_GRAY2RGBA);
        if (!rgba.empty() &&
            (rgba.cols != canvas_w_ || rgba.rows != canvas_h_)) {
            cv::Mat resized;
            cv::resize(rgba, resized, cv::Size(canvas_w_, canvas_h_),
                       0, 0, cv::INTER_NEAREST);
            rgba = std::move(resized);
        }
        // OpenCV reads PNGs as BGRA on imread; the cvtColor calls above
        // covered the 3/1-channel paths. For the 4-channel native case the
        // bytes are BGRA — swap to RGBA so we match the project's renderer
        // convention.
        if (rgba.channels() == 4) {
            cv::Mat fixed;
            cv::cvtColor(rgba, fixed, cv::COLOR_BGRA2RGBA);
            canvas_ = std::move(fixed);
        }
    }
    if (canvas_.empty())
        canvas_ = cv::Mat(canvas_h_, canvas_w_, CV_8UC4, cv::Scalar(0, 0, 0, 0));

    cursor_x_ = bbox_.x + bbox_.width  / 2;
    cursor_y_ = bbox_.y + bbox_.height / 2;
    undo_stack_.clear();
    anchor_set_ = false;
    brush_size_ = 0;
    tool_       = Tool::Pencil;

    open_ = true;
}

void FaceEditor::close() {
    open_       = false;
    on_commit_  = nullptr;
    on_cancel_  = nullptr;
    on_preview_ = nullptr;
    live_frame_ = nullptr;
    live_mode_  = false;
    canvas_     = cv::Mat();
    undo_stack_.clear();
    covered_.clear();
    covered_labels_.clear();
    palette_.clear();
}

bool FaceEditor::inside_covered(int x, int y) const {
    for (const auto& r : covered_)
        if (x >= r.x && x < r.x + r.width &&
            y >= r.y && y < r.y + r.height) return true;
    return false;
}

void FaceEditor::push_undo() {
    constexpr int kMaxUndo = 16;
    if (undo_stack_.size() >= static_cast<size_t>(kMaxUndo))
        undo_stack_.erase(undo_stack_.begin());
    undo_stack_.push_back(canvas_.clone());
}

void FaceEditor::paint_pixel(int x, int y) {
    if (!inside_covered(x, y)) return;
    cv::Vec4b& dst = canvas_.at<cv::Vec4b>(y, x);
    if (tool_ == Tool::Eraser) {
        dst = cv::Vec4b(0, 0, 0, 0);
        return;
    }
    if (mode_ == Mode::Mono) {
        dst = cv::Vec4b(255, 255, 255, 255);
    } else {
        const uint32_t hex = palette_.empty() ? 0xffffff
                              : palette_[palette_idx_ % palette_.size()];
        const uint8_t r = (hex >> 16) & 0xFF;
        const uint8_t g = (hex >>  8) & 0xFF;
        const uint8_t b =  hex        & 0xFF;
        dst = cv::Vec4b(r, g, b, 255);   // RGBA order (matches loader)
    }
}

void FaceEditor::paint_brush(int cx, int cy) {
    // brush_size_ is a radius in pixels (0 = single, 1 = 3x3, 2 = 5x5).
    // Each cell of the square fires paint_pixel which is bbox/covered safe.
    const int r = brush_size_;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            paint_pixel(cx + dx, cy + dy);
}

void FaceEditor::flood_fill(int sx, int sy) {
    if (!inside_covered(sx, sy)) return;
    const cv::Vec4b seed = canvas_.at<cv::Vec4b>(sy, sx);
    // Pick the target colour the bucket will write, then bail if it's the
    // same as the seed (nothing to do — saves a noisy undo entry).
    cv::Vec4b target;
    if (tool_ == Tool::Eraser) {
        target = cv::Vec4b(0, 0, 0, 0);
    } else if (mode_ == Mode::Mono) {
        target = cv::Vec4b(255, 255, 255, 255);
    } else {
        const uint32_t hex = palette_.empty() ? 0xffffff
                              : palette_[palette_idx_ % palette_.size()];
        target = cv::Vec4b(static_cast<uint8_t>((hex >> 16) & 0xFF),
                           static_cast<uint8_t>((hex >>  8) & 0xFF),
                           static_cast<uint8_t>( hex        & 0xFF), 255);
    }
    if (seed == target) return;

    // Iterative scanline-ish 4-connected flood fill. Bounded by the bbox.
    std::vector<std::pair<int, int>> stack;
    stack.reserve(64);
    stack.emplace_back(sx, sy);
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (!inside_covered(x, y)) continue;
        if (canvas_.at<cv::Vec4b>(y, x) != seed) continue;
        canvas_.at<cv::Vec4b>(y, x) = target;
        stack.emplace_back(x + 1, y);
        stack.emplace_back(x - 1, y);
        stack.emplace_back(x, y + 1);
        stack.emplace_back(x, y - 1);
    }
}

// Brush-aware Bresenham. Walks the line one cell at a time and stamps the
// current brush so wider sizes give thicker lines, matching the pencil's
// behaviour. Same eraser/colour resolution as paint_pixel.
void FaceEditor::draw_line(int x0, int y0, int x1, int y1) {
    int dx =  std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        paint_brush(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Filled rectangle (inclusive of both corners). Brush size doesn't widen
// the result here — the rect is itself the shape; instead we fill every
// cell in the bounding box of (x0,y0)-(x1,y1).
void FaceEditor::draw_rect_filled(int x0, int y0, int x1, int y1) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            paint_pixel(x, y);
}

void FaceEditor::eyedrop_at(int x, int y) {
    if (mode_ != Mode::Color || palette_.empty()) return;
    if (!inside_covered(x, y)) return;
    const cv::Vec4b pix = canvas_.at<cv::Vec4b>(y, x);
    if (pix[3] == 0) return;   // transparent — nothing to pick
    // Choose the palette entry closest to the sampled pixel (sum of squared
    // channel differences). Keeps the picker self-contained — no "custom
    // colour" slot needed yet.
    int best = 0;
    int best_d = std::numeric_limits<int>::max();
    for (size_t i = 0; i < palette_.size(); ++i) {
        const int pr = (palette_[i] >> 16) & 0xFF;
        const int pg = (palette_[i] >>  8) & 0xFF;
        const int pb =  palette_[i]        & 0xFF;
        const int dr = pr - pix[0];
        const int dg = pg - pix[1];
        const int db = pb - pix[2];
        const int d  = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = static_cast<int>(i); }
    }
    palette_idx_ = best;
}

void FaceEditor::apply_at_cursor() {
    if (tool_ == Tool::Eyedrop) {
        // Sampling doesn't mutate the canvas — no undo push, no mirror.
        eyedrop_at(cursor_x_, cursor_y_);
        return;
    }

    // Eye Region: a free-form closed polygon drawn point-by-point (like the
    // line tool, but the ends join up). Each primary() drops a vertex; clicking
    // back on (or next to) the first vertex with >= 3 points closes the shape
    // and records it. With mirror on, closing also adds the mirrored polygon so
    // one shape sets both eyes. With mirror off the first closed shape is the
    // left eye, the second the right, and a third resets.
    if (tool_ == Tool::EyeBox) {
        // Close when the cursor lands on the first vertex and we have a real
        // polygon (>= 3 points). Otherwise append this vertex.
        const bool can_close =
            eye_pts_.size() >= 3 &&
            cursor_x_ == eye_pts_.front().x && cursor_y_ == eye_pts_.front().y;
        if (!can_close) {
            // Avoid duplicate consecutive vertices (a double-click on one cell).
            if (eye_pts_.empty() ||
                eye_pts_.back().x != cursor_x_ || eye_pts_.back().y != cursor_y_)
                eye_pts_.emplace_back(cursor_x_, cursor_y_);
            return;
        }
        EyePoly poly = eye_pts_;
        eye_pts_.clear();
        if (mirror_) {
            const int axis = (mirror_axis_x_ >= 0)
                ? (2 * mirror_axis_x_ - 1)
                : (2 * bbox_.x + bbox_.width - 1);
            EyePoly mpoly;
            mpoly.reserve(poly.size());
            for (const auto& p : poly) mpoly.emplace_back(axis - p.x, p.y);
            eye_polys_ = { std::move(poly), std::move(mpoly) };
        } else {
            if (eye_polys_.size() >= 2) eye_polys_.clear();
            eye_polys_.push_back(std::move(poly));
        }
        return;
    }

    // Line / Rect are two-step: first primary() sets the anchor, second
    // primary() commits the shape from anchor to cursor. Same behaviour
    // every standard pixel editor has.
    if (tool_ == Tool::Line || tool_ == Tool::Rect) {
        if (!anchor_set_) {
            anchor_set_ = true;
            anchor_x_   = cursor_x_;
            anchor_y_   = cursor_y_;
            return;
        }
        push_undo();
        // Mirror axis: if the caller passed a nose-centre fence
        // (mirror_axis_x_ >= 0) use that; otherwise fall back to the
        // bbox horizontal centre. fence formula: mirrored = 2*axis - 1 - x.
        const int axis = (mirror_axis_x_ >= 0)
            ? (2 * mirror_axis_x_ - 1)
            : (2 * bbox_.x + bbox_.width - 1);
        const int mx_anchor = axis - anchor_x_;
        const int mx_cursor = axis - cursor_x_;
        const bool do_mirror = mirror_ &&
            (mx_anchor != anchor_x_ || mx_cursor != cursor_x_);
        if (tool_ == Tool::Line) {
            draw_line(anchor_x_, anchor_y_, cursor_x_, cursor_y_);
            if (do_mirror) draw_line(mx_anchor, anchor_y_, mx_cursor, cursor_y_);
        } else {
            draw_rect_filled(anchor_x_, anchor_y_, cursor_x_, cursor_y_);
            if (do_mirror) draw_rect_filled(mx_anchor, anchor_y_, mx_cursor, cursor_y_);
        }
        anchor_set_ = false;
        return;
    }

    push_undo();
    const int axis = (mirror_axis_x_ >= 0)
        ? (2 * mirror_axis_x_ - 1)
        : (2 * bbox_.x + bbox_.width - 1);
    const int mirror_x = axis - cursor_x_;
    const bool do_mirror = mirror_ && mirror_x != cursor_x_;
    if (tool_ == Tool::Bucket) {
        flood_fill(cursor_x_, cursor_y_);
        if (do_mirror) flood_fill(mirror_x, cursor_y_);
        return;
    }
    paint_brush(cursor_x_, cursor_y_);
    if (do_mirror) paint_brush(mirror_x, cursor_y_);
}

void FaceEditor::cursor_step(int dx, int dy) {
    if (!open_) return;
    cursor_x_ = std::clamp(cursor_x_ + dx, bbox_.x, bbox_.x + bbox_.width  - 1);
    cursor_y_ = std::clamp(cursor_y_ + dy, bbox_.y, bbox_.y + bbox_.height - 1);
}

void FaceEditor::primary()   { if (open_) apply_at_cursor(); }

void FaceEditor::secondary() {
    if (!open_) return;
    // Cycle Pencil → Eraser → Bucket → Eyedrop → Line → Rect → Pencil…
    tool_ = static_cast<Tool>((static_cast<int>(tool_) + 1) % kToolCount);
    anchor_set_ = false;
    eye_pts_.clear();
}

void FaceEditor::tertiary()  { if (open_) mirror_ = !mirror_; }

void FaceEditor::set_tool(Tool t) {
    if (!open_) return;
    tool_ = t;
    anchor_set_ = false;
    eye_pts_.clear();
}

void FaceEditor::set_brush_size(int radius) {
    if (!open_) return;
    if (radius < 0) radius = 0;
    if (radius > 2) radius = 2;
    brush_size_ = radius;
}

void FaceEditor::back() {
    if (!open_) return;
    // A pending line/rect anchor consumes one back press without closing
    // the editor — same UX as cancelling a shape in Aseprite / Krita.
    if (anchor_set_) {
        anchor_set_ = false;
        return;
    }
    // A partially-drawn eye polygon: back removes the last vertex (and a final
    // press on the empty shape falls through to closing the editor).
    if (!eye_pts_.empty()) {
        eye_pts_.pop_back();
        return;
    }
    auto cb = std::move(on_cancel_);
    close();
    if (cb) cb();
}

void FaceEditor::save() {
    if (!open_) return;
    auto cb     = std::move(on_commit_);
    cv::Mat out = canvas_.clone();
    const std::string path = abs_path_;
    std::vector<EyePoly> eyes = eye_polys_;
    close();
    if (cb) cb(out, path, eyes);
}

void FaceEditor::cycle_palette(int dir) {
    if (!open_ || mode_ != Mode::Color || palette_.empty()) return;
    const int n = static_cast<int>(palette_.size());
    palette_idx_ = ((palette_idx_ + dir) % n + n) % n;
}

void FaceEditor::preview() {
    if (!open_ || !on_preview_ || canvas_.empty()) return;
    on_preview_(canvas_, preview_duration_s_);
}

void FaceEditor::toggle_live() {
    if (!open_ || !live_frame_) return;
    live_mode_ = !live_mode_;
    // When entering live mode, also push the current canvas so the next
    // few rendered frames already reflect the user's in-progress work.
    if (live_mode_ && on_preview_)
        on_preview_(canvas_, std::max(1.5, preview_duration_s_));
}

void FaceEditor::undo() {
    if (!open_) return;
    // Z while a shape anchor is set just cancels it (no canvas change to
    // roll back). Otherwise pop the most recent undo snapshot.
    if (anchor_set_) { anchor_set_ = false; return; }
    if (!eye_pts_.empty()) { eye_pts_.pop_back(); return; }
    if (undo_stack_.empty()) return;
    canvas_ = std::move(undo_stack_.back());
    undo_stack_.pop_back();
}

void FaceEditor::mouse_move(float mx, float my) {
    if (!open_ || cell_size_ <= 0.f) return;
    const int px = static_cast<int>((mx - grid_origin_x_) / cell_size_) + bbox_.x;
    const int py = static_cast<int>((my - grid_origin_y_) / cell_size_) + bbox_.y;
    if (px < bbox_.x || px >= bbox_.x + bbox_.width)  return;
    if (py < bbox_.y || py >= bbox_.y + bbox_.height) return;
    cursor_x_ = px;
    cursor_y_ = py;
}

void FaceEditor::mouse_down(float mx, float my) {
    mouse_move(mx, my);
    primary();
}

// Held-button drag. Only the freehand brushes paint on drag (continuing the
// stroke that the press-edge primary() began, so no extra undo entries and no
// re-triggered anchor/commit on two-step tools). Other tools just track the
// cursor so the live preview follows the pointer.
void FaceEditor::mouse_drag(float mx, float my) {
    if (!open_) return;
    if (tool_ != Tool::Pencil && tool_ != Tool::Eraser) { mouse_move(mx, my); return; }
    const int px = cursor_x_, py = cursor_y_;
    mouse_move(mx, my);
    if (cursor_x_ == px && cursor_y_ == py) return;   // same cell — nothing to add
    draw_line(px, py, cursor_x_, cursor_y_);          // connect cells, brush-aware
    if (mirror_) {
        const int axis = (mirror_axis_x_ >= 0)
            ? (2 * mirror_axis_x_ - 1) : (2 * bbox_.x + bbox_.width - 1);
        draw_line(axis - px, py, axis - cursor_x_, cursor_y_);
    }
}

void FaceEditor::draw(ImDrawList* dl, ImFont* font, float fs,
                      float W, float H, ImU32 accent) {
    if (!open_) return;

    // Dim + panel chrome (same look as the file picker).
    dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(4, 8, 12, 175));
    const float mx = W * 0.05f, my = H * 0.06f;
    const ImVec2 pmin{mx, my}, pmax{W - mx, H - my};
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 230));
    dl->AddRect      (pmin, pmax, accent, 0.f, 0, 2.f);

    const float pad = 22.f;
    const float cx0 = pmin.x + pad;
    const float cx1 = pmax.x - pad;
    float cy = pmin.y + 14.f;

    // Title.
    dl->AddText(font, fs * 1.6f, {cx0, cy}, IM_COL32(255, 255, 255, 255),
                title_.c_str());
    cy += fs * 1.6f + 6.f;

    // Subtitle: dims + cursor + tool + brush + mirror + mode.
    char sub[192];
    const char* tool_str = "Pencil";
    switch (tool_) {
    case Tool::Pencil:  tool_str = "Pencil";  break;
    case Tool::Eraser:  tool_str = "Eraser";  break;
    case Tool::Bucket:  tool_str = "Bucket";  break;
    case Tool::Eyedrop: tool_str = "Eyedrop"; break;
    case Tool::Line:    tool_str = anchor_set_ ? "Line (anchor set)"   : "Line";    break;
    case Tool::Rect:    tool_str = anchor_set_ ? "Rect (anchor set)"   : "Rect";    break;
    case Tool::EyeBox:  tool_str = eye_pts_.empty() ? "Eye Region"
                                                    : "Eye Region (drawing)"; break;
    }
    const int brush_side = 1 + brush_size_ * 2;
    std::snprintf(sub, sizeof(sub),
        "%dx%d  cursor (%d,%d)  %s  brush %dx%d%s%s%s",
        bbox_.width, bbox_.height,
        cursor_x_, cursor_y_,
        tool_str, brush_side, brush_side,
        mirror_ ? "  Mirror" : "",
        mode_ == Mode::Color ? "  Color" : "  Mono",
        live_mode_ ? "  Live" : "");
    dl->AddText(font, fs * 0.9f, {cx0, cy}, IM_COL32(170, 180, 190, 230), sub);
    cy += fs * 0.9f + 10.f;
    dl->AddLine({cx0, cy}, {cx1, cy}, IM_COL32(255, 255, 255, 60), 1.f);
    cy += 10.f;

    // Footer reservation — each control hint sits on its own line for
    // readability. Two columns: general controls on the left, the six
    // numbered tools on the right. Height tracks the taller column.
    const float footer_line_h = fs * 0.95f + 2.f;
    const int   left_lines    = (mode_ == Mode::Color) ? 10 : 9;
    const int   right_lines   = 7;   // 1..7 tool bindings
    const int   footer_lines  = std::max(left_lines, right_lines);
    const float footer_h      = footer_line_h * footer_lines + 12.f;
    // Palette strip (color mode) above the footer.
    const float palette_h = (mode_ == Mode::Color) ? (fs * 1.4f + 14.f) : 0.f;
    const float grid_bot  = pmax.y - footer_h - palette_h - 8.f;

    // Fit the bbox into the remaining rect at integer cell size.
    if (bbox_.width > 0 && bbox_.height > 0) {
        const float avail_w = cx1 - cx0;
        const float avail_h = grid_bot - cy;
        const float cell_w = avail_w / bbox_.width;
        const float cell_h = avail_h / bbox_.height;
        cell_size_ = std::max(2.f, std::floor(std::min(cell_w, cell_h)));
        const float gw = cell_size_ * bbox_.width;
        const float gh = cell_size_ * bbox_.height;
        grid_origin_x_ = cx0 + (avail_w - gw) * 0.5f;
        grid_origin_y_ = cy  + (avail_h - gh) * 0.5f;

        // Background: dark with a slight tint inside the bbox.
        dl->AddRectFilled({grid_origin_x_, grid_origin_y_},
                          {grid_origin_x_ + gw, grid_origin_y_ + gh},
                          IM_COL32(16, 20, 28, 255));

        // Draw covered regions slightly brighter so the "lit" area pops.
        for (const auto& r : covered_) {
            const float rx = grid_origin_x_ + (r.x - bbox_.x) * cell_size_;
            const float ry = grid_origin_y_ + (r.y - bbox_.y) * cell_size_;
            dl->AddRectFilled({rx, ry},
                              {rx + r.width  * cell_size_,
                               ry + r.height * cell_size_},
                              IM_COL32(24, 32, 44, 255));
        }

        // Live overlay — when enabled, keep pushing the user's in-progress
        // canvas to the controller as a transient face so its renderer
        // composites material + effects on top, then pull the rendered
        // (RGB) frame back for display in place of the per-pixel cells.
        // Falls through to the painted-canvas path if the controller hasn't
        // produced a frame yet.
        cv::Mat live;
        bool have_live = false;
        if (live_mode_) {
            if (on_preview_) on_preview_(canvas_, 1.5);   // refresh deadline
            if (live_frame_) have_live = live_frame_(live);
            if (have_live && live.type() != CV_8UC3) {
                cv::Mat tmp;
                live.convertTo(tmp, CV_8UC3);
                live = std::move(tmp);
            }
        }

        // Per-pixel cells. In live mode we draw every covered cell from the
        // rendered (RGB) frame; otherwise we draw the painted RGBA pixels and
        // skip transparency so the bbox tint shows through.
        for (int py = 0; py < bbox_.height; ++py) {
            for (int px = 0; px < bbox_.width; ++px) {
                const int cx_canvas = bbox_.x + px;
                const int cy_canvas = bbox_.y + py;
                if (!inside_covered(cx_canvas, cy_canvas)) continue;
                ImU32 col;
                if (have_live &&
                    cx_canvas >= 0 && cx_canvas < live.cols &&
                    cy_canvas >= 0 && cy_canvas < live.rows) {
                    const cv::Vec3b& p = live.at<cv::Vec3b>(cy_canvas, cx_canvas);
                    col = IM_COL32(p[0], p[1], p[2], 255);
                } else {
                    const cv::Vec4b& pix = canvas_.at<cv::Vec4b>(cy_canvas, cx_canvas);
                    if (pix[3] == 0) continue;   // transparent — skip
                    col = IM_COL32(pix[0], pix[1], pix[2], pix[3]);
                }
                const float rx = grid_origin_x_ + px * cell_size_;
                const float ry = grid_origin_y_ + py * cell_size_;
                dl->AddRectFilled({rx, ry},
                                  {rx + cell_size_, ry + cell_size_}, col);
            }
        }

        // Grid lines (only inside covered cells so we don't clutter the
        // un-addressable area).
        const ImU32 grid_col = IM_COL32(255, 255, 255, 25);
        for (int gx = 0; gx <= bbox_.width;  ++gx) {
            const float xx = grid_origin_x_ + gx * cell_size_;
            dl->AddLine({xx, grid_origin_y_}, {xx, grid_origin_y_ + gh},
                        grid_col, 1.f);
        }
        for (int gy = 0; gy <= bbox_.height; ++gy) {
            const float yy = grid_origin_y_ + gy * cell_size_;
            dl->AddLine({grid_origin_x_, yy}, {grid_origin_x_ + gw, yy},
                        grid_col, 1.f);
        }

        // Per-zone bounding boxes — outline each chain rect in a bright
        // contrast colour (full-alpha yellow) and stamp its name above the
        // rect (or just inside the top edge if the rect is flush with the
        // grid top). Inset by 1 px so the outline stays visible when a
        // rect sits against the grid edge (the fallback single-rect case
        // where covered == full canvas).
        const ImU32 bbox_col  = IM_COL32(255, 220, 60, 255);
        const ImU32 label_col = IM_COL32(255, 240, 130, 255);
        const ImU32 label_bg  = IM_COL32(0, 0, 0, 170);
        const float label_fs  = std::max(10.f, fs * 0.65f);
        for (size_t i = 0; i < covered_.size(); ++i) {
            const auto& r = covered_[i];
            const float rx = grid_origin_x_ + (r.x - bbox_.x) * cell_size_ + 1.f;
            const float ry = grid_origin_y_ + (r.y - bbox_.y) * cell_size_ + 1.f;
            const float rw = r.width  * cell_size_ - 2.f;
            const float rh = r.height * cell_size_ - 2.f;
            dl->AddRect({rx, ry}, {rx + rw, ry + rh}, bbox_col, 0.f, 0, 3.f);

            const std::string& lbl = (i < covered_labels_.size()) ? covered_labels_[i]
                                                                   : std::string();
            if (lbl.empty()) continue;
            const ImVec2 ts = font->CalcTextSizeA(label_fs, FLT_MAX, 0.f, lbl.c_str());
            // Prefer just above the rect; if there isn't room above the
            // grid, drop the label inside the top of the rect.
            const float above_y = ry - ts.y - 4.f;
            const bool fits_above = above_y >= grid_origin_y_ + 1.f;
            const float tx = rx + 4.f;
            const float ty = fits_above ? above_y : (ry + 3.f);
            dl->AddRectFilled({tx - 3.f, ty - 1.f},
                              {tx + ts.x + 3.f, ty + ts.y + 1.f},
                              label_bg, 2.f);
            dl->AddText(font, label_fs, {tx, ty}, label_col, lbl.c_str());
        }

        // Existing eye-region polygons (where a blink replaces the open eye).
        // Always drawn — cyan closed outline + a small label + faint fill — so
        // the user can see where the eyes / blink area sit regardless of the
        // active tool. Vertices are marked so a saved shape stays editable.
        {
            // Canvas pixel (col,row) → screen point at cell centre.
            auto to_screen = [&](int cxp, int cyp) -> ImVec2 {
                return { grid_origin_x_ + (cxp - bbox_.x + 0.5f) * cell_size_,
                         grid_origin_y_ + (cyp - bbox_.y + 0.5f) * cell_size_ };
            };
            const ImU32 eye_col  = IM_COL32(80, 220, 255, 235);
            const ImU32 eye_fill = IM_COL32(80, 220, 255, 40);
            const ImU32 vtx_col  = IM_COL32(200, 245, 255, 255);
            const float eye_fs   = std::max(10.f, fs * 0.62f);
            for (size_t i = 0; i < eye_polys_.size(); ++i) {
                const auto& poly = eye_polys_[i];
                if (poly.empty()) continue;
                std::vector<ImVec2> pts;
                pts.reserve(poly.size());
                for (const auto& p : poly) pts.push_back(to_screen(p.x, p.y));
                if (pts.size() >= 3)
                    dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), eye_fill);
                dl->AddPolyline(pts.data(), (int)pts.size(), eye_col,
                                ImDrawFlags_Closed, 2.f);
                for (const auto& sp : pts)
                    dl->AddRectFilled({sp.x - 1.5f, sp.y - 1.5f},
                                      {sp.x + 1.5f, sp.y + 1.5f}, vtx_col);
                const char* lbl = (eye_polys_.size() == 2)
                                  ? (i == 0 ? "eye L" : "eye R") : "eye";
                dl->AddText(font, eye_fs, {pts.front().x + 3.f, pts.front().y + 3.f},
                            eye_col, lbl);
            }
        }

        // Live preview for the EyeBox tool while a polygon is being placed:
        // draw the edges between dropped vertices, a rubber-band edge to the
        // cursor, and highlight the first vertex (the close target) once the
        // shape has enough points to close.
        if (tool_ == Tool::EyeBox && !eye_pts_.empty()) {
            auto to_screen = [&](int cxp, int cyp) -> ImVec2 {
                return { grid_origin_x_ + (cxp - bbox_.x + 0.5f) * cell_size_,
                         grid_origin_y_ + (cyp - bbox_.y + 0.5f) * cell_size_ };
            };
            const ImU32 edge_col = IM_COL32(120, 235, 255, 255);
            const ImU32 band_col = IM_COL32(120, 235, 255, 140);
            std::vector<ImVec2> pts;
            pts.reserve(eye_pts_.size());
            for (const auto& p : eye_pts_) pts.push_back(to_screen(p.x, p.y));
            if (pts.size() >= 2)
                dl->AddPolyline(pts.data(), (int)pts.size(), edge_col,
                                ImDrawFlags_None, 2.f);
            // Rubber-band edge from the last placed vertex to the cursor.
            dl->AddLine(pts.back(), to_screen(cursor_x_, cursor_y_), band_col, 1.5f);
            // Vertex dots; ring the first one when the shape can close.
            for (size_t k = 0; k < pts.size(); ++k) {
                dl->AddRectFilled({pts[k].x - 1.5f, pts[k].y - 1.5f},
                                  {pts[k].x + 1.5f, pts[k].y + 1.5f}, edge_col);
            }
            if (eye_pts_.size() >= 3)
                dl->AddCircle(pts.front(), cell_size_ * 0.8f,
                              IM_COL32(255, 240, 130, 255), 0, 2.f);
            if (mirror_) {
                const int axis = (mirror_axis_x_ >= 0)
                    ? (2 * mirror_axis_x_ - 1) : (2 * bbox_.x + bbox_.width - 1);
                std::vector<ImVec2> mpts;
                mpts.reserve(eye_pts_.size());
                for (const auto& p : eye_pts_) mpts.push_back(to_screen(axis - p.x, p.y));
                if (mpts.size() >= 2)
                    dl->AddPolyline(mpts.data(), (int)mpts.size(),
                                    IM_COL32(120, 235, 255, 110), ImDrawFlags_None, 1.5f);
            }
        }

        // Live anchor preview for Line / Rect tools (shape that would be
        // committed by the next primary()). Translucent so the underlying
        // canvas pixels still read through.
        if (anchor_set_ && (tool_ == Tool::Line || tool_ == Tool::Rect)) {
            const ImU32 preview_col = (accent & 0x00FFFFFFu) | (110u << 24);
            auto stamp_cell = [&](int gx, int gy) {
                if (gx < bbox_.x || gx >= bbox_.x + bbox_.width)  return;
                if (gy < bbox_.y || gy >= bbox_.y + bbox_.height) return;
                const float rx = grid_origin_x_ + (gx - bbox_.x) * cell_size_;
                const float ry = grid_origin_y_ + (gy - bbox_.y) * cell_size_;
                dl->AddRectFilled({rx, ry}, {rx + cell_size_, ry + cell_size_},
                                  preview_col);
            };
            if (tool_ == Tool::Line) {
                int x0 = anchor_x_, y0 = anchor_y_;
                int x1 = cursor_x_, y1 = cursor_y_;
                int dx =  std::abs(x1 - x0);
                int dy = -std::abs(y1 - y0);
                int sx = (x0 < x1) ? 1 : -1;
                int sy = (y0 < y1) ? 1 : -1;
                int err = dx + dy;
                for (;;) {
                    stamp_cell(x0, y0);
                    if (x0 == x1 && y0 == y1) break;
                    const int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x0 += sx; }
                    if (e2 <= dx) { err += dx; y0 += sy; }
                }
            } else {
                int x0 = std::min(anchor_x_, cursor_x_);
                int y0 = std::min(anchor_y_, cursor_y_);
                int x1 = std::max(anchor_x_, cursor_x_);
                int y1 = std::max(anchor_y_, cursor_y_);
                for (int gy = y0; gy <= y1; ++gy)
                    for (int gx = x0; gx <= x1; ++gx)
                        stamp_cell(gx, gy);
            }
            // Anchor marker: solid outline on the anchor cell.
            const float ax = grid_origin_x_ + (anchor_x_ - bbox_.x) * cell_size_;
            const float ay = grid_origin_y_ + (anchor_y_ - bbox_.y) * cell_size_;
            dl->AddRect({ax, ay}, {ax + cell_size_, ay + cell_size_},
                        accent, 0.f, 0, 1.5f);
        }

        // Cursor — sized to the active brush footprint so the user sees
        // how wide a stroke will land. Brush sizing only applies to the
        // pixel-painting tools (Pencil / Eraser / Line / Rect); Bucket
        // and Eyedrop are single-cell point operations regardless of
        // brush, so their cursor stays 1 cell.
        const bool brush_sized_tool =
            tool_ == Tool::Pencil || tool_ == Tool::Eraser ||
            tool_ == Tool::Line   || tool_ == Tool::Rect;
        const int cur_radius = brush_sized_tool ? brush_size_ : 0;
        const int cur_side   = 1 + cur_radius * 2;
        const float ccx = grid_origin_x_ + (cursor_x_ - bbox_.x - cur_radius) * cell_size_;
        const float ccy = grid_origin_y_ + (cursor_y_ - bbox_.y - cur_radius) * cell_size_;
        const float ccs = cell_size_ * static_cast<float>(cur_side);
        // Dark halo first (one px outside + inside), then the accent box on top.
        // The grey/black outline keeps the cursor readable over white paint —
        // a plain white accent would vanish against lit pixels.
        const ImU32 cursor_halo = IM_COL32(20, 20, 24, 230);
        dl->AddRect({ccx - 1.5f, ccy - 1.5f}, {ccx + ccs + 1.5f, ccy + ccs + 1.5f},
                    cursor_halo, 0.f, 0, 1.5f);
        dl->AddRect({ccx + 1.5f, ccy + 1.5f}, {ccx + ccs - 1.5f, ccy + ccs - 1.5f},
                    cursor_halo, 0.f, 0, 1.5f);
        dl->AddRect({ccx, ccy}, {ccx + ccs, ccy + ccs},
                    accent, 0.f, 0, 2.f);
        // Mirror cursor preview — same brush footprint, mirrored around axis.
        if (mirror_) {
            const int axis_pix = (mirror_axis_x_ >= 0)
                ? (2 * mirror_axis_x_ - 1)
                : (2 * bbox_.x + bbox_.width - 1);
            const int mirror_canvas_x = axis_pix - cursor_x_;
            if (mirror_canvas_x != cursor_x_ &&
                mirror_canvas_x >= bbox_.x &&
                mirror_canvas_x <  bbox_.x + bbox_.width) {
                const float mcx = grid_origin_x_ + (mirror_canvas_x - bbox_.x - cur_radius) * cell_size_;
                dl->AddRect({mcx - 1.5f, ccy - 1.5f}, {mcx + ccs + 1.5f, ccy + ccs + 1.5f},
                            cursor_halo, 0.f, 0, 1.5f);
                dl->AddRect({mcx, ccy}, {mcx + ccs, ccy + ccs},
                            (accent & 0x00FFFFFFu) | (140u << 24), 0.f, 0, 2.f);
            }
        }
    }

    // Palette strip (color mode).
    if (mode_ == Mode::Color && !palette_.empty()) {
        const float py = pmax.y - footer_h - palette_h + 8.f;
        const float sz = palette_h - 14.f;
        const float spacing = 6.f;
        float px = cx0;
        for (size_t i = 0; i < palette_.size(); ++i) {
            const ImU32 col = imu32_from_hex(palette_[i]);
            dl->AddRectFilled({px, py}, {px + sz, py + sz}, col, 3.f);
            if (static_cast<int>(i) == palette_idx_)
                dl->AddRect({px - 2.f, py - 2.f},
                            {px + sz + 2.f, py + sz + 2.f},
                            accent, 3.f, 0, 2.f);
            px += sz + spacing;
            if (px + sz > cx1) break;
        }
    }

    // Footer hints — two columns, one key↔action per line for easy
    // scanning. Left column lists general controls; right column lists
    // the six numbered tool bindings (each spelled out). Matches what
    // menu_system.cpp polls when the editor is the active overlay.
    const char* left[] = {
        "Select       paint",
        "X            cycle tool",
        "-/+          brush size",
        "Y / M        mirror",
        "[ / ]        palette colour",   // skipped in mono mode
        "Z            undo",
        "V            preview to panels",
        "T            show live (effects)",
        "S            save",
        "Back         cancel",
    };
    const char* right[] = {
        "1            Pencil",
        "2            Eraser",
        "3            Bucket",
        "4            Eyedrop",
        "5            Line",
        "6            Rect",
        "7            Eye Region",
    };
    const ImU32 hint_col = IM_COL32(170, 185, 200, 220);
    const float text_size = footer_line_h - 2.f;
    const float left_x  = cx0;
    const float right_x = cx0 + (cx1 - cx0) * 0.55f;
    const float ftop    = pmax.y - footer_h + 6.f;
    float ly = ftop;
    for (size_t i = 0; i < sizeof(left) / sizeof(left[0]); ++i) {
        // Hide the palette-cycle line in mono mode (no palette to cycle).
        if (mode_ != Mode::Color && left[i][0] == '[') continue;
        dl->AddText(font, text_size, {left_x, ly}, hint_col, left[i]);
        ly += footer_line_h;
    }
    ly = ftop;
    for (size_t i = 0; i < sizeof(right) / sizeof(right[0]); ++i) {
        dl->AddText(font, text_size, {right_x, ly}, hint_col, right[i]);
        ly += footer_line_h;
    }
}

} // namespace menu
