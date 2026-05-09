#pragma once
#include <stdint.h>

// UART bridge between Pico 2 W (main controller) and ESP32 (BT audio bridge).
// Baud: 2 000 000. All frames: [MAGIC 2B][CMD 1B][LEN 2B (little-endian)][PAYLOAD][CRC16 2B]
// CRC16-CCITT (poly 0x1021, init 0xFFFF) over CMD + LEN bytes + PAYLOAD bytes.
// Magic: 0xAA 0x55.

static constexpr uint8_t  FRAME_MAGIC0 = 0xAA;
static constexpr uint8_t  FRAME_MAGIC1 = 0x55;
static constexpr uint16_t FRAME_HEADER_LEN = 5;  // magic(2) + cmd(1) + len(2)
static constexpr uint16_t FRAME_CRC_LEN    = 2;
static constexpr uint16_t FRAME_OVERHEAD   = FRAME_HEADER_LEN + FRAME_CRC_LEN;

// ── Pico → ESP32 commands ────────────────────────────────────────────────────
static constexpr uint8_t CMD_PLAY             = 0x01;  // no payload
static constexpr uint8_t CMD_PAUSE            = 0x02;  // no payload
static constexpr uint8_t CMD_SET_VOLUME       = 0x03;  // uint8_t vol (0–100)
static constexpr uint8_t CMD_BT_SOURCE_START  = 0x04;  // char name[32]
static constexpr uint8_t CMD_BT_SINK_START    = 0x05;  // char name[32]
static constexpr uint8_t CMD_BT_STOP          = 0x06;  // no payload
static constexpr uint8_t CMD_BT_CONNECT       = 0x07;  // uint8_t addr[6]
static constexpr uint8_t CMD_AUDIO_FRAME      = 0x10;  // uint16_t samples + PCM s16le stereo
static constexpr uint8_t CMD_MUTE             = 0x11;  // uint8_t on (1=mute, 0=unmute)

// ── ESP32 → Pico responses ───────────────────────────────────────────────────
static constexpr uint8_t RSP_STATUS           = 0x80;  // BtStatusPayload
static constexpr uint8_t RSP_BT_CONNECTED     = 0x81;  // BtEventPayload
static constexpr uint8_t RSP_BT_DISCONNECTED  = 0x82;  // no payload
static constexpr uint8_t RSP_ERROR            = 0x84;  // uint8_t code + char msg[32]

#pragma pack(push, 1)
struct BtStatusPayload {
    uint8_t bt_mode;      // 0=off, 1=source, 2=sink
    uint8_t connected;    // 0/1
    int8_t  rssi;         // dBm; 0 if not connected
};

struct BtEventPayload {
    uint8_t addr[6];
    char    name[32];
};

struct ErrorPayload {
    uint8_t code;
    char    msg[32];
};
#pragma pack(pop)

// Maximum audio payload: 512 stereo samples × 4 bytes (s16le L+R) = 2048 bytes
static constexpr uint16_t AUDIO_SAMPLES_PER_FRAME = 512;
static constexpr uint16_t AUDIO_BYTES_PER_FRAME   = AUDIO_SAMPLES_PER_FRAME * 4;

// CRC16-CCITT (poly 0x1021, init 0xFFFF)
inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    return crc;
}

inline uint16_t crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; ++i)
        crc = crc16_update(crc, data[i]);
    return crc;
}
