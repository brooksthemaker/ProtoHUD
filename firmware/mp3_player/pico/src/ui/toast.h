#pragma once
#include <lvgl.h>

// Slide-up toast notification rendered on lv_layer_top() (above all screens).
//
// Usage:
//   Toast::init();          // once, after lv_init()
//   Toast::show("text");    // from Core 0 / LVGL context only
//   Toast::task();          // every display loop iteration to drive slide-out
class Toast {
public:
    static void init();
    static void show(const char* msg, uint32_t duration_ms = 2000);
    static void task();   // drives the auto-hide timer and slide-out animation

private:
    static void anim_y_cb(void* obj, int32_t v);
    static void slide_out_ready_cb(lv_anim_t* a);

    static lv_obj_t* s_cont_;
    static lv_obj_t* s_label_;
    static uint32_t  s_hide_tick_;
    static bool      s_visible_;
};
