#pragma once
#include <cstdint>
#include <cstddef>

// ── Frame format ──────────────────────────────────────────────────────────────
//   [SYNC_A][SYNC_B][CMD][LEN][...PAYLOAD...][CRC8]
// The wire frame and its helpers (proto_crc8 / proto_build) are defined ONCE in
// the shared protocol headers vendored from the Smart-Knob-Redux repo, so the
// knob firmware and ProtoHUD can never disagree about the transport. Every
// serial device here (Teensy/LoRa/SmartKnob/wireless) shares this same frame.
#include "../vendor/smart-knob/include/proto_frame.h"

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
// Opcodes (KnobCmd::*), button/status enums, and payload structs are the single
// source of truth, vendored from the Smart-Knob-Redux firmware repo. Editing the
// knob protocol means editing vendor/smart-knob/include/knob_protocol.h and
// bumping the submodule — the two ends can no longer silently drift.
#include "../vendor/smart-knob/include/knob_protocol.h"

// ── Wireless Controller (ESP32-C3 USB-serial bridge + ESP-NOW peer) ───────────
// Frame format: same as all other serial devices — [0xAA][0x55][CMD][LEN][...][CRC8]
//
// Bridge firmware responsibilities:
//   1. Receive ESP-NOW packets from the in-paw controller.
//   2. Wrap each packet in the ProtoHUD serial frame and write to USB CDC serial.
//   3. Receive HAPTIC / LED frames from CM5 and forward via ESP-NOW to controller.
//
// Button IDs (WcButton::*) must match the controller firmware exactly.

namespace WcCmd {
    // Bridge → CM5
    static constexpr uint8_t BUTTON_EVENT = 0x01;  // WcButtonPayload
    static constexpr uint8_t BATTERY      = 0x02;  // percent(1), 0–100

    // CM5 → Bridge (forwarded to controller via ESP-NOW)
    static constexpr uint8_t HAPTIC       = 0x81;  // WcHapticPayload
    static constexpr uint8_t LED          = 0x82;  // r(1) g(1) b(1)
}

namespace WcButton {
    static constexpr uint8_t SELECT    = 0x00;  // A / primary action
    static constexpr uint8_t BACK      = 0x01;  // B / cancel
    static constexpr uint8_t PIP_LEFT  = 0x02;  // X / left shoulder tap
    static constexpr uint8_t PIP_RIGHT = 0x03;  // Y / right shoulder tap
    static constexpr uint8_t AF        = 0x04;  // LB / L1 — autofocus
    static constexpr uint8_t CAPTURE   = 0x05;  // RB / R1 — capture stereo
    static constexpr uint8_t MENU      = 0x06;  // Menu / Start
    static constexpr uint8_t NAV_UP    = 0x07;
    static constexpr uint8_t NAV_DOWN  = 0x08;
    static constexpr uint8_t NAV_LEFT  = 0x09;
    static constexpr uint8_t NAV_RIGHT = 0x0A;
}

namespace WcHapticPattern {
    static constexpr uint8_t CLICK   = 0x00;  // short single tap
    static constexpr uint8_t DOUBLE  = 0x01;  // two quick taps
    static constexpr uint8_t ERROR   = 0x02;  // long buzz
    static constexpr uint8_t SUCCESS = 0x03;  // rising double
}

#pragma pack(push, 1)
struct WcButtonPayload {
    uint8_t button_id;  // WcButton:: constant
    uint8_t state;      // 0 = released, 1 = pressed
};
struct WcHapticPayload {
    uint8_t  pattern;      // WcHapticPattern:: constant
    uint16_t duration_ms;  // total vibration duration (little-endian)
};
struct WcLedPayload { uint8_t r, g, b; };
#pragma pack(pop)
