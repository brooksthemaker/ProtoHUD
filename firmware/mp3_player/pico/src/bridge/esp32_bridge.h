#pragma once
#include <Arduino.h>
#include <cstdint>
#include <functional>
#include "../../include/pins.h"

// UART framing protocol shared with the ESP32 bridge.
// Include the same protocol.h definition (copied / symlinked from esp32_bridge).
#include "protocol_pico.h"

// Communicates with the ESP32 BT audio bridge over UART0 at 2 Mbps.
// send_*() methods may be called from Core 1 (audio task).
// receive polling must be called from Core 0 (loop/task).

class Esp32Bridge {
public:
    explicit Esp32Bridge(uart_inst_t* uart = uart0);

    void begin();

    // ── Commands to ESP32 ─────────────────────────────────────────────────────

    void play();
    void pause();
    void set_volume(uint8_t vol_0_100);
    void mute(bool on);
    void bt_source_start(const char* name = "MP3Player");
    void bt_sink_start(const char* name = "MP3Player-In");
    void bt_stop();
    void bt_connect(const uint8_t addr[6]);

    // Send a decoded PCM frame (512 stereo sample pairs = 2048 bytes).
    // Returns immediately; frame is written via DMA.
    void send_audio_frame(const int16_t* pcm, size_t sample_pairs);

    // ── Status from ESP32 (call poll() regularly from Core 0) ─────────────────
    void poll();  // parse incoming frames; fires callbacks below

    std::function<void(bool connected, const char* peer_name)> on_bt_connected;
    std::function<void()>                                       on_bt_disconnected;
    std::function<void(uint8_t mode, bool connected, int8_t rssi)> on_status;

private:
    void send_frame(uint8_t cmd, const uint8_t* payload, uint16_t len);
    void process_frame(uint8_t cmd, const uint8_t* payload, uint16_t len);

    uart_inst_t* uart_;

    // Receive state machine.
    static constexpr size_t RX_BUF = 64;
    uint8_t  rx_buf_[RX_BUF];
    uint16_t rx_pos_ = 0;

    // Transmit mutex (shared between Core 0 and Core 1).
    mutex_t tx_mtx_;
};
