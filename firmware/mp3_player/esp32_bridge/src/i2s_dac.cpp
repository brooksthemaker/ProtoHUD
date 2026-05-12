#include "i2s_dac.h"
#include <Arduino.h>
#include <driver/gpio.h>

bool I2sDac::begin() {
    // Configure mute pin — start muted to prevent power-on pop.
    pinMode(PIN_PCM_XSMT, OUTPUT);
    digitalWrite(PIN_PCM_XSMT, LOW);
    muted_ = true;

    const i2s_config_t cfg = {
        .mode                 = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = 44100,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = true,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0,
    };
    const i2s_pin_config_t pins = {
        .bck_io_num   = PIN_I2S_BCLK,
        .ws_io_num    = PIN_I2S_LRC,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    if (i2s_driver_install(DAC_I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(DAC_I2S_PORT, &pins) != ESP_OK) return false;
    i2s_zero_dma_buffer(DAC_I2S_PORT);

    ring_ = xRingbufferCreate(DAC_RING_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!ring_) return false;

    xTaskCreatePinnedToCore(drain_task_fn, "i2s_drain", 4096, this, 22, &drain_task_, 1);
    return true;
}

size_t I2sDac::write(const uint8_t* pcm, size_t len) {
    if (!ring_) return 0;
    if (xRingbufferSend(ring_, pcm, len, 0) == pdTRUE) return len;
    return 0;
}

void I2sDac::mute() {
    muted_ = true;
    digitalWrite(PIN_PCM_XSMT, LOW);
}

void I2sDac::unmute() {
    muted_ = false;
    digitalWrite(PIN_PCM_XSMT, HIGH);
}

void I2sDac::flush_and_mute() {
    vTaskDelay(pdMS_TO_TICKS(100));
    mute();
}

void I2sDac::drain_task_fn(void* param) {
    auto* self = static_cast<I2sDac*>(param);
    size_t bytes_written;
    for (;;) {
        size_t item_size;
        void* item = xRingbufferReceiveUpTo(self->ring_, &item_size,
                                            pdMS_TO_TICKS(20), 2048);
        if (item) {
            i2s_write(DAC_I2S_PORT, item, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(self->ring_, item);
        } else {
            // No data: write silence to keep I2S DMA fed and prevent underrun clicks.
            static const uint8_t silence[256] = {};
            i2s_write(DAC_I2S_PORT, silence, sizeof(silence), &bytes_written, 0);
        }
    }
}
