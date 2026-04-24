/**
 * usb_protocol.h — USB serial framing for communication between the
 * RAK4631 main node and the CM5 application.
 *
 * Frame format (mirrors src/protocols.h in the CM5 project):
 *   [0xAA][0x55][CMD][LEN][...PAYLOAD...][CRC8]
 *   CRC8 covers CMD + LEN + PAYLOAD (polynomial 0x07).
 *
 * This file is Arduino-compatible (no C++ stdlib, no exceptions).
 */

#pragma once
#include <stdint.h>
#include <string.h>

// ── Frame constants ────────────────────────────────────────────────────────────
#define USB_SYNC_A       0xAA
#define USB_SYNC_B       0x55
#define USB_MAX_PAYLOAD  255
#define USB_MAX_FRAME    (4 + USB_MAX_PAYLOAD + 1)  // header + payload + CRC

// ── CRC-8 (poly 0x07, init 0x00) ─────────────────────────────────────────────
inline uint8_t usb_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
    }
    return crc;
}

/**
 * Build a framed packet into buf[].
 * @param buf     Output buffer — must be at least USB_MAX_FRAME bytes.
 * @param cmd     Command byte.
 * @param payload Payload bytes (may be NULL if len == 0).
 * @param len     Payload length (0–255).
 * @return        Total frame length in bytes.
 */
inline uint8_t usb_build_frame(uint8_t* buf, uint8_t cmd,
                                const uint8_t* payload, uint8_t len) {
    buf[0] = USB_SYNC_A;
    buf[1] = USB_SYNC_B;
    buf[2] = cmd;
    buf[3] = len;
    if (len && payload) memcpy(buf + 4, payload, len);
    buf[4 + len] = usb_crc8(buf + 2, (uint8_t)(2 + len));
    return (uint8_t)(5 + len);
}

// ── Command bytes (Radio → CM5) ───────────────────────────────────────────────
#define LORA_CMD_LOCATION_UPDATE  0x01  // LoRaLocationPayload
#define LORA_CMD_MESSAGE_RECV     0x02  // LoRaMessageHeader + text
#define LORA_CMD_RADIO_STATUS     0x03  // rssi(int8) snr(int8)
#define LORA_CMD_NODE_INFO        0x04  // NodeInfoPayload + name

// ── Command bytes (CM5 → Radio) ───────────────────────────────────────────────
#define LORA_CMD_SEND_MESSAGE     0x81  // dest_id(4) len(1) text[len]
#define LORA_CMD_REQ_STATUS       0x82  // no payload
#define LORA_CMD_PING_NODE        0x83  // node_id(4)
#define LORA_CMD_SET_WATCHLIST    0x84  // count(1) + node_id(4)*count  (max 8)

// ── Payload structs (packed, no padding) ─────────────────────────────────────
#pragma pack(push, 1)

// LORA_CMD_LOCATION_UPDATE (Radio → CM5)
// NOTE: node_id here is a compact 1-byte LOCAL index (0x01–0xFF),
//       not the full 32-bit Meshtastic/LoRa ID.  The full ID and name
//       are sent once per node via LORA_CMD_NODE_INFO.
typedef struct {
    uint8_t  local_id;       // 1-based local index assigned by this radio
    uint16_t heading_deg10;  // bearing to that node × 10  (0–3599)
    uint32_t distance_cm;    // distance in centimetres
    int8_t   rssi;           // last received RSSI in dBm
    int8_t   snr;            // last received SNR in dB (×1, integer)
} UsbLocationPayload;        // 9 bytes

// LORA_CMD_NODE_INFO (Radio → CM5) — header only; name bytes follow
typedef struct {
    uint8_t  local_id;       // same index as UsbLocationPayload.local_id
    uint32_t node_id;        // full 32-bit LoRa node ID
    uint8_t  name_len;       // length of name string that follows (0–12)
    // char name[name_len];
} UsbNodeInfoHeader;         // 6 bytes

// LORA_CMD_RADIO_STATUS (Radio → CM5)
typedef struct {
    int8_t rssi;
    int8_t snr;
} UsbRadioStatus;            // 2 bytes

// LORA_CMD_MESSAGE_RECV (Radio → CM5) — header only; text follows
typedef struct {
    uint8_t  local_id;
    uint32_t unix_timestamp;
    uint8_t  msg_len;
    // char text[msg_len];
} UsbMessageHeader;          // 6 bytes

#pragma pack(pop)
