#include "background_library.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

// Own static stb_image implementation (STB_IMAGE_STATIC keeps the symbols
// file-local so they don't collide with splash.cpp / nanovg / gif_player copies).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

namespace fs = std::filesystem;

BackgroundLibrary::~BackgroundLibrary() {
    if (tex_) glDeleteTextures(1, &tex_);
}

static bool is_image_ext(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

void BackgroundLibrary::scan(const std::vector<std::string>& dirs) {
    entries_.clear();
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::exists(d, ec) || !fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            if (!is_image_ext(e.path().extension().string())) continue;
            std::string nm = e.path().stem().string();
            // De-dup by basename (earlier dirs win — defaults before user dir).
            bool dup = false;
            for (auto& en : entries_) if (en.name == nm) { dup = true; break; }
            if (!dup) entries_.push_back({ nm, e.path().string() });
        }
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b){ return a.name < b.name; });
    if (current_ >= static_cast<int>(entries_.size())) current_ = 0;
    dirty_ = true;
}

const std::string& BackgroundLibrary::name(int i) const {
    static std::string empty;
    return (i >= 0 && i < count()) ? entries_[i].name : empty;
}
const std::string& BackgroundLibrary::path(int i) const {
    static std::string empty;
    return (i >= 0 && i < count()) ? entries_[i].path : empty;
}

void BackgroundLibrary::set_current(int i) {
    if (entries_.empty()) { current_ = 0; return; }
    int n = static_cast<int>(entries_.size());
    int c = ((i % n) + n) % n;
    if (c != current_) { current_ = c; dirty_ = true; }
}
bool BackgroundLibrary::set_current_by_name(const std::string& nm) {
    for (int i = 0; i < count(); ++i)
        if (entries_[i].name == nm) { set_current(i); return true; }
    return false;
}
void BackgroundLibrary::next() { if (!entries_.empty()) set_current(current_ + 1); }
void BackgroundLibrary::prev() { if (!entries_.empty()) set_current(current_ - 1); }

void BackgroundLibrary::load_current() {
    dirty_ = false;
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
    cur_w_ = cur_h_ = 0;
    if (entries_.empty()) return;

    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(entries_[current_].path.c_str(), &w, &h, &comp, 4);
    if (!data) {
        std::fprintf(stderr, "[bg] failed to load '%s'\n", entries_[current_].path.c_str());
        return;
    }
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    cur_w_ = w; cur_h_ = h;
    stbi_image_free(data);
}

GLuint BackgroundLibrary::texture() {
    if (dirty_) load_current();
    return tex_;
}
