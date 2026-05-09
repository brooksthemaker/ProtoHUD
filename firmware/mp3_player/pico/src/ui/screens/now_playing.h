#pragma once
#include <lvgl.h>
#include "../../app_state.h"

// Now Playing screen layout (320×240):
//
//  ┌──────────────────────────────────────────────────┐
//  │  ♪ MP3Player        [BLE] [BT]     vol 80%       │ h=28 status bar
//  ├──────────────────────────────────────────────────┤
//  │        Artist Name                               │ y=55
//  │        Track Title (scrolls if long)             │ y=80
//  │        Album · 4:12                              │ y=110
//  │  ────────────────────────────────────────────    │
//  │  1:32  ████████░░░░░░░░░░░░░░░░░░░░░░  4:12     │ y=145 seek bar
//  │  ────────────────────────────────────────────    │
//  │   [|◄◄]        [ ►► / ‖‖ ]        [►►|]         │ y=178
//  │  Shuffle: ON   Repeat: ALL   7 / 43              │ y=218
//  └──────────────────────────────────────────────────┘

class NowPlayingScreen {
public:
    // Create LVGL objects on the given parent screen object.
    void create(lv_obj_t* parent);

    // Call ~1 Hz or whenever playback state changes.
    void update(const AppState& state);

private:
    lv_obj_t* lbl_artist_   = nullptr;
    lv_obj_t* lbl_title_    = nullptr;
    lv_obj_t* lbl_album_    = nullptr;
    lv_obj_t* bar_progress_ = nullptr;
    lv_obj_t* lbl_pos_      = nullptr;
    lv_obj_t* lbl_dur_      = nullptr;
    lv_obj_t* btn_prev_     = nullptr;
    lv_obj_t* btn_play_     = nullptr;
    lv_obj_t* btn_next_     = nullptr;
    lv_obj_t* lbl_queue_    = nullptr;
    lv_obj_t* lbl_vol_      = nullptr;
    lv_obj_t* lbl_ble_      = nullptr;
    lv_obj_t* lbl_bt_       = nullptr;

    static char fmt_time(char* buf, uint32_t secs);
};
