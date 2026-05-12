#pragma once
#include <lvgl.h>
#include "../../app_state.h"

// Now Playing screen layout (320×240):
//
//  ┌────────────────────────────────────────────────┐
//  │  ♪ MP3Player        [BLE] [BT]     vol 80%       │ h=28 status bar
//  ├────────────────────────────────────────────────┤
//  │ [────]  Artist Name                           │ y=32 art (80×80)
//  │ [ art]  Track Title (scrolls if long)        │ y=52 title
//  │ [────]  Album                                 │ y=74 album
//  │  ────────────────────────────────────────────    │
//  │  1:32  ████████░░░░░░░░░░░░░░░░░░░░░░  4:12     │ y=118 seek bar
//  │   [|◄◄]       [ ►► / ‖‖ ]       [►►|]         │ y=148
//  │  Shuffle: ON   Repeat: ALL   7 / 43              │ bottom
//  └────────────────────────────────────────────────┘

class NowPlayingScreen {
public:
    void create(lv_obj_t* parent);
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

    // Generation counter from CoverArtBuf; UINT32_MAX forces load on first update.
    uint32_t last_art_gen_  = 0xFFFFFFFFu;
};
