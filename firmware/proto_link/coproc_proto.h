#pragma once
// ── coproc_proto.h ──────────────────────────────────────────────────────────────
// Shared wire contract for the ProtoHUD "proto-coproc" companion MCU (RP2350 /
// Raspberry Pi Pico 2 W). ONE header, included by BOTH sides:
//   • firmware/button_coproc/pico/src/main.cpp   (the MCU)
//   • src/input/coproc_inputs.{h,cpp}            (the CM5 host)
// so the framing, command ids, payload layouts, and CRC can never drift.
//
// The companion does three jobs, advertised in the HELLO capability list:
//   1. buttons  — debounce + short/long classification (protocol v1, ASCII)
//   2. sensors  — it is the I²C master for the boop/IMU/light chips and streams
//                 DECODED values to the CM5 (aggregator model). The CM5 applies
//                 declination / axis-remap / boop coalescing / squint logic, so
//                 this header carries RAW fused/native values + timestamps only.
//   3. panels   — it can drive MAX7219 chains from 1bpp frames the CM5 pushes.
//
// ── Framing (v2, binary) ────────────────────────────────────────────────────────
//   [0xAA][0x55][CMD u8][LEN u16 LE][PAYLOAD ...][CRC16 u16 LE]
//   CRC16-CCITT (poly 0x1021, init 0xFFFF) over CMD + the two LEN bytes + PAYLOAD.
// Identical framing to firmware/mp3_player/esp32_bridge/src/protocol.h on purpose.
//
// Back-compat: v1 was newline ASCII ("HELLO ...", "BTN <id> SHORT|LONG", "PING").
// v2 keeps HELLO and PING as ASCII lines so an old host still detects the board;
// the host reader is dual-mode (a 0xAA byte starts a binary frame, anything else
// accumulates an ASCII line). The HELLO line advertises caps so each side knows
// what the other speaks:
//   "HELLO proto-coproc v2 caps=buttons,imu_bno,imu_mpu,boop,light,panels n_btn=8 n_chain=2"

#include <stdint.h>
#include <stddef.h>

namespace coproc_proto {

// ── Framing constants ───────────────────────────────────────────────────────────
static constexpr uint8_t  MAGIC0       = 0xAA;
static constexpr uint8_t  MAGIC1       = 0x55;
static constexpr uint16_t HEADER_LEN   = 4;   // magic(2) + cmd(1) + len_lo(1)+len_hi(1) → see note
// NOTE: header on the wire is magic(2)+cmd(1)+len(2)=5 bytes; HEADER_LEN below is
// used only for buffer math where the 2 magic bytes are already consumed.
static constexpr uint16_t WIRE_HEADER  = 5;   // magic(2) + cmd(1) + len(2)
static constexpr uint16_t CRC_LEN      = 2;
static constexpr uint16_t MAX_PAYLOAD  = 1024; // largest panel frame fits easily

// ── Capability bit flags (HELLO caps= list ↔ bitmask the host keeps) ────────────
enum Caps : uint16_t {
    CAP_BUTTONS = 1u << 0,
    CAP_IMU_BNO = 1u << 1,
    CAP_IMU_MPU = 1u << 2,
    CAP_BOOP    = 1u << 3,
    CAP_LIGHT   = 1u << 4,
    CAP_PANELS  = 1u << 5,
};

// ── Command ids ─────────────────────────────────────────────────────────────────
enum Cmd : uint8_t {
    // Pico → CM5 (telemetry)
    RSP_IMU_BNO = 0x20,   // BnoPayload
    RSP_IMU_MPU = 0x21,   // MpuPayload
    RSP_BOOP    = 0x22,   // BoopPayload  (raw per-zone edges; CM5 coalesces)
    RSP_LIGHT   = 0x23,   // LightPayload
    RSP_BTN     = 0x24,   // BtnPayload   (binary equivalent of the v1 ASCII BTN)
    RSP_STATUS  = 0x2F,   // StatusPayload (binary heartbeat; ASCII "PING" also sent)

    // CM5 → Pico (control)
    CMD_PANEL_FRAME = 0x40,  // PanelFrameHeader + 1bpp packed bitmap
    CMD_PANEL_CFG   = 0x41,  // PanelCfgHeader + PanelChainCfg[n]
    CMD_CFG         = 0x42,  // CfgPayload (sensor tunables / long_ms)
    CMD_LED         = 0x43,  // LedPayload (switch backlight)
    CMD_PONG        = 0x4F,  // no payload (ack, ignored)
};

// boop edge / zone encodings (BothCheeks is derived on the CM5, not sent)
enum BoopZone : uint8_t { BOOP_SNOUT = 0, BOOP_LEFT = 1, BOOP_RIGHT = 2 };
enum BoopEdge : uint8_t { BOOP_RELEASE = 0, BOOP_PRESS = 1 };
enum BtnKind  : uint8_t { BTN_SHORT = 0, BTN_LONG = 1 };

#pragma pack(push, 1)

// BNO055: on-chip 9-DOF fusion. Raw fused values; CM5 applies declination,
// heading axis-remap and offset (mirrors src/sensor/bno055.cpp).
struct BnoPayload {
    uint32_t t_ms;          // Pico millis() at sample
    float    euler[3];      // [0]=heading, [1]=roll, [2]=pitch (deg)
    float    accel_g[3];
    float    gyro_dps[3];
    float    mag_ut[3];
    uint8_t  calib_sys;     // 0..3
    uint8_t  calib_gyro;
    uint8_t  calib_accel;
    uint8_t  calib_mag;
};

// MPU9250 + AK8963: tilt-compensated heading computed on the MCU; raw axes too.
struct MpuPayload {
    uint32_t t_ms;
    float    heading_deg;   // 0..360 (pre declination/offset)
    float    accel_g[3];
    float    gyro_dps[3];
    float    mag_ut[3];
    float    temp_c;
};

// MPR121: one frame per electrode edge. CM5 owns the BothCheeks coalesce window,
// per-zone expression, refractory and eye-trigger easter egg.
struct BoopPayload {
    uint32_t t_ms;
    uint8_t  zone;          // BoopZone
    uint8_t  edge;          // BoopEdge
};

// BH1750 ambient lux. CM5 runs the dark→bright squint edge detector.
struct LightPayload {
    uint32_t t_ms;
    float    lux;
};

struct BtnPayload {
    uint32_t t_ms;
    uint8_t  id;            // index into the firmware's kButtonPins
    uint8_t  kind;          // BtnKind
};

struct StatusPayload {
    uint32_t t_ms;
    uint16_t caps;          // Caps bitmask (also in the ASCII HELLO)
    uint8_t  n_btn;
    uint8_t  n_chain;
};

// CM5 → Pico ---------------------------------------------------------------------

// Followed by ceil(w*h/8) bytes, MSB-first, row-major 1bpp (1 = lit).
struct PanelFrameHeader {
    uint8_t  chain;         // which SPI chain (0..n_chain-1)
    uint8_t  w;             // pixels
    uint8_t  h;
};

struct PanelChainCfg {
    uint8_t  chain;
    uint8_t  modules;       // 8x8 modules in the daisy-chain
    uint8_t  order;         // 0 = row-major, 1 = serpentine
    uint8_t  intensity;     // 0..15 (MAX7219 register)
};
struct PanelCfgHeader { uint8_t n_chain; /* PanelChainCfg[n_chain] follows */ };

struct CfgPayload {
    uint32_t long_ms;       // short/long button threshold
    uint8_t  boop_touch[3]; // per-zone touch threshold (snout/left/right)
    uint8_t  boop_release[3];
};

struct LedPayload { uint8_t id; uint8_t on; };

#pragma pack(pop)

// ── CRC16-CCITT (poly 0x1021, init 0xFFFF) ──────────────────────────────────────
inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                             : static_cast<uint16_t>(crc << 1);
    return crc;
}
inline uint16_t crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; ++i) crc = crc16_update(crc, data[i]);
    return crc;
}

// Encode one frame into out (must hold WIRE_HEADER + len + CRC_LEN bytes).
// Returns the total byte count written. payload may be null when len==0.
inline size_t encode(uint8_t* out, uint8_t cmd, const void* payload, uint16_t len) {
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = cmd;
    out[3] = static_cast<uint8_t>(len & 0xFF);
    out[4] = static_cast<uint8_t>((len >> 8) & 0xFF);
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    for (uint16_t i = 0; i < len; ++i) out[WIRE_HEADER + i] = p[i];
    // CRC over cmd + len(2) + payload (i.e. everything after the magic).
    uint16_t crc = crc16(out + 2, static_cast<uint16_t>(3 + len));
    out[WIRE_HEADER + len]     = static_cast<uint8_t>(crc & 0xFF);
    out[WIRE_HEADER + len + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return WIRE_HEADER + len + CRC_LEN;
}

} // namespace coproc_proto
