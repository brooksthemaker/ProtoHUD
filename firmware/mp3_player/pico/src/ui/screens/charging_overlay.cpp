#include "charging_overlay.h"
#include "../theme.h"

lv_obj_t* ChargingOverlay::s_overlay_ = nullptr;
bool      ChargingOverlay::s_visible_  = false;

void ChargingOverlay::init() {
    lv_obj_t* layer = lv_layer_top();

    s_overlay_ = lv_obj_create(layer);
    lv_obj_set_size(s_overlay_, 320, 240);
    lv_obj_set_pos(s_overlay_, 0, 0);
    lv_obj_set_style_bg_color(s_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay_, 0, 0);
    lv_obj_clear_flag(s_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay_, LV_OBJ_FLAG_HIDDEN);

    // Animated spinner arc.
    lv_obj_t* spinner = lv_spinner_create(s_overlay_, 1200, 60);
    lv_obj_set_size(spinner, 80, 80);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -28);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, Theme::ACCENT(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, Theme::SURFACE(), LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(s_overlay_);
    lv_label_set_text(lbl, LV_SYMBOL_USB "  USB Connected");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 44);

    lv_obj_t* hint = lv_label_create(s_overlay_);
    lv_label_set_text(hint, "Eject SD card to resume");
    lv_obj_set_style_text_color(hint, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 76);
}

void ChargingOverlay::set_visible(bool visible) {
    if (!s_overlay_) return;
    s_visible_ = visible;
    if (visible)
        lv_obj_clear_flag(s_overlay_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_overlay_, LV_OBJ_FLAG_HIDDEN);
}
