#include "face_editor.h"

#include <algorithm>
#include <cstdio>

#include <opencv2/imgcodecs.hpp>

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
                     Mode mode,
                     std::vector<uint32_t> palette,
                     CommitFn on_commit,
                     CancelFn on_cancel) {
    if (canvas_w <= 0 || canvas_h <= 0) return;

    title_     = std::move(title);
    abs_path_  = std::move(abs_path);
    canvas_w_  = canvas_w;
    canvas_h_  = canvas_h;
    covered_   = std::move(covered_regions);
    mode_      = mode;
    tool_      = Tool::Pencil;
    mirror_    = false;
    on_commit_ = std::move(on_commit);
    on_cancel_ = std::move(on_cancel);

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

    open_ = true;
}

void FaceEditor::close() {
    open_      = false;
    on_commit_ = nullptr;
    on_cancel_ = nullptr;
    canvas_    = cv::Mat();
    undo_stack_.clear();
    covered_.clear();
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

void FaceEditor::apply_at_cursor() {
    push_undo();
    paint_pixel(cursor_x_, cursor_y_);
    if (mirror_) {
        // Mirror across the vertical centre of the bbox.
        const int mirror_x = bbox_.x + (bbox_.x + bbox_.width - 1) - cursor_x_;
        if (mirror_x != cursor_x_) paint_pixel(mirror_x, cursor_y_);
    }
}

void FaceEditor::cursor_step(int dx, int dy) {
    if (!open_) return;
    cursor_x_ = std::clamp(cursor_x_ + dx, bbox_.x, bbox_.x + bbox_.width  - 1);
    cursor_y_ = std::clamp(cursor_y_ + dy, bbox_.y, bbox_.y + bbox_.height - 1);
}

void FaceEditor::primary()   { if (open_) apply_at_cursor(); }
void FaceEditor::secondary() { if (open_) tool_ = (tool_ == Tool::Pencil) ? Tool::Eraser : Tool::Pencil; }
void FaceEditor::tertiary()  { if (open_) mirror_ = !mirror_; }

void FaceEditor::back() {
    if (!open_) return;
    auto cb = std::move(on_cancel_);
    close();
    if (cb) cb();
}

void FaceEditor::save() {
    if (!open_) return;
    auto cb     = std::move(on_commit_);
    cv::Mat out = canvas_.clone();
    const std::string path = abs_path_;
    close();
    if (cb) cb(out, path);
}

void FaceEditor::cycle_palette(int dir) {
    if (!open_ || mode_ != Mode::Color || palette_.empty()) return;
    const int n = static_cast<int>(palette_.size());
    palette_idx_ = ((palette_idx_ + dir) % n + n) % n;
}

void FaceEditor::undo() {
    if (!open_ || undo_stack_.empty()) return;
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

    // Subtitle: dims + cursor + tool + mirror + palette idx.
    char sub[160];
    const char* tool_str = (tool_ == Tool::Pencil) ? "Pencil" : "Eraser";
    std::snprintf(sub, sizeof(sub),
        "%dx%d  cursor (%d,%d)  %s%s%s",
        bbox_.width, bbox_.height,
        cursor_x_, cursor_y_,
        tool_str, mirror_ ? "  Mirror" : "",
        mode_ == Mode::Color ? "  Color" : "  Mono");
    dl->AddText(font, fs * 0.9f, {cx0, cy}, IM_COL32(170, 180, 190, 230), sub);
    cy += fs * 0.9f + 10.f;
    dl->AddLine({cx0, cy}, {cx1, cy}, IM_COL32(255, 255, 255, 60), 1.f);
    cy += 10.f;

    // Footer reservation.
    const float footer_h = fs * 1.0f + 14.f;
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

        // Per-pixel cells.
        for (int py = 0; py < bbox_.height; ++py) {
            for (int px = 0; px < bbox_.width; ++px) {
                const int cx_canvas = bbox_.x + px;
                const int cy_canvas = bbox_.y + py;
                if (!inside_covered(cx_canvas, cy_canvas)) continue;
                const cv::Vec4b& pix = canvas_.at<cv::Vec4b>(cy_canvas, cx_canvas);
                if (pix[3] == 0) continue;   // transparent — skip
                const float rx = grid_origin_x_ + px * cell_size_;
                const float ry = grid_origin_y_ + py * cell_size_;
                const ImU32 col = IM_COL32(pix[0], pix[1], pix[2], pix[3]);
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

        // Cursor: filled outline at the cell.
        const float ccx = grid_origin_x_ + (cursor_x_ - bbox_.x) * cell_size_;
        const float ccy = grid_origin_y_ + (cursor_y_ - bbox_.y) * cell_size_;
        dl->AddRect({ccx, ccy}, {ccx + cell_size_, ccy + cell_size_},
                    accent, 0.f, 0, 2.f);
        // Mirror cursor preview.
        if (mirror_) {
            const int mirror_canvas_x = bbox_.x + (bbox_.x + bbox_.width - 1) - cursor_x_;
            if (mirror_canvas_x != cursor_x_) {
                const float mcx = grid_origin_x_ + (mirror_canvas_x - bbox_.x) * cell_size_;
                dl->AddRect({mcx, ccy}, {mcx + cell_size_, ccy + cell_size_},
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

    // Footer hints.
    const char* hints =
        (mode_ == Mode::Color)
        ? "Select: paint    X: tool    Y: mirror    LB/RB: color    Z: undo    S: save    Back: cancel"
        : "Select: paint    X: tool    Y: mirror    Z: undo    S: save    Back: cancel";
    dl->AddText(font, fs * 0.9f, {cx0, pmax.y - footer_h + 6.f},
                IM_COL32(170, 185, 200, 220), hints);
}

} // namespace menu
