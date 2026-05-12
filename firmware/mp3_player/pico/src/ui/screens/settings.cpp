#include "settings.h"
#include "../theme.h"
#include <cstdio>

static const char* REPEAT_OPTIONS = "OFF\nONE\nALL";
static const char* EQ_OPTIONS     = "Flat\nBass Boost\nVocal\nTreble";

void SettingsScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, Theme::BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_size(list, 320, 240);
    lv_obj_center(list);
    lv_obj_set_style_bg_color(list, Theme::BG(), 0);
    lv_obj_set_style_border_width(list, 0, 0);

    // ── Output mode ──────────────────────────────────────────────────────────────
    lv_list_add_text(list, "Output Mode");

    btn_mode_wired_ = lv_list_add_btn(list, LV_SYMBOL_AUDIO, "Wired (PCM5102)");
    lv_obj_add_event_cb(btn_mode_wired_, event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn_mode_wired_, reinterpret_cast<void*>(0));

    btn_mode_bt_src_ = lv_list_add_btn(list, LV_SYMBOL_BLUETOOTH, "BT Headphones");
    lv_obj_add_event_cb(btn_mode_bt_src_, event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn_mode_bt_src_, reinterpret_cast<void*>(1));

    btn_mode_bt_sink_ = lv_list_add_btn(list, LV_SYMBOL_BLUETOOTH, "BT Sink (receive)");
    lv_obj_add_event_cb(btn_mode_bt_sink_, event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn_mode_bt_sink_, reinterpret_cast<void*>(2));

    // ── Volume ─────────────────────────────────────────────────────────────────
    lv_list_add_text(list, "Volume");
    lv_obj_t* vol_row = lv_obj_create(list);
    lv_obj_set_size(vol_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);

    slider_vol_ = lv_slider_create(vol_row);
    lv_slider_set_range(slider_vol_, 0, 100);
    lv_slider_set_value(slider_vol_, 80, LV_ANIM_OFF);
    lv_obj_set_width(slider_vol_, 200);
    lv_obj_center(slider_vol_);
    lv_obj_set_style_bg_color(slider_vol_, Theme::ACCENT(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider_vol_, Theme::ACCENT(), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider_vol_, event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_user_data(slider_vol_, reinterpret_cast<void*>(10));

    // ── Shuffle ───────────────────────────────────────────────────────────────
    lv_list_add_text(list, "Shuffle");
    lv_obj_t* shuf_row = lv_obj_create(list);
    lv_obj_set_size(shuf_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(shuf_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shuf_row, 0, 0);
    sw_shuffle_ = lv_switch_create(shuf_row);
    lv_obj_center(sw_shuffle_);
    lv_obj_add_event_cb(sw_shuffle_, event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_user_data(sw_shuffle_, reinterpret_cast<void*>(11));

    // ── Repeat ───────────────────────────────────────────────────────────────
    lv_list_add_text(list, "Repeat");
    roller_repeat_ = lv_roller_create(list);
    lv_roller_set_options(roller_repeat_, REPEAT_OPTIONS, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller_repeat_, 1);
    lv_obj_set_width(roller_repeat_, 120);
    lv_obj_add_event_cb(roller_repeat_, event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_user_data(roller_repeat_, reinterpret_cast<void*>(12));

    // ── EQ preset ──────────────────────────────────────────────────────────
    lv_list_add_text(list, "EQ");
    roller_eq_ = lv_roller_create(list);
    lv_roller_set_options(roller_eq_, EQ_OPTIONS, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller_eq_, 1);
    lv_obj_set_width(roller_eq_, 160);
    lv_obj_add_event_cb(roller_eq_, event_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_user_data(roller_eq_, reinterpret_cast<void*>(13));

    // ── SD info ─────────────────────────────────────────────────────────────
    lv_list_add_text(list, "SD Card");
    lbl_sd_info_ = lv_label_create(list);
    lv_label_set_text(lbl_sd_info_, "Scanning...");
    lv_obj_set_style_text_color(lbl_sd_info_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_sd_info_, &lv_font_montserrat_12, 0);
}

void SettingsScreen::update(const AppState& state) {
    if (!sw_shuffle_) return;

    if (state.playback.shuffled)
        lv_obj_add_state(sw_shuffle_, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(sw_shuffle_, LV_STATE_CHECKED);

    lv_roller_set_selected(roller_repeat_,
                           static_cast<uint16_t>(state.playback.repeat), LV_ANIM_OFF);
    lv_roller_set_selected(roller_eq_,
                           static_cast<uint16_t>(state.playback.eq_preset), LV_ANIM_OFF);
    lv_slider_set_value(slider_vol_, state.playback.volume, LV_ANIM_OFF);

    char buf[48];
    snprintf(buf, sizeof(buf), "%.1f GB free / %.1f GB",
             0.0f,
             0.0f);
    lv_label_set_text(lbl_sd_info_, buf);
}

void SettingsScreen::event_cb(lv_event_t* e) {
    auto* self = static_cast<SettingsScreen*>(lv_event_get_user_data(e));
    auto* obj  = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const intptr_t id = reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj));

    switch (id) {
    case 0: if (self->on_mode_change) self->on_mode_change(AppMode::SD_PLAYBACK); break;
    case 1: if (self->on_mode_change) self->on_mode_change(AppMode::BT_SOURCE);   break;
    case 2: if (self->on_mode_change) self->on_mode_change(AppMode::BT_SINK);     break;
    case 10:
        if (self->on_volume_change)
            self->on_volume_change(static_cast<uint8_t>(lv_slider_get_value(obj)));
        break;
    case 11:
        if (self->on_shuffle_change)
            self->on_shuffle_change(lv_obj_has_state(obj, LV_STATE_CHECKED));
        break;
    case 12:
        if (self->on_repeat_change)
            self->on_repeat_change(
                static_cast<RepeatMode>(lv_roller_get_selected(obj)));
        break;
    case 13:
        if (self->on_eq_change)
            self->on_eq_change(
                static_cast<EqPreset>(lv_roller_get_selected(obj)));
        break;
    default: break;
    }
}
