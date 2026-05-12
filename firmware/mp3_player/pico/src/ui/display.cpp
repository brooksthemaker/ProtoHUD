#include "display.h"
#include "theme.h"
#include "../../../include/pins.h"
#include <Arduino.h>

lv_color_t Display::buf1_[320 * Display::BUF_LINES];
lv_color_t Display::buf2_[320 * Display::BUF_LINES];
int        Display::enc_delta_           = 0;
bool       Display::enc_select_          = false;
uint32_t   Display::s_last_activity_ms_ = 0;
uint8_t    Display::s_brightness_pct_   = 100;

Display::Display(AppState& state, AnoEncoder& enc)
    : state_(state), enc_(enc) {}

bool Display::begin() {
    tft_.begin();
    tft_.setRotation(1);  // landscape: 320×240
    tft_.fillScreen(TFT_BLACK);
    tft_.initDMA();

    // Backlight: full brightness at startup.
    pinMode(PIN_TFT_BL, OUTPUT);
    set_brightness(100);
    s_last_activity_ms_ = millis();

    lv_init();

    lv_disp_draw_buf_init(&draw_buf_, buf1_, buf2_, 320 * BUF_LINES);

    lv_disp_drv_init(&disp_drv_);
    disp_drv_.hor_res   = 320;
    disp_drv_.ver_res   = 240;
    disp_drv_.draw_buf  = &draw_buf_;
    disp_drv_.flush_cb  = flush_cb;
    disp_drv_.user_data = this;
    lv_disp_t* disp = lv_disp_drv_register(&disp_drv_);

    Theme::apply(disp);

    // Register rotary encoder as LVGL input device.
    lv_indev_drv_init(&enc_drv_);
    enc_drv_.type    = LV_INDEV_TYPE_ENCODER;
    enc_drv_.read_cb = encoder_read_cb;
    enc_indev_ = lv_indev_drv_register(&enc_drv_);

    // Wire encoder events to LVGL delta accumulation.
    enc_.on_rotate = [](int delta) { enc_delta_ += delta; };
    enc_.on_select = []() { enc_select_ = true; };

    return true;
}

void Display::set_brightness(uint8_t pct) {
    s_brightness_pct_ = pct > 100 ? 100 : pct;
    // Map 0-100% to 0-255 PWM duty cycle (0% stays 0, 100% is full 255).
    const uint8_t duty = (s_brightness_pct_ == 0) ? 0
                       : (s_brightness_pct_ >= 100) ? 255
                       : static_cast<uint8_t>(s_brightness_pct_ * 255u / 100u);
    analogWrite(PIN_TFT_BL, duty);
}

void Display::on_activity() {
    s_last_activity_ms_ = millis();
    if (s_brightness_pct_ < 100) {
        set_brightness(100);
    }
}

void Display::task() {
    lv_tick_inc(33);   // ~30 fps nominal tick
    enc_.poll();       // read seesaw; fires enc_ callbacks above

    // Any encoder activity resets the inactivity timer and wakes the screen.
    if (enc_delta_ != 0 || enc_select_) {
        on_activity();
    }

    // Backlight power management: dim → off based on inactivity.
    const uint32_t idle_ms = millis() - s_last_activity_ms_;
    if (idle_ms >= OFF_TIMEOUT_MS) {
        if (s_brightness_pct_ != 0) set_brightness(0);
    } else if (idle_ms >= DIM_TIMEOUT_MS) {
        if (s_brightness_pct_ > 30) set_brightness(30);
    }

    lv_task_handler();
}

void Display::flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    auto* self = static_cast<Display*>(drv->user_data);
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    self->tft_.startWrite();
    self->tft_.setAddrWindow(area->x1, area->y1, w, h);
    self->tft_.pushPixelsDMA(reinterpret_cast<uint16_t*>(color_p), w * h);
    self->tft_.endWrite();
    lv_disp_flush_ready(drv);
}

void Display::encoder_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    data->enc_diff = static_cast<int16_t>(enc_delta_);
    enc_delta_     = 0;
    data->state    = enc_select_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    enc_select_    = false;
}
