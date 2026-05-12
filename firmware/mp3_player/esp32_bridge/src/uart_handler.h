#pragma once
#include <Arduino.h>
#include "protocol.h"
#include "bt_audio.h"
#include "i2s_dac.h"

static constexpr uint32_t BRIDGE_BAUD = 2000000;

// Receives framed commands from Pico over Serial (UART0) at 2 Mbps.
// Sends status frames back to Pico.
// All parsing happens in the uart_task FreeRTOS task on Core 0.

class UartHandler {
public:
    UartHandler(BtAudio& bt, I2sDac& dac);

    void begin();

    // Send a response frame to Pico.
    void send_frame(uint8_t cmd, const uint8_t* payload, uint16_t len);
    void send_status();

    static void uart_task(void* param);

private:
    void process_frame(uint8_t cmd, const uint8_t* payload, uint16_t len);
    bool read_frame();

    BtAudio& bt_;
    I2sDac&  dac_;

    // Parse state machine buffer.
    static constexpr size_t RX_BUF = AUDIO_BYTES_PER_FRAME + FRAME_OVERHEAD + 2;
    uint8_t  rx_buf_[RX_BUF];
    size_t   rx_pos_ = 0;

    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t tx_mtx_ = nullptr;
};
