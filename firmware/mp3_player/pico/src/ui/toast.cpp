#include "toast.h"

lv_obj_t* Toast::s_cont_      = nullptr;
lv_obj_t* Toast::s_label_     = nullptr;
uint32_t  Toast::s_hide_tick_ = 0;
bool      Toast::s_visible_   = false;

static constexpr lv_coord_t TOAST_Y_HIDDEN = 240;  // off-screen below display
static constexpr lv_coord_t TOAST_Y_SHOWN  = 198;  // 6 px above bottom edge

void Toast::init() {
    lv_obj_t* layer = lv_layer_top();

    s_cont_ = lv_obj_create(layer);
    lv_obj_set_size(s_cont_, 300, 36);
    lv_obj_set_pos(s_cont_, 10, TOAST_Y_HIDDEN);
    lv_obj_set_style_bg_color(s_cont_, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(s_cont_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_cont_, 0, 0);
    lv_obj_set_style_radius(s_cont_, 8, 0);
    lv_obj_set_style_pad_all(s_cont_, 4, 0);
    lv_obj_clear_flag(s_cont_, LV_OBJ_FLAG_SCROLLABLE);

    s_label_ = lv_label_create(s_cont_);
    lv_label_set_long_mode(s_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_label_, 290);
    lv_obj_set_style_text_color(s_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label_, LV_ALIGN_CENTER, 0, 0);
}

void Toast::anim_y_cb(void* obj, int32_t v) {
    lv_obj_set_y(static_cast<lv_obj_t*>(obj), v);
}

void Toast::slide_out_ready_cb(lv_anim_t* a) {
    // Snap to hidden position after animation completes.
    lv_obj_set_y(static_cast<lv_obj_t*>(a->var), TOAST_Y_HIDDEN);
}

void Toast::show(const char* msg, uint32_t duration_ms) {
    if (!s_cont_ || !msg) return;
    lv_label_set_text(s_label_, msg);
    s_hide_tick_ = lv_tick_get() + duration_ms;
    s_visible_   = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cont_);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_cont_), TOAST_Y_SHOWN);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void Toast::task() {
    if (!s_visible_ || !s_cont_) return;
    if (lv_tick_get() < s_hide_tick_) return;

    s_visible_ = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cont_);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, TOAST_Y_SHOWN, TOAST_Y_HIDDEN);
    lv_anim_set_time(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, slide_out_ready_cb);
    lv_anim_start(&a);
}
