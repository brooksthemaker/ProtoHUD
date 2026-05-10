#include "now_playing.h"
#include "../theme.h"
#include "../album_art.h"
#include <cstdio>
#include <cstring>

static AlbumArt s_art;

static void fmt_time(char* buf, uint32_t secs) {
    snprintf(buf, 8, "%u:%02u", secs / 60, secs % 60);
}

void NowPlayingScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, Theme::BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // ── Status bar ───────────────────────────────────────────────────
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 28);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, Theme::SURFACE(), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_title_bar = lv_label_create(bar);
    lv_label_set_text(lbl_title_bar, LV_SYMBOL_AUDIO " MP3Player");
    lv_obj_set_style_text_color(lbl_title_bar, Theme::ACCENT(), 0);
    lv_obj_align(lbl_title_bar, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_ble_ = lv_label_create(bar);
    lv_label_set_text(lbl_ble_, "BLE");
    lv_obj_set_style_text_color(lbl_ble_, Theme::TEXT_DIM(), 0);
    lv_obj_align(lbl_ble_, LV_ALIGN_RIGHT_MID, -60, 0);

    lbl_bt_ = lv_label_create(bar);
    lv_label_set_text(lbl_bt_, "BT");
    lv_obj_set_style_text_color(lbl_bt_, Theme::TEXT_DIM(), 0);
    lv_obj_align(lbl_bt_, LV_ALIGN_RIGHT_MID, -30, 0);

    lbl_vol_ = lv_label_create(bar);
    lv_label_set_text(lbl_vol_, "80%");
    lv_obj_set_style_text_color(lbl_vol_, Theme::TEXT(), 0);
    lv_obj_align(lbl_vol_, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Album art (80×80, x=8, y=32) ───────────────────────────────────
    s_art.create(parent, 8, 32);

    // ── Track info (right of art, x=96) ───────────────────────────────
    lbl_artist_ = lv_label_create(parent);
    lv_label_set_text(lbl_artist_, "Artist");
    lv_label_set_long_mode(lbl_artist_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_artist_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_artist_, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_artist_, 218);
    lv_obj_set_pos(lbl_artist_, 96, 34);

    lbl_title_ = lv_label_create(parent);
    lv_label_set_text(lbl_title_, "Track Title");
    lv_label_set_long_mode(lbl_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(lbl_title_, Theme::TEXT(), 0);
    lv_obj_set_style_text_font(lbl_title_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl_title_, 218);
    lv_obj_set_pos(lbl_title_, 96, 52);

    lbl_album_ = lv_label_create(parent);
    lv_label_set_text(lbl_album_, "Album");
    lv_label_set_long_mode(lbl_album_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl_album_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_album_, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_album_, 218);
    lv_obj_set_pos(lbl_album_, 96, 74);

    // ── Progress bar ──────────────────────────────────────────────
    bar_progress_ = lv_bar_create(parent);
    lv_obj_set_size(bar_progress_, 280, 6);
    lv_obj_align(bar_progress_, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(bar_progress_, Theme::BAR_BG(), 0);
    lv_obj_set_style_bg_color(bar_progress_, Theme::ACCENT(), LV_PART_INDICATOR);
    lv_bar_set_range(bar_progress_, 0, 1000);
    lv_bar_set_value(bar_progress_, 0, LV_ANIM_OFF);

    lbl_pos_ = lv_label_create(parent);
    lv_label_set_text(lbl_pos_, "0:00");
    lv_obj_set_style_text_color(lbl_pos_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_pos_, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_pos_, LV_ALIGN_TOP_LEFT, 20, 128);

    lbl_dur_ = lv_label_create(parent);
    lv_label_set_text(lbl_dur_, "0:00");
    lv_obj_set_style_text_color(lbl_dur_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_dur_, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_dur_, LV_ALIGN_TOP_RIGHT, -20, 128);

    // ── Prev / Play-Pause / Next buttons ───────────────────────────────
    btn_prev_ = lv_btn_create(parent);
    lv_obj_set_size(btn_prev_, 60, 40);
    lv_obj_align(btn_prev_, LV_ALIGN_TOP_LEFT, 30, 150);
    lv_obj_t* lbl_p = lv_label_create(btn_prev_);
    lv_label_set_text(lbl_p, LV_SYMBOL_PREV);
    lv_obj_center(lbl_p);

    btn_play_ = lv_btn_create(parent);
    lv_obj_set_size(btn_play_, 70, 44);
    lv_obj_align(btn_play_, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_bg_color(btn_play_, Theme::ACCENT(), 0);
    lv_obj_t* lbl_pl = lv_label_create(btn_play_);
    lv_label_set_text(lbl_pl, LV_SYMBOL_PLAY);
    lv_obj_center(lbl_pl);

    btn_next_ = lv_btn_create(parent);
    lv_obj_set_size(btn_next_, 60, 40);
    lv_obj_align(btn_next_, LV_ALIGN_TOP_RIGHT, -30, 150);
    lv_obj_t* lbl_n = lv_label_create(btn_next_);
    lv_label_set_text(lbl_n, LV_SYMBOL_NEXT);
    lv_obj_center(lbl_n);

    // ── Queue / shuffle info ──────────────────────────────────────────
    lbl_queue_ = lv_label_create(parent);
    lv_label_set_text(lbl_queue_, "Shuffle: OFF   Repeat: OFF   1 / 1");
    lv_obj_set_style_text_color(lbl_queue_, Theme::TEXT_DIM(), 0);
    lv_obj_set_style_text_font(lbl_queue_, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_queue_, 300);
    lv_obj_set_style_text_align(lbl_queue_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_queue_, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void NowPlayingScreen::update(const AppState& state) {
    // Refresh album art when Core 1 signals a new track (generation change).
    const uint32_t gen = state.cover_art.generation.load(std::memory_order_acquire);
    if (gen != last_art_gen_) {
        last_art_gen_ = gen;
        const int32_t len = state.cover_art.len.load(std::memory_order_relaxed);
        if (len > 0) {
            s_art.load_jpeg(state.cover_art.data, static_cast<size_t>(len));
        } else {
            s_art.show_placeholder();
        }
        // Toast the track title whenever the art generation changes (i.e. track changed).
        const char* title = state.playback.current.title;
        Toast_show_from_update(title[0] ? title : state.playback.current.path);
    }

    char buf[64];

    lv_label_set_text(lbl_artist_, state.playback.current.artist[0]
                                   ? state.playback.current.artist : "Unknown Artist");
    lv_label_set_text(lbl_title_,  state.playback.current.title[0]
                                   ? state.playback.current.title  : "Unknown Track");
    lv_label_set_text(lbl_album_,  state.playback.current.album[0]
                                   ? state.playback.current.album  : "Unknown Album");

    const uint32_t dur = state.playback.current.duration_s;
    const uint32_t pos = state.playback.current.position_s;
    if (dur > 0)
        lv_bar_set_value(bar_progress_, static_cast<int32_t>(pos * 1000 / dur), LV_ANIM_OFF);

    char pos_buf[8], dur_buf[8];
    fmt_time(pos_buf, pos);
    fmt_time(dur_buf, dur);
    lv_label_set_text(lbl_pos_, pos_buf);
    lv_label_set_text(lbl_dur_, dur_buf);

    // Play/pause icon.
    lv_obj_t* lbl_icon = lv_obj_get_child(btn_play_, 0);
    lv_label_set_text(lbl_icon, state.playback.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    // Volume.
    snprintf(buf, sizeof(buf), "%u%%", state.playback.volume);
    lv_label_set_text(lbl_vol_, buf);

    // BLE / BT indicators.
    lv_obj_set_style_text_color(lbl_ble_, Theme::ACCENT(), 0);
    lv_obj_set_style_text_color(lbl_bt_,
        state.bt.source_connected || state.bt.sink_connected
        ? Theme::SUCCESS() : Theme::TEXT_DIM(), 0);

    // Queue / shuffle / repeat.
    const char* repeat_str =
        state.playback.repeat == RepeatMode::ONE ? "ONE" :
        state.playback.repeat == RepeatMode::ALL ? "ALL" : "OFF";
    snprintf(buf, sizeof(buf), "Shuf: %s  Rep: %s  %u/%u",
             state.playback.shuffled ? "ON" : "OFF",
             repeat_str,
             state.playback.queue_index + 1,
             state.playback.queue_total);
    lv_label_set_text(lbl_queue_, buf);
}
