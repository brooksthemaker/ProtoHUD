#pragma once
#include <lvgl.h>

// Colour palette (RGB565 via lv_color_make).
namespace Theme {
    inline lv_color_t BG        () { return lv_color_make(0x10, 0x10, 0x18); }
    inline lv_color_t SURFACE   () { return lv_color_make(0x1E, 0x1E, 0x2E); }
    inline lv_color_t ACCENT    () { return lv_color_make(0x5E, 0x81, 0xF4); }
    inline lv_color_t ACCENT2   () { return lv_color_make(0xA0, 0xC4, 0xFF); }
    inline lv_color_t TEXT      () { return lv_color_make(0xE0, 0xE0, 0xF0); }
    inline lv_color_t TEXT_DIM  () { return lv_color_make(0x70, 0x70, 0x90); }
    inline lv_color_t SUCCESS   () { return lv_color_make(0x50, 0xFA, 0x7B); }
    inline lv_color_t WARNING   () { return lv_color_make(0xFF, 0xB8, 0x6C); }
    inline lv_color_t DANGER    () { return lv_color_make(0xFF, 0x55, 0x55); }
    inline lv_color_t BAR_BG    () { return lv_color_make(0x30, 0x30, 0x50); }

    // Apply base theme to LVGL display.
    void apply(lv_disp_t* disp);
}
