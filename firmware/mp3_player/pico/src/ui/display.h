#pragma once
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "../app_state.h"
#include "../input/encoder.h"

// LVGL 8 display driver on top of TFT_eSPI / ILI9341 (320×240).
// Two 320×20-line draw buffers in static RAM (~25 KB total).
// Flush uses TFT_eSPI::pushImageDMA for DMA-backed transfers.
//
// Backlight PWM on PIN_TFT_BL (GPIO 28):
//   - Dims to 30% after DIM_TIMEOUT_MS of no encoder input.
//   - Turns off after OFF_TIMEOUT_MS of no encoder input.
//   - Any encoder rotation or press wakes the screen immediately.

class Display {
public:
    Display(AppState& state, AnoEncoder& enc);

    bool begin();
    void task();  // drives lv_task_handler() and encoder input

    // Set backlight level 0–100 (0 = off, 100 = full).
    static void set_brightness(uint8_t pct);

    // Call on any user-visible interaction to reset the inactivity timer.
    void on_activity();

    // Inactivity thresholds.
    static constexpr uint32_t DIM_TIMEOUT_MS = 30000;  // 30 s → dim to 30%
    static constexpr uint32_t OFF_TIMEOUT_MS = 60000;  // 60 s → backlight off

private:
    static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
    static void encoder_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

    AppState&   state_;
    AnoEncoder& enc_;
    TFT_eSPI    tft_;

    // Double draw buffer: 320 × 20 × 2 bytes × 2 = 25.6 KB static.
    static constexpr int BUF_LINES = 20;
    static lv_color_t buf1_[320 * BUF_LINES];
    static lv_color_t buf2_[320 * BUF_LINES];

    lv_disp_draw_buf_t draw_buf_;
    lv_disp_drv_t      disp_drv_;
    lv_indev_drv_t     enc_drv_;
    lv_indev_t*        enc_indev_ = nullptr;

    // Encoder state passed to LVGL indev callback.
    static int    enc_delta_;
    static bool   enc_select_;

    // Backlight state (Core 0 only).
    static uint32_t s_last_activity_ms_;
    static uint8_t  s_brightness_pct_;
};
