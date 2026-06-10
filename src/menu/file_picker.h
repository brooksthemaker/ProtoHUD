#pragma once
// ── file_picker.h ─────────────────────────────────────────────────────────────
// Controller-driven file browser overlay used by MenuSystem to import media
// (GIFs, face images, later splash backgrounds, …) without a mouse or
// keyboard. open() configures the picker with a starting directory and a list
// of accepted extensions, and a callback fires with the chosen absolute path
// on commit.
//
// Input routing mirrors the OSK: while is_open() returns true MenuSystem
// forwards navigate/select/back to step/activate/back, and draw() replaces
// the normal deep-menu body. No knowledge of any specific media type lives
// here — the caller's commit callback does the copy-into-target-folder /
// manifest-bind work.

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "overlay.h"

namespace menu {

class FilePicker : public IOverlay {
public:
    using CommitFn = std::function<void(const std::string& abs_path)>;
    using CancelFn = std::function<void()>;

    // start_dir is where the picker opens; extensions are matched
    // case-insensitively against the file's full extension (with leading dot,
    // e.g. ".gif"). An empty extension list shows every regular file.
    void open(std::string title,
              std::string start_dir,
              std::vector<std::string> extensions,
              CommitFn on_commit,
              CancelFn on_cancel = {});

    void close() override;
    bool is_open() const override { return open_; }

    // Input — wired from MenuSystem (via IOverlay) when is_open() is true.
    void step(int dir) override;   // cursor up (-1) / down (+1)
    void activate() override;      // enter dir or pick file
    void back() override;          // up one dir, or cancel at fs root
    void cancel();                 // close without commit (fires on_cancel)

    // Full-screen overlay drawn in place of the deep menu when open.
    void draw(ImDrawList* dl, ImFont* font, float base_font_size,
              float screen_w, float screen_h, ImU32 accent_color) override;

    // Last directory navigated to — useful to remember per media type.
    const std::string& current_dir() const { return cur_dir_; }

private:
    void scan(std::string dir);

    struct Entry {
        std::string name;     // basename; dirs do *not* include a trailing slash
        bool        is_dir;
    };

    bool        open_ = false;
    std::string title_;
    std::string cur_dir_;
    std::vector<std::string> exts_;            // lower-case, leading dot
    CommitFn    on_commit_;
    CancelFn    on_cancel_;

    std::vector<Entry> entries_;
    int  cursor_ = 0;
    int  scroll_ = 0;     // first visible entry index
};

} // namespace menu
