#include "uart_handler.h"
#include <cstring>

UartHandler::UartHandler(BtAudio& bt, I2sDac& dac) : bt_(bt), dac_(dac) {
    tx_mtx_ = xSemaphoreCreateMutex();
}

void UartHandler::begin() {
    Serial.begin(BRIDGE_BAUD);
    xTaskCreatePinnedToCore(uart_task, "uart_rx", 8192, this, 10, &task_, 0);
}

void UartHandler::send_frame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
    uint8_t header[FRAME_HEADER_LEN];
    header[0] = FRAME_MAGIC0;
    header[1] = FRAME_MAGIC1;
    header[2] = cmd;
    header[3] = len & 0xFF;
    header[4] = (len >> 8) & 0xFF;

    // CRC over cmd, len bytes, and payload.
    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, cmd);
    crc = crc16_update(crc, header[3]);
    crc = crc16_update(crc, header[4]);
    for (uint16_t i = 0; i < len; ++i)
        crc = crc16_update(crc, payload[i]);

    uint8_t crc_bytes[2] = { static_cast<uint8_t>(crc & 0xFF),
                              static_cast<uint8_t>((crc >> 8) & 0xFF) };

    xSemaphoreTake(tx_mtx_, portMAX_DELAY);
    Serial.write(header, FRAME_HEADER_LEN);
    if (len > 0) Serial.write(payload, len);
    Serial.write(crc_bytes, 2);
    xSemaphoreGive(tx_mtx_);
}

void UartHandler::send_status() {
    BtStatusPayload p;
    p.bt_mode  = static_cast<uint8_t>(bt_.mode());
    p.connected = bt_.connected() ? 1 : 0;
    p.rssi      = bt_.rssi();
    send_frame(RSP_STATUS, reinterpret_cast<uint8_t*>(&p), sizeof(p));
}

void UartHandler::uart_task(void* param) {
    auto* self = static_cast<UartHandler*>(param);
    for (;;) {
        self->read_frame();
        taskYIELD();
    }
}

bool UartHandler::read_frame() {
    // Fill buffer byte by byte until we have a complete frame.
    while (Serial.available()) {
        uint8_t b = Serial.read();

        // Scan for magic header.
        if (rx_pos_ == 0 && b != FRAME_MAGIC0) continue;
        if (rx_pos_ == 1 && b != FRAME_MAGIC1) { rx_pos_ = 0; continue; }

        if (rx_pos_ < RX_BUF)
            rx_buf_[rx_pos_++] = b;

        // Need at least the full header (5 bytes) to know payload length.
        if (rx_pos_ < FRAME_HEADER_LEN) continue;

        const uint16_t payload_len = rx_buf_[3] | (static_cast<uint16_t>(rx_buf_[4]) << 8);
        const uint16_t total       = FRAME_HEADER_LEN + payload_len + FRAME_CRC_LEN;

        if (payload_len > AUDIO_BYTES_PER_FRAME + 2) {
            // Implausible length — reset.
            rx_pos_ = 0;
            continue;
        }

        if (rx_pos_ < total) continue;

        // Verify CRC.
        const uint8_t* payload = rx_buf_ + FRAME_HEADER_LEN;
        const uint16_t rx_crc  = rx_buf_[total - 2] |
                                  (static_cast<uint16_t>(rx_buf_[total - 1]) << 8);
        uint16_t calc_crc = 0xFFFF;
        calc_crc = crc16_update(calc_crc, rx_buf_[2]);  // cmd
        calc_crc = crc16_update(calc_crc, rx_buf_[3]);  // len lo
        calc_crc = crc16_update(calc_crc, rx_buf_[4]);  // len hi
        for (uint16_t i = 0; i < payload_len; ++i)
            calc_crc = crc16_update(calc_crc, payload[i]);

        if (calc_crc == rx_crc)
            process_frame(rx_buf_[2], payload, payload_len);

        rx_pos_ = 0;
        return true;
    }
    return false;
}

void UartHandler::process_frame(uint8_t cmd, const uint8_t* payload, uint16_t len) {
    switch (cmd) {
    case CMD_PLAY:
        dac_.unmute();
        break;

    case CMD_PAUSE:
        dac_.flush_and_mute();
        break;

    case CMD_SET_VOLUME:
        if (len >= 1) {
            // Volume 0–100 mapped to AVRC absolute volume 0–127.
            uint8_t vol = payload[0];
            if (bt_.mode() == BtMode::SOURCE)
                bt_.a2dp_src_.set_volume(vol * 127 / 100);
        }
        break;

    case CMD_BT_SOURCE_START: {
        char name[33] = "MP3Player";
        if (len > 0) {
            memcpy(name, payload, len < 32 ? len : 32);
            name[len < 32 ? len : 32] = '\0';
        }
        bt_.start_source(name);
        send_status();
        break;
    }

    case CMD_BT_SINK_START: {
        char name[33] = "MP3Player-In";
        if (len > 0) {
            memcpy(name, payload, len < 32 ? len : 32);
            name[len < 32 ? len : 32] = '\0';
        }
        bt_.start_sink(name);
        send_status();
        break;
    }

    case CMD_BT_STOP:
        bt_.stop();
        send_status();
        break;

    case CMD_AUDIO_FRAME:
        if (len >= 2) {
            // First 2 bytes: sample count (little-endian); remainder: PCM data.
            const uint16_t samples = payload[0] | (static_cast<uint16_t>(payload[1]) << 8);
            const size_t   pcm_len = static_cast<size_t>(samples) * 4;
            if (len >= 2 + pcm_len) {
                // Write to DAC ring buffer (wired) and BT source ring buffer.
                dac_.write(payload + 2, pcm_len);
                if (bt_.mode() == BtMode::SOURCE)
                    bt_.push_source_audio(payload + 2, pcm_len);
            }
        }
        break;

    case CMD_MUTE:
        if (len >= 1 && payload[0])
            dac_.mute();
        else
            dac_.unmute();
        break;

    default:
        break;
    }
}
