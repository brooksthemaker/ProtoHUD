#include "file_picker.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace menu {

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

// Render a string truncated with an ellipsis if it overflows the given width
// at the given font/size. Returns the visible string.
std::string fit_text(const std::string& s, ImFont* font, float sz, float max_w) {
    if (max_w <= 0.f) return {};
    ImVec2 m = font->CalcTextSizeA(sz, FLT_MAX, 0.f, s.c_str());
    if (m.x <= max_w) return s;
    static const char ell[] = "...";
    const float ell_w = font->CalcTextSizeA(sz, FLT_MAX, 0.f, ell).x;
    // Binary-ish trim: shrink until it fits.
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        const float w = font->CalcTextSizeA(sz, FLT_MAX, 0.f,
                                            s.c_str(), s.c_str() + mid).x;
        if (w + ell_w <= max_w) lo = mid; else hi = mid - 1;
    }
    return s.substr(0, lo) + ell;
}
} // namespace

void FilePicker::open(std::string title,
                      std::string start_dir,
                      std::vector<std::string> extensions,
                      CommitFn on_commit,
                      CancelFn on_cancel) {
    title_     = std::move(title);
    on_commit_ = std::move(on_commit);
    on_cancel_ = std::move(on_cancel);
    exts_.clear();
    exts_.reserve(extensions.size());
    for (auto& e : extensions) exts_.push_back(to_lower(std::move(e)));

    // Fall back to $HOME / "/" if the requested start dir doesn't exist.
    std::error_code ec;
    if (start_dir.empty() || !fs::is_directory(start_dir, ec)) {
        const char* home = std::getenv("HOME");
        start_dir = home ? std::string(home) : std::string("/");
        if (!fs::is_directory(start_dir, ec)) start_dir = "/";
    }
    open_ = true;
    scan(std::move(start_dir));
}

void FilePicker::close() {
    open_      = false;
    on_commit_ = nullptr;
    on_cancel_ = nullptr;
    entries_.clear();
    cursor_ = 0;
    scroll_ = 0;
}

void FilePicker::cancel() {
    auto cb = std::move(on_cancel_);   // capture before close clears it
    close();
    if (cb) cb();
}

void FilePicker::step(int dir) {
    if (!open_ || entries_.empty()) return;
    const int n = static_cast<int>(entries_.size());
    cursor_ = ((cursor_ + dir) % n + n) % n;
}

void FilePicker::activate() {
    if (!open_ || entries_.empty()) return;
    const Entry& e = entries_[cursor_];

    if (e.name == "..") {
        std::error_code ec;
        fs::path p = fs::path(cur_dir_).parent_path();
        if (p.empty()) p = "/";
        scan(p.string());
        return;
    }
    if (e.is_dir) {
        scan((fs::path(cur_dir_) / e.name).string());
        return;
    }
    const std::string abs = (fs::path(cur_dir_) / e.name).string();
    auto cb = std::move(on_commit_);   // capture before close clears it
    close();
    if (cb) cb(abs);
}

void FilePicker::back() {
    if (!open_) return;
    if (cur_dir_ == "/" || cur_dir_.empty()) { cancel(); return; }
    std::error_code ec;
    fs::path p = fs::path(cur_dir_).parent_path();
    if (p.empty()) p = "/";
    scan(p.string());
}

void FilePicker::scan(std::string dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) dir = "/";

    std::vector<Entry> dirs, files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); ++it) {
        const auto& path = it->path();
        std::string name = path.filename().string();
        if (name.empty() || name[0] == '.') continue;   // hide dotfiles
        std::error_code ec2;
        if (fs::is_directory(path, ec2)) {
            dirs.push_back({std::move(name), true});
        } else if (fs::is_regular_file(path, ec2)) {
            if (!exts_.empty()) {
                std::string ext = to_lower(path.extension().string());
                bool match = false;
                for (const auto& x : exts_) if (ext == x) { match = true; break; }
                if (!match) continue;
            }
            files.push_back({std::move(name), false});
        }
    }
    auto by_name = [](const Entry& a, const Entry& b){ return a.name < b.name; };
    std::sort(dirs.begin(),  dirs.end(),  by_name);
    std::sort(files.begin(), files.end(), by_name);

    entries_.clear();
    if (dir != "/") entries_.push_back({"..", true});
    for (auto& d : dirs)  entries_.push_back(std::move(d));
    for (auto& f : files) entries_.push_back(std::move(f));

    cur_dir_ = std::move(dir);
    cursor_  = 0;
    scroll_  = 0;
}

void FilePicker::draw(ImDrawList* dl, ImFont* font, float fs,
                      float W, float H, ImU32 accent) {
    if (!open_) return;

    // Same dim + panel chrome the deep menu uses, so the picker reads as
    // a sibling overlay rather than a different visual context.
    dl->AddRectFilled({0.f, 0.f}, {W, H}, IM_COL32(4, 8, 12, 165));
    const float mx = W * 0.07f, my = H * 0.09f;
    const ImVec2 pmin{mx, my}, pmax{W - mx, H - my};
    dl->AddRectFilled(pmin, pmax, IM_COL32(8, 12, 16, 230));
    dl->AddRect      (pmin, pmax, accent, 0.f, 0, 2.f);

    const float pad = 24.f;
    const float cx0 = pmin.x + pad;
    const float cx1 = pmax.x - pad;
    float cy = pmin.y + 14.f;

    // Title
    dl->AddText(font, fs * 1.7f, {cx0, cy}, IM_COL32(255, 255, 255, 255),
                title_.c_str());
    cy += fs * 1.7f + 8.f;

    // Breadcrumb (fit-to-width)
    const std::string crumb = fit_text(cur_dir_, font, fs, cx1 - cx0);
    dl->AddText(font, fs, {cx0, cy}, IM_COL32(180, 195, 205, 220), crumb.c_str());
    cy += fs + 10.f;

    // Divider
    dl->AddLine({cx0, cy}, {cx1, cy}, IM_COL32(255, 255, 255, 60), 1.f);
    cy += 8.f;

    // Footer + reserved space for it (so the list never paints under the hints).
    const float footer_h = fs + 16.f;
    const float list_top = cy;
    const float list_bot = pmax.y - footer_h - 8.f;
    const float row_h    = fs * 1.35f;
    const int   visible  = std::max(1, static_cast<int>((list_bot - list_top) / row_h));

    // Scroll so cursor stays in view.
    if (cursor_ < scroll_)               scroll_ = cursor_;
    if (cursor_ >= scroll_ + visible)    scroll_ = cursor_ - visible + 1;
    if (scroll_ < 0)                     scroll_ = 0;

    if (entries_.empty()) {
        const char* msg = "No files in this folder.";
        const ImVec2 m = font->CalcTextSizeA(fs, FLT_MAX, 0.f, msg);
        dl->AddText(font, fs,
                    {cx0 + ((cx1 - cx0) - m.x) * 0.5f,
                     list_top + ((list_bot - list_top) - m.y) * 0.5f},
                    IM_COL32(180, 195, 205, 200), msg);
    } else {
        for (int i = 0; i < visible; ++i) {
            const int idx = scroll_ + i;
            if (idx >= static_cast<int>(entries_.size())) break;
            const Entry& e = entries_[idx];
            const float ry = list_top + i * row_h;
            const bool sel = (idx == cursor_);

            if (sel) {
                dl->AddRectFilled({cx0 - 4.f, ry - 2.f},
                                  {cx1 + 4.f, ry + row_h - 4.f},
                                  IM_COL32(255, 255, 255, 235));
            }

            const ImU32 fg = sel
                ? IM_COL32(10, 12, 16, 255)
                : (e.is_dir ? IM_COL32(160, 220, 255, 240)
                            : IM_COL32(225, 230, 235, 240));

            // Prefix dir entries so they read as folders at a glance.
            std::string label = e.is_dir ? (e.name + "/") : e.name;
            label = fit_text(label, font, fs, cx1 - cx0 - 12.f);
            dl->AddText(font, fs, {cx0 + 6.f, ry + 4.f}, fg, label.c_str());
        }

        // Right-edge scroll hint when overflow.
        if (static_cast<int>(entries_.size()) > visible) {
            char hint[32];
            std::snprintf(hint, sizeof(hint), "%d / %d",
                          cursor_ + 1, static_cast<int>(entries_.size()));
            const ImVec2 m = font->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0.f, hint);
            dl->AddText(font, fs * 0.85f, {cx1 - m.x, list_top - fs * 0.85f - 4.f},
                        IM_COL32(180, 195, 205, 200), hint);
        }
    }

    // Footer hints
    const char* hints = "Select: pick / enter folder    Back: up (closes at root)";
    dl->AddText(font, fs * 0.95f, {cx0, pmax.y - footer_h + 4.f},
                IM_COL32(170, 185, 200, 220), hints);
}

} // namespace menu
