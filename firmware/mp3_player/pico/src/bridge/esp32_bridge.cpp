#include "esp32_bridge.h"
#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <pico/mutex.h>
#include <cstring>

static constexpr uint32_t BRIDGE_BAUD = 2000000;

Esp32Bridge::Esp32Bridge(uart_inst_t* uart) : uart_(uart) {
    mutex_init(&tx_mtx_);
}

void Esp32Bridge::begin() {
    uart_init(uart_, BRIDGE_BAUD);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_UART_RX, GPIO_FUNC_UART);
    uart_set_format(uart_, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart_, true);
}

void Esp32Bridge::send_frame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
    uint8_t header[FRAME_HEADER_LEN];
    header[0] = FRAME_MAGIC0;
    header[1] = FRAME_MAGIC1;
    header[2] = cmd;
    header[3] = static_cast<uint8_t>(len & 0xFF);
    header[4] = static_cast<uint8_t>((len >> 8) & 0xFF);

    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, cmd);
    crc = crc16_update(crc, header[3]);
    crc = crc16_update(crc, header[4]);
    for (uint16_t i = 0; i < len; ++i)
        crc = crc16_update(crc, payload[i]);

    uint8_t crc_bytes[2] = { static_cast<uint8_t>(crc & 0xFF),
                              static_cast<uint8_t>((crc >> 8) & 0xFF) };

    mutex_enter_blocking(&tx_mtx_);
    uart_write_blocking(uart_, header, FRAME_HEADER_LEN);
    if (len > 0) uart_write_blocking(uart_, payload, len);
    uart_write_blocking(uart_, crc_bytes, 2);
    mutex_exit(&tx_mtx_);
}

void Esp32Bridge::play()  { send_frame(CMD_PLAY,  nullptr, 0); }
void Esp32Bridge::pause() { send_frame(CMD_PAUSE, nullptr, 0); }

void Esp32Bridge::set_volume(uint8_t vol) {
    send_frame(CMD_SET_VOLUME, &vol, 1);
}

void Esp32Bridge::mute(bool on) {
    uint8_t v = on ? 1 : 0;
    send_frame(CMD_MUTE, &v, 1);
}

void Esp32Bridge::bt_source_start(const char* name) {
    send_frame(CMD_BT_SOURCE_START,
               reinterpret_cast<const uint8_t*>(name),
               static_cast<uint16_t>(strlen(name)));
}

void Esp32Bridge::bt_sink_start(const char* name) {
    send_frame(CMD_BT_SINK_START,
               reinterpret_cast<const uint8_t*>(name),
               static_cast<uint16_t>(strlen(name)));
}

void Esp32Bridge::bt_stop() {
    send_frame(CMD_BT_STOP, nullptr, 0);
}

void Esp32Bridge::bt_connect(const uint8_t addr[6]) {
    send_frame(CMD_BT_CONNECT, addr, 6);
}

void Esp32Bridge::send_audio_frame(const int16_t* pcm, size_t sample_pairs) {
    // Payload: uint16_t sample count + raw PCM bytes.
    const uint16_t samples    = static_cast<uint16_t>(sample_pairs);
    const uint16_t pcm_bytes  = samples * 4;
    const uint16_t total_len  = 2 + pcm_bytes;

    // Stack-allocate for small frames; heap for large ones is avoidable because
    // AUDIO_BYTES_PER_FRAME is always exactly 2048 bytes.
    uint8_t payload[2 + AUDIO_BYTES_PER_FRAME];
    payload[0] = static_cast<uint8_t>(samples & 0xFF);
    payload[1] = static_cast<uint8_t>((samples >> 8) & 0xFF);
    memcpy(payload + 2, pcm, pcm_bytes);

    send_frame(CMD_AUDIO_FRAME, payload, total_len);
}

void Esp32Bridge::poll() {
    while (uart_is_readable(uart_)) {
        const uint8_t b = uart_getc(uart_);

        if (rx_pos_ == 0 && b != FRAME_MAGIC0) continue;
        if (rx_pos_ == 1 && b != FRAME_MAGIC1) { rx_pos_ = 0; continue; }
        if (rx_pos_ < RX_BUF) rx_buf_[rx_pos_++] = b;

        if (rx_pos_ < FRAME_HEADER_LEN) continue;

        const uint16_t payload_len = rx_buf_[3] | (static_cast<uint16_t>(rx_buf_[4]) << 8);
        const uint16_t total       = FRAME_HEADER_LEN + payload_len + FRAME_CRC_LEN;

        if (payload_len >= RX_BUF || rx_pos_ < total) continue;

        // Verify CRC.
        const uint8_t* payload  = rx_buf_ + FRAME_HEADER_LEN;
        const uint16_t rx_crc   = rx_buf_[total - 2] |
                                   (static_cast<uint16_t>(rx_buf_[total - 1]) << 8);
        uint16_t calc = 0xFFFF;
        calc = crc16_update(calc, rx_buf_[2]);
        calc = crc16_update(calc, rx_buf_[3]);
        calc = crc16_update(calc, rx_buf_[4]);
        for (uint16_t i = 0; i < payload_len; ++i)
            calc = crc16_update(calc, payload[i]);

        if (calc == rx_crc)
            process_frame(rx_buf_[2], payload, payload_len);

        rx_pos_ = 0;
    }
}

void Esp32Bridge::process_frame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
    switch (cmd) {
    case RSP_STATUS:
        if (len >= sizeof(BtStatusPayload) && on_status) {
            const auto* s = reinterpret_cast<const BtStatusPayload*>(payload);
            on_status(s->bt_mode, s->connected != 0, s->rssi);
        }
        break;

    case RSP_BT_CONNECTED:
        if (on_bt_connected) {
            char name[33] = {};
            if (len >= sizeof(BtEventPayload)) {
                const auto* e = reinterpret_cast<const BtEventPayload*>(payload);
                memcpy(name, e->name, 32);
            }
            on_bt_connected(true, name);
        }
        break;

    case RSP_BT_DISCONNECTED:
        if (on_bt_disconnected) on_bt_disconnected();
        break;

    default:
        break;
    }
}
