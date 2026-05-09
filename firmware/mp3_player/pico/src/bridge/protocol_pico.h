#pragma once
// Pico-side copy of the bridge protocol constants.
// Keep in sync with esp32_bridge/src/protocol.h.

#include <stdint.h>

static constexpr uint8_t  FRAME_MAGIC0      = 0xAA;
static constexpr uint8_t  FRAME_MAGIC1      = 0x55;
static constexpr uint16_t FRAME_HEADER_LEN  = 5;
static constexpr uint16_t FRAME_CRC_LEN     = 2;
static constexpr uint16_t FRAME_OVERHEAD    = FRAME_HEADER_LEN + FRAME_CRC_LEN;

// Pico → ESP32
static constexpr uint8_t CMD_PLAY            = 0x01;
static constexpr uint8_t CMD_PAUSE           = 0x02;
static constexpr uint8_t CMD_SET_VOLUME      = 0x03;
static constexpr uint8_t CMD_BT_SOURCE_START = 0x04;
static constexpr uint8_t CMD_BT_SINK_START   = 0x05;
static constexpr uint8_t CMD_BT_STOP         = 0x06;
static constexpr uint8_t CMD_BT_CONNECT      = 0x07;
static constexpr uint8_t CMD_AUDIO_FRAME     = 0x10;
static constexpr uint8_t CMD_MUTE            = 0x11;

// ESP32 → Pico
static constexpr uint8_t RSP_STATUS          = 0x80;
static constexpr uint8_t RSP_BT_CONNECTED    = 0x81;
static constexpr uint8_t RSP_BT_DISCONNECTED = 0x82;
static constexpr uint8_t RSP_ERROR           = 0x84;

static constexpr uint16_t AUDIO_SAMPLES_PER_FRAME = 512;
static constexpr uint16_t AUDIO_BYTES_PER_FRAME   = AUDIO_SAMPLES_PER_FRAME * 4;

#pragma pack(push, 1)
struct BtStatusPayload {
    uint8_t bt_mode;
    uint8_t connected;
    int8_t  rssi;
};
struct BtEventPayload {
    uint8_t addr[6];
    char    name[32];
};
#pragma pack(pop)

inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    return crc;
}
