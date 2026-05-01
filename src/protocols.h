#pragma once
#include <cstdint>
#include <cstddef>

// ── Frame format ──────────────────────────────────────────────────────────────
//   [SYNC_A][SYNC_B][CMD][LEN][...PAYLOAD...][CRC8]
//   SYNC_A = 0xAA, SYNC_B = 0x55
//   LEN = payload byte count (0-255)
//   CRC8 covers CMD+LEN+PAYLOAD

static constexpr uint8_t PROTO_SYNC_A = 0xAA;
static constexpr uint8_t PROTO_SYNC_B = 0x55;
static constexpr size_t  PROTO_HEADER_LEN = 4;  // SYNC_A SYNC_B CMD LEN
static constexpr size_t  PROTO_MAX_PAYLOAD = 255;
static constexpr size_t  PROTO_MAX_FRAME = PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + 1;

inline uint8_t proto_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
    }
    return crc;
}

// Build a frame into buf (must be >= PROTO_MAX_FRAME bytes).
// Returns total frame length.
inline size_t proto_build(uint8_t* buf, uint8_t cmd,
                          const uint8_t* payload, uint8_t len) {
    buf[0] = PROTO_SYNC_A;
    buf[1] = PROTO_SYNC_B;
    buf[2] = cmd;
    buf[3] = len;
    if (len && payload) __builtin_memcpy(buf + 4, payload, len);
    buf[4 + len] = proto_crc8(buf + 2, 2 + len);
    return 5 + len;
}

// ── Teensy (Prototracer) ──────────────────────────────────────────────────────

namespace TeensyCmd {
    // CM5 → Teensy
    static constexpr uint8_t SET_COLOR       = 0x01;  // r g b layer(1)
    static constexpr uint8_t SET_EFFECT      = 0x02;  // effect_id(1) p1(1) p2(1)
    static constexpr uint8_t PLAY_GIF        = 0x03;  // gif_id(1)
    static constexpr uint8_t SET_BRIGHTNESS  = 0x04;  // brightness(1)
    static constexpr uint8_t SET_PALETTE     = 0x05;  // palette_id(1)
    static constexpr uint8_t REQ_STATUS      = 0x06;  // no payload
    static constexpr uint8_t RELEASE_CONTROL = 0x07;  // no payload — return Teensy to autonomous mode
    static constexpr uint8_t SET_MENU_ITEM   = 0x08;  // menu_index(1) value(1)

    // Teensy → CM5
    static constexpr uint8_t STATUS          = 0x81;  // see FaceStatusPayload
}

#pragma pack(push, 1)
struct TeensyColorPayload    { uint8_t r, g, b, layer; };
struct TeensyEffectPayload   { uint8_t effect_id, p1, p2; };
struct TeensyGifPayload      { uint8_t gif_id; };
struct TeensyBrightnessPayload { uint8_t value; };
struct TeensyMenuPayload       { uint8_t menu_index, value; };

struct TeensyStatusPayload {
    uint8_t  effect_id;
    uint8_t  gif_id;
    uint8_t  r, g, b;
    uint8_t  brightness;
    uint8_t  palette_id;
    uint8_t  flags;        // bit0=playing_gif, bit1=menu_open
};
#pragma pack(pop)

// ── LoRa Radio ───────────────────────────────────────────────────────────────

namespace LoRaCmd {
    // Radio → CM5
    static constexpr uint8_t LOCATION_UPDATE = 0x01;  // see LoRaLocationPayload
    static constexpr uint8_t MESSAGE_RECV    = 0x02;  // see LoRaMessageHeader + text
    static constexpr uint8_t RADIO_STATUS    = 0x03;  // rssi(int8) snr(int8)
    static constexpr uint8_t NODE_INFO       = 0x04;  // see LoRaNodeInfoHeader + name

    // CM5 → Radio
    static constexpr uint8_t SEND_MESSAGE    = 0x81;  // dest_id(4) len(1) text[len]
    static constexpr uint8_t REQ_STATUS      = 0x82;  // no payload
    static constexpr uint8_t PING_NODE       = 0x83;  // node_id(4) — request position
    static constexpr uint8_t SET_WATCHLIST   = 0x84;  // count(1) + node_id(4)*count
}

#pragma pack(push, 1)

// LOCATION_UPDATE  (Radio → CM5)
// local_id is a compact 1-based index assigned by the radio firmware.
// The full 32-bit hardware ID and display name are delivered via NODE_INFO.
struct LoRaLocationPayload {
    uint8_t  local_id;       // 1-based index matching LoRaNodeInfoHeader::local_id
    uint16_t heading_deg10;  // bearing to that node × 10  (0–3599)
    uint32_t distance_cm;    // distance in centimetres
    int8_t   rssi;           // received signal strength (dBm)
    int8_t   snr;            // signal-to-noise ratio (dB)
};

// MESSAGE_RECV  (Radio → CM5) — followed immediately by msg_len bytes of UTF-8 text
struct LoRaMessageHeader {
    uint8_t  local_id;
    uint32_t unix_timestamp; // 0 if radio has no RTC
    uint8_t  msg_len;
};

// NODE_INFO  (Radio → CM5) — followed immediately by name_len bytes of UTF-8 name.
// Sent once when a node is first heard, then every ~60 s to survive CM5 restarts.
struct LoRaNodeInfoHeader {
    uint8_t  local_id;       // compact index used in LOCATION_UPDATE / MESSAGE_RECV
    uint32_t node_id;        // full 32-bit hardware ID from the remote radio
    uint8_t  name_len;       // bytes of name that follow (0–12)
};

#pragma pack(pop)

// ── SmartKnob (ESP32-S3) ──────────────────────────────────────────────────────

namespace KnobCmd {
    // ESP32 → CM5 (legacy position/events)
    static constexpr uint8_t POSITION_UPDATE = 0x01;  // see KnobPositionPayload
    static constexpr uint8_t WAKE_EVENT      = 0x02;  // no payload
    static constexpr uint8_t SLEEP_EVENT     = 0x03;  // no payload

    // ESP32 → CM5 (status messages from binary protocol)
    static constexpr uint8_t STATUS_READY         = 0x01;  // motor calibration complete
    static constexpr uint8_t STATUS_ENTERING_SLEEP= 0x02;  // entering low-power mode
    static constexpr uint8_t STATUS_WOKE_UP       = 0x03;  // woke from sleep (param: 0=rotation, 1=command)

    // CM5 → ESP32
    static constexpr uint8_t SET_DETENTS     = 0x81;  // count(1) positions[count*2 bytes, int16 each]
    static constexpr uint8_t WAKE_DEVICE     = 0x82;  // no payload
    static constexpr uint8_t SET_SLEEP_TMO   = 0x83;  // timeout_s(2, uint16)
    static constexpr uint8_t SET_RANGE       = 0x84;  // spacing_deg(1) min_pos(2) max_pos(2) start_pos(2)
    static constexpr uint8_t SET_HAPTIC      = 0x85;  // amplitude(1) frequency(1) detent_strength(1)
}

#pragma pack(push, 1)
struct KnobPositionPayload {
    int8_t   direction;      // -1, 0, +1
    uint16_t velocity_rpm10; // RPM * 10
    int16_t  detent_index;   // current selected detent (-1 if between)
    int32_t  angle_milli;    // absolute angle in millidegrees
};
#pragma pack(pop)
