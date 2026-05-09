#include <Arduino.h>
#include "i2s_dac.h"
#include "bt_audio.h"
#include "uart_handler.h"
#include "protocol.h"

static I2sDac       dac;
static BtAudio      bt(dac);
static UartHandler  uart(bt, dac);

void setup() {
    if (!dac.begin()) {
        // Fatal: I2S init failed — blink LED and halt.
        pinMode(LED_BUILTIN, OUTPUT);
        for (;;) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }

    bt.on_connection_changed = [](bool connected, const uint8_t* /*addr*/, const char* /*name*/) {
        uart.send_status();
        if (connected) {
            uint8_t evt[sizeof(BtEventPayload)] = {};
            uart.send_frame(RSP_BT_CONNECTED, evt, sizeof(evt));
        } else {
            uart.send_frame(RSP_BT_DISCONNECTED, nullptr, 0);
        }
    };

    uart.begin();

    // Signal Pico that the bridge is ready with an initial status frame.
    vTaskDelay(pdMS_TO_TICKS(100));
    uart.send_status();
}

void loop() {
    // All work is done in FreeRTOS tasks; send periodic keepalive status.
    vTaskDelay(pdMS_TO_TICKS(5000));
    uart.send_status();
}
