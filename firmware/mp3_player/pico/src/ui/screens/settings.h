#pragma once
#include <lvgl.h>
#include <functional>
#include "../../app_state.h"

// Settings screen — scrollable list of controls.
class SettingsScreen {
public:
    void create(lv_obj_t* parent);
    void update(const AppState& state);

    std::function<void(AppMode)>   on_mode_change;
    std::function<void(uint8_t)>   on_volume_change;
    std::function<void(bool)>      on_shuffle_change;
    std::function<void(RepeatMode)>on_repeat_change;

private:
    static void event_cb(lv_event_t* e);

    lv_obj_t* sw_shuffle_  = nullptr;
    lv_obj_t* roller_repeat_= nullptr;
    lv_obj_t* slider_vol_  = nullptr;
    lv_obj_t* lbl_sd_info_ = nullptr;
    lv_obj_t* btn_mode_wired_   = nullptr;
    lv_obj_t* btn_mode_bt_src_  = nullptr;
    lv_obj_t* btn_mode_bt_sink_ = nullptr;
};
