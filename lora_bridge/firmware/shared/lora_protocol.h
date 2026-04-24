/**
 * lora_protocol.h — Over-the-air LoRa packet definitions.
 *
 * Shared between main_node and remote_node firmware.
 * All multi-byte fields are LITTLE-ENDIAN.
 *
 * Frame layout:
 *   [MAGIC_0][MAGIC_1][TYPE][NODE_ID:4][SEQ:2][...payload...]
 *   Total header = 8 bytes.  Max payload = 240 bytes (fits in one LoRa frame).
 */

#pragma once
#include <stdint.h>

// ── Magic bytes ────────────────────────────────────────────────────────────────
// Used as the first two bytes of every over-the-air frame so receivers can
// quickly reject foreign LoRa traffic before attempting to parse it.
static const uint8_t LORA_MAGIC[2] = { 0xB0, 0xCA };  // "BoCA" sentinel

// ── Packet types ───────────────────────────────────────────────────────────────
#define LORA_PKT_POSITION   0x01   // Node broadcasts its GPS position + name
#define LORA_PKT_PING_REQ   0x02   // Request immediate position from target node
#define LORA_PKT_PING_ACK   0x03   // Response to ping (same layout as POSITION)
#define LORA_PKT_MESSAGE    0x04   // Plain-text message

// ── Header ─────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)

struct LoRaHeader {
    uint8_t  magic[2];       // LORA_MAGIC
    uint8_t  type;           // LORA_PKT_*
    uint32_t node_id;        // Sender's 32-bit unique ID
    uint16_t seq;            // Sequence counter (wraps at 65535)
};
static_assert(sizeof(LoRaHeader) == 8, "LoRaHeader must be 8 bytes");

// ── LORA_PKT_POSITION / LORA_PKT_PING_ACK ────────────────────────────────────
// Payload immediately following LoRaHeader.
struct LoRaPositionPayload {
    int32_t  lat_i;          // latitude  * 1e7  (degrees, signed)
    int32_t  lon_i;          // longitude * 1e7  (degrees, signed)
    int16_t  alt_dm;         // altitude in decimetres above MSL (signed)
    uint8_t  hdop10;         // HDOP * 10 (0 = unknown, 255 = very bad)
    uint8_t  sats;           // satellites in use
    uint8_t  name_len;       // length of the following name string (0–12)
    char     name[12];       // UTF-8 node name, NOT null-terminated
};
static_assert(sizeof(LoRaPositionPayload) == 22, "LoRaPositionPayload must be 22 bytes");

// ── LORA_PKT_PING_REQ ────────────────────────────────────────────────────────
struct LoRaPingReqPayload {
    uint32_t target_id;      // Node ID being pinged (0xFFFFFFFF = broadcast ping)
};

// ── LORA_PKT_MESSAGE ─────────────────────────────────────────────────────────
struct LoRaMessageHeader {
    uint32_t dest_id;        // 0xFFFFFFFF = broadcast
    uint8_t  msg_len;        // bytes of text that follow
    // char text[msg_len];
};

#pragma pack(pop)

// ── Convenience: full position packet ────────────────────────────────────────
struct LoRaPositionPacket {
    LoRaHeader          hdr;
    LoRaPositionPayload pos;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

inline bool lora_magic_ok(const uint8_t* buf) {
    return buf[0] == LORA_MAGIC[0] && buf[1] == LORA_MAGIC[1];
}

/** Fill a LoRaHeader. seq is caller-managed. */
inline void lora_fill_header(LoRaHeader* h, uint8_t type,
                              uint32_t node_id, uint16_t seq) {
    h->magic[0] = LORA_MAGIC[0];
    h->magic[1] = LORA_MAGIC[1];
    h->type     = type;
    h->node_id  = node_id;
    h->seq      = seq;
}
