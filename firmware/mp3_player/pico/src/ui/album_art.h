#pragma once
#include <lvgl.h>
#include <cstdint>
#include <cstddef>

// Renders JPEG album art into an 80×80 LVGL canvas using JPEGDEC.
// Singleton: only one active decode at a time (static pixel buffer).
// Call create() once in NowPlayingScreen::create(), then load_jpeg() or
// show_placeholder() whenever the track changes.
class AlbumArt {
public:
    static constexpr int W = 80;
    static constexpr int H = 80;

    // Create the LVGL canvas widget at (x, y) inside parent.
    void create(lv_obj_t* parent, int x, int y);

    // Decode jpeg_data[len] and draw into canvas.
    bool load_jpeg(const uint8_t* jpeg_data, size_t len);

    // Fill canvas with a placeholder music-note graphic.
    void show_placeholder();

    // Exposed so the static JPEGDEC draw callback can write pixels.
    static lv_color_t* px_buf() { return s_px_buf_; }

private:
    lv_obj_t*         canvas_  = nullptr;
    static lv_color_t s_px_buf_[W * H];
};
