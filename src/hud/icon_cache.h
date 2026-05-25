#pragma once
#include <nanovg.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

// ── IconCache ─────────────────────────────────────────────────────────────────
// Loads PNG icons once into NanoVG image handles, keyed by name, and draws them.
// A name maps to "<dir>/<name>.png". Missing/unloadable icons are cached as a
// no-op so callers can ship the pipeline before the art exists. Image handles are
// owned by the NanoVG context and freed when it is destroyed.
//
// Note: NanoVG image paint can only scale alpha, not recolor, so icons render in
// their own colors — author them already-colored (or white if you only need a
// glyph). All calls happen on the render thread inside an NVG frame.
class IconCache {
public:
    void set_dir(std::string dir) { dir_ = std::move(dir); }

    // Returns the NanoVG image handle (>0) or 0 if missing/unloadable. Cached.
    int handle(NVGcontext* vg, const std::string& name) {
        auto it = cache_.find(name);
        if (it != cache_.end()) return it->second;
        int img = 0;
        if (vg && !dir_.empty() && !name.empty())
            img = nvgCreateImage(vg, (dir_ + "/" + name + ".png").c_str(), 0);
        cache_[name] = img;   // cache misses (0) too, so we don't retry every frame
        return img;
    }

    // Draw `name` centered at (cx, cy), fit within size×size (aspect preserved).
    // Returns true if an icon was actually drawn (false → caller can lay out as
    // if no icon is present).
    bool draw(NVGcontext* vg, const std::string& name,
              float cx, float cy, float size, float alpha = 1.f) {
        const int img = handle(vg, name);
        if (img <= 0) return false;
        int iw = 0, ih = 0;
        nvgImageSize(vg, img, &iw, &ih);
        if (iw <= 0 || ih <= 0) return false;
        const float s = size / static_cast<float>(std::max(iw, ih));
        const float w = iw * s, h = ih * s;
        const float x = cx - w * 0.5f, y = cy - h * 0.5f;
        NVGpaint p = nvgImagePattern(vg, x, y, w, h, 0.f, img, alpha);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, p);
        nvgFill(vg);
        return true;
    }

private:
    std::string dir_;
    std::unordered_map<std::string, int> cache_;
};
