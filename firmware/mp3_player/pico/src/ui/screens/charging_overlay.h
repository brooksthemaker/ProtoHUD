#pragma once
#include <lvgl.h>

// Full-screen overlay shown when USB MSC mode is active.
// Drawn on lv_layer_top() so it appears above all screens.
// Call init() once after lv_init(), set_visible() on USB state changes.
class ChargingOverlay {
public:
    static void init();
    static void set_visible(bool visible);
    static bool is_visible() { return s_visible_; }

private:
    static lv_obj_t* s_overlay_;
    static bool      s_visible_;
};
