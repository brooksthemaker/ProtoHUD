/**
 * remote_node.ino — ProtoHUD LoRa Remote / Tracked Node
 *
 * This firmware runs on any RAK4631 that should be tracked by the ProtoHUD
 * main node.  Each person/teammate carries one of these.
 *
 * Hardware:
 *   - RAK4631  (nRF52840 + SX1262, with or without 1W booster)
 *   - RAK5005-O WisBlock Base Board
 *   - RAK12501 (Quectel L76K) GPS in WisBlock Sensor Slot A  → Serial1
 *   - No USB connection to any host required at runtime.
 *
 * Required Arduino libraries (same as main_node):
 *   - RadioLib  >= 6.4.0
 *   - TinyGPSPlus >= 1.0.3
 *
 * ── Configuration ────────────────────────────────────────────────────────────
 * Edit the USER CONFIG section below, then flash.
 */

#include <Arduino.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include "../shared/lora_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════════
// USER CONFIG
// ═══════════════════════════════════════════════════════════════════════════════

// Node display name (max 12 characters).  Appears on the ProtoHUD display.
#define NODE_NAME           "Alpha"

// LoRa frequency — must match main_node and all other nodes.
#define LORA_FREQ_MHZ       915.0f

// TX power (dBm, 2–22).  Remove the booster board and lower this for low-power
// nodes without the 1W PA.  22 dBm ≈ 160 mW at SX1262 output pin.
#define LORA_TX_POWER_DBM   20

// How often to broadcast position (seconds).
#define POSITION_BROADCAST_S  30

// Set true if a 1W RAK3401 booster is fitted (enables DIO2 RF switch).
#define HAS_1W_BOOSTER      false

// ── Optional: static position fallback ──────────────────────────────────────
// If USE_STATIC_POSITION is true the node broadcasts a fixed lat/lon even
// without a GPS fix.  Useful for base stations or relay nodes.
#define USE_STATIC_POSITION false
#define STATIC_LAT          37.7749
#define STATIC_LON         -122.4194
#define STATIC_ALT_M        10.0

// ═══════════════════════════════════════════════════════════════════════════════
// WisBlock RAK4631 SX1262 pins (same as main_node)
// ═══════════════════════════════════════════════════════════════════════════════
#ifndef PIN_LORA_NSS
  #define PIN_LORA_NSS    SS
#endif
#ifndef PIN_LORA_DIO_1
  #define PIN_LORA_DIO_1  47
#endif
#ifndef PIN_LORA_RESET
  #define PIN_LORA_RESET  38
#endif
#ifndef PIN_LORA_BUSY
  #define PIN_LORA_BUSY   46
#endif

#define LORA_BW_KHZ     250.0f
#define LORA_SF         9
#define LORA_CR         5
#define LORA_PREAMBLE   8
#define LORA_SYNC_WORD  0x34
#define LORA_CRC_ENABLE true

#define GPS_BAUD        9600

// ═══════════════════════════════════════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════════════════════════════════════

SX1262   radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO_1,
                              PIN_LORA_RESET, PIN_LORA_BUSY);
TinyGPSPlus gps;

uint32_t g_node_id  = 0;
uint16_t g_seq      = 0;

double g_lat = 0.0, g_lon = 0.0, g_alt_m = 0.0;
bool   g_gps_valid = false;

uint32_t g_last_broadcast_ms = 0;

volatile bool g_lora_rx_flag = false;

// ═══════════════════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════════════════
static void     radio_isr();
static void     init_radio();
static void     poll_gps();
static void     poll_lora();
static void     transmit_position(uint8_t type);
static uint32_t read_hw_id();

// ═══════════════════════════════════════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    // Optional debug output over USB if connected to a PC (not CM5).
    Serial.begin(115200);

    g_node_id = read_hw_id();

#if USE_STATIC_POSITION
    g_lat      = STATIC_LAT;
    g_lon      = STATIC_LON;
    g_alt_m    = STATIC_ALT_M;
    g_gps_valid = true;
#else
    Serial1.begin(GPS_BAUD);
#endif

    init_radio();

    Serial.print("# Remote node ready, ID=");
    Serial.println(g_node_id, HEX);
    Serial.print("# Name: ");
    Serial.println(NODE_NAME);
}

// ═══════════════════════════════════════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════════════════════════════════════

void loop() {
#if !USE_STATIC_POSITION
    poll_gps();
#endif
    poll_lora();

    uint32_t now = millis();
    if (now - g_last_broadcast_ms >= (uint32_t)POSITION_BROADCAST_S * 1000UL) {
        g_last_broadcast_ms = now;
        if (g_gps_valid) {
            transmit_position(LORA_PKT_POSITION);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Radio
// ═══════════════════════════════════════════════════════════════════════════════

static void radio_isr() { g_lora_rx_flag = true; }

static void init_radio() {
    int state = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR,
                             LORA_SYNC_WORD, LORA_TX_POWER_DBM, LORA_PREAMBLE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("# LoRa init failed: ");
        Serial.println(state);
        while (true) delay(1000);
    }

#if HAS_1W_BOOSTER
    radio.setDio2AsRfSwitch(true);
#endif
    radio.setCRC(LORA_CRC_ENABLE ? 2 : 0);
    radio.setDio1Action(radio_isr);
    radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════════════════════
// GPS
// ═══════════════════════════════════════════════════════════════════════════════

static void poll_gps() {
    while (Serial1.available()) {
        if (gps.encode(Serial1.read())) {
            if (gps.location.isValid() && gps.location.isUpdated()) {
                g_lat     = gps.location.lat();
                g_lon     = gps.location.lng();
                g_alt_m   = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
                g_gps_valid = true;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// LoRa receive — handle pings directed at us
// ═══════════════════════════════════════════════════════════════════════════════

static void poll_lora() {
    if (!g_lora_rx_flag) return;
    g_lora_rx_flag = false;

    uint8_t buf[256];
    int     state = radio.readData(buf, sizeof(buf));

    if (state != RADIOLIB_ERR_NONE || radio.getPacketLength() < sizeof(LoRaHeader)) {
        radio.startReceive();
        return;
    }

    if (!lora_magic_ok(buf)) {
        radio.startReceive();
        return;
    }

    LoRaHeader hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.node_id == g_node_id) {
        radio.startReceive();
        return;
    }

    if (hdr.type == LORA_PKT_PING_REQ &&
        radio.getPacketLength() >= sizeof(LoRaHeader) + sizeof(LoRaPingReqPayload)) {

        LoRaPingReqPayload req;
        memcpy(&req, buf + sizeof(LoRaHeader), sizeof(req));

        if (req.target_id == g_node_id || req.target_id == 0xFFFFFFFF) {
            if (g_gps_valid) {
                // Random backoff 50–300 ms to reduce collision probability
                delay(random(50, 300));
                transmit_position(LORA_PKT_PING_ACK);
            }
        }
    }

    radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Transmit position (POSITION or PING_ACK)
// ═══════════════════════════════════════════════════════════════════════════════

static void transmit_position(uint8_t type) {
    LoRaPositionPacket pkt;
    lora_fill_header(&pkt.hdr, type, g_node_id, g_seq++);

    pkt.pos.lat_i   = (int32_t)(g_lat * 1e7);
    pkt.pos.lon_i   = (int32_t)(g_lon * 1e7);
    pkt.pos.alt_dm  = (int16_t)constrain(g_alt_m * 10.0, -32768.0, 32767.0);
    pkt.pos.hdop10  = gps.hdop.isValid() ? (uint8_t)constrain(gps.hdop.value(), 0, 255) : 255;
    pkt.pos.sats    = gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;

    const char* name = NODE_NAME;
    uint8_t nlen = (uint8_t)min((int)strlen(name), 12);
    pkt.pos.name_len = nlen;
    memcpy(pkt.pos.name, name, nlen);

    radio.transmit((uint8_t*)&pkt, sizeof(pkt));
    radio.startReceive();

    Serial.print("# TX position type=");
    Serial.println(type, HEX);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hardware ID
// ═══════════════════════════════════════════════════════════════════════════════

static uint32_t read_hw_id() {
    return NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
}
