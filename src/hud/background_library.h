#pragma once
// ── background_library.h ────────────────────────────────────────────────────────
// A small library of background images for the startup landing page. Scans one or
// more directories (bundled defaults + a user folder), and lazily decodes the
// selected image into a GL texture via stb_image. GL calls happen only in
// texture() / on destroy, so call those on the GL thread.

#include <string>
#include <vector>
#include <GLES2/gl2.h>

class BackgroundLibrary {
public:
    BackgroundLibrary() = default;
    ~BackgroundLibrary();

    BackgroundLibrary(const BackgroundLibrary&)            = delete;
    BackgroundLibrary& operator=(const BackgroundLibrary&) = delete;

    // Scan the given directories (in order) for image files (png/jpg/jpeg/bmp).
    // Results are de-duplicated by basename and sorted; safe to call again to
    // refresh. Does NOT touch GL. The dir list is remembered for refresh().
    void scan(const std::vector<std::string>& dirs);

    // Re-run the previous scan with its remembered dirs. Used after an
    // import / delete so the library picks up file-system changes without
    // callers having to thread their original bg_dirs back in.
    void refresh();

    int count() const { return static_cast<int>(entries_.size()); }
    const std::string& name(int i) const;   // basename (no extension)
    const std::string& path(int i) const;   // full path

    int  current() const { return current_; }
    void set_current(int i);                 // clamps; lazy-reloads on texture()
    bool set_current_by_name(const std::string& name);  // true if found
    void next();
    void prev();

    // GL texture for the current background; 0 if there are none or load failed.
    // Lazily (re)loads when the selection changed. Call on the GL thread.
    GLuint texture();
    int width()  const { return cur_w_; }
    int height() const { return cur_h_; }

private:
    struct Entry { std::string name, path; };
    void load_current();   // GL

    std::vector<std::string> last_dirs_;   // remembered for refresh()
    std::vector<Entry> entries_;
    int    current_ = 0;
    bool   dirty_   = true;   // selection changed, texture needs (re)load
    GLuint tex_     = 0;
    int    cur_w_   = 0, cur_h_ = 0;
};
