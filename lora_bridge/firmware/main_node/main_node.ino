/**
 * main_node.ino — ProtoHUD LoRa Main Node
 *
 * Hardware:
 *   - RAK4631  (nRF52840 + SX1262 1W via RAK3401 booster) — WisBlock Core
 *   - RAK5005-O WisBlock Base Board
 *   - RAK12501 (Quectel L76K) GPS in WisBlock Sensor Slot A  → Serial1
 *   - USB CDC to CM5 Raspberry Pi CM5                        → Serial (USB)
 *
 * Required Arduino libraries (install via Library Manager):
 *   - RadioLib  >= 6.4.0   (jgromes/RadioLib)
 *   - TinyGPSPlus >= 1.0.3 (mikalhart/TinyGPSPlus)
 *
 * Board: "WisCore RAK4631 Board"  (from RAK WisBlock BSP)
 * Board Manager URL:
 *   https://raw.githubusercontent.com/RAKwireless/RAKwireless-Arduino-BSP-Index/main/package_rakwireless_index.json
 *
 * ── Configuration ────────────────────────────────────────────────────────────
 * Edit the USER CONFIG section below before flashing.
 */

#include <Arduino.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include "../shared/lora_protocol.h"
#include "../shared/usb_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════════
// USER CONFIG — edit before flashing
// ═══════════════════════════════════════════════════════════════════════════════

// LoRa frequency (MHz).  Match all nodes in your network.
//   915.0  — US / AU / NZ
//   868.0  — EU / UK (check local EIRP regulations for 1W)
//   433.0  — Asia
#define LORA_FREQ_MHZ       915.0f

// Transmit power fed into the SX1262 (dBm, 2–22).
// The RAK3401 1W booster adds ~+8 dB, giving ≈30 dBm (~1 W) at the antenna.
// Note: 1 W / 30 dBm may require a licence in some regions.  Reduce if needed.
#define LORA_TX_POWER_DBM   22

// How often this node broadcasts its own position (seconds).
#define POSITION_BROADCAST_S  30

// How often to re-send NODE_INFO packets to CM5 (seconds).
// Useful if CM5 restarts while the radio is already running.
#define NODE_INFO_REFRESH_S   60

// How many remote nodes can be tracked simultaneously.
#define MAX_WATCH_NODES       8

// ═══════════════════════════════════════════════════════════════════════════════
// WisBlock RAK4631 pin map for SX1262
// These match the RAK WisBlock BSP variant.h — verify against your BSP version.
// ═══════════════════════════════════════════════════════════════════════════════
#ifndef PIN_LORA_NSS
  #define PIN_LORA_NSS    SS          // P1.10  (42) — SPI chip-select
#endif
#ifndef PIN_LORA_DIO_1
  #define PIN_LORA_DIO_1  47          // P1.15  — IRQ line
#endif
#ifndef PIN_LORA_RESET
  #define PIN_LORA_RESET  38          // P1.06  — active-low reset
#endif
#ifndef PIN_LORA_BUSY
  #define PIN_LORA_BUSY   46          // P1.14  — BUSY line
#endif

// ── LoRa modem settings ───────────────────────────────────────────────────────
#define LORA_BW_KHZ         250.0f  // bandwidth
#define LORA_SF             9       // spreading factor (7–12)
#define LORA_CR             5       // coding rate denominator (5 = 4/5)
#define LORA_PREAMBLE       8       // preamble symbols
#define LORA_SYNC_WORD      0x34    // custom sync word (avoids Meshtastic 0x2B)
#define LORA_CRC_ENABLE     true

// ── Serial ports ──────────────────────────────────────────────────────────────
// Serial  = USB CDC → CM5
// Serial1 = WisBlock Slot A UART → RAK12501 GPS (9600 baud, NMEA)
#define GPS_BAUD            9600

// ═══════════════════════════════════════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════════════════════════════════════

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO_1,
                           PIN_LORA_RESET, PIN_LORA_BUSY);
TinyGPSPlus  gps;

// Our own node ID — derived from nRF52840 hardware FICR at boot.
uint32_t g_node_id = 0;

// Sequence counter for outbound LoRa packets.
uint16_t g_seq = 0;

// Latest own GPS fix.
double g_own_lat = 0.0, g_own_lon = 0.0, g_own_alt_m = 0.0;
bool   g_gps_valid = false;

// Timing
uint32_t g_last_broadcast_ms  = 0;
uint32_t g_last_node_info_ms  = 0;

// ── Watch-list ────────────────────────────────────────────────────────────────
// Populated at runtime by LORA_CMD_SET_WATCHLIST from CM5 (or empty = accept all).
struct WatchNode {
    uint32_t node_id;       // remote node's 32-bit ID
    uint8_t  local_id;      // compact 1-based index sent to CM5
    char     name[13];      // last received name (null-terminated)
    double   last_lat;
    double   last_lon;
    int8_t   last_rssi;
    int8_t   last_snr;
    uint32_t last_seen_ms;
    bool     info_sent;     // have we sent NODE_INFO to CM5 yet?
};
WatchNode g_nodes[MAX_WATCH_NODES];
uint8_t   g_node_count   = 0;
bool      g_accept_all   = true;  // true = track any node we hear
uint8_t   g_next_local_id = 1;

// Inbound USB frame parser state
enum class UsbRxState { SYNC_A, SYNC_B, CMD, LEN, PAYLOAD, CRC };
UsbRxState g_usb_rx_state = UsbRxState::SYNC_A;
uint8_t    g_usb_rx_cmd   = 0;
uint8_t    g_usb_rx_len   = 0;
uint8_t    g_usb_rx_buf[USB_MAX_PAYLOAD];
uint8_t    g_usb_rx_pos   = 0;

// LoRa IRQ flag — set in ISR, consumed in loop()
volatile bool g_lora_rx_flag = false;

// ═══════════════════════════════════════════════════════════════════════════════
// Forward declarations
// ═══════════════════════════════════════════════════════════════════════════════
static void     radio_isr();
static void     init_radio();
static void     init_gps();
static void     poll_gps();
static void     poll_lora();
static void     poll_usb();
static void     handle_usb_frame(uint8_t cmd, const uint8_t* payload, uint8_t len);
static void     broadcast_position();
static void     send_ping(uint32_t target_id);
static void     process_position_packet(const LoRaHeader* hdr,
                                         const LoRaPositionPayload* pos,
                                         int8_t rssi, int8_t snr);
static void     send_location_update(WatchNode* node);
static void     send_node_info(const WatchNode* node);
static void     send_radio_status(int8_t rssi, int8_t snr);
static WatchNode* find_or_add_node(uint32_t node_id);
static WatchNode* find_node(uint32_t node_id);
static double   calc_bearing(double lat1, double lon1, double lat2, double lon2);
static double   calc_distance_m(double lat1, double lon1, double lat2, double lon2);
static uint32_t read_hw_id();

// ═══════════════════════════════════════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
    // USB CDC to CM5 — wait up to 5 s for host to connect, then proceed anyway.
    Serial.begin(115200);
    for (uint32_t t = millis(); !Serial && (millis() - t < 5000);) {}

    g_node_id = read_hw_id();
    memset(g_nodes, 0, sizeof(g_nodes));

    init_gps();
    init_radio();

    Serial.print("# ProtoHUD main-node ready, ID=");
    Serial.println(g_node_id, HEX);
}

// ═══════════════════════════════════════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════════════════════════════════════

void loop() {
    poll_gps();
    poll_lora();
    poll_usb();

    uint32_t now = millis();

    // Periodic own-position broadcast over LoRa
    if (now - g_last_broadcast_ms >= (uint32_t)POSITION_BROADCAST_S * 1000UL) {
        g_last_broadcast_ms = now;
        if (g_gps_valid) broadcast_position();
    }

    // Periodic NODE_INFO refresh to CM5 (handles CM5 restarts)
    if (now - g_last_node_info_ms >= (uint32_t)NODE_INFO_REFRESH_S * 1000UL) {
        g_last_node_info_ms = now;
        for (uint8_t i = 0; i < g_node_count; i++) {
            if (g_nodes[i].node_id) send_node_info(&g_nodes[i]);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Radio init + ISR
// ═══════════════════════════════════════════════════════════════════════════════

static void radio_isr() { g_lora_rx_flag = true; }

static void init_radio() {
    int state = radio.begin(
        LORA_FREQ_MHZ,
        LORA_BW_KHZ,
        LORA_SF,
        LORA_CR,
        LORA_SYNC_WORD,
        LORA_TX_POWER_DBM,
        LORA_PREAMBLE
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("# LoRa init failed: ");
        Serial.println(state);
        while (true) delay(1000);
    }

    // RAK3401 1W booster uses SX1262 DIO2 as the RF TX/RX switch control line.
    radio.setDio2AsRfSwitch(true);

    // CRC on air packets
    radio.setCRC(LORA_CRC_ENABLE ? 2 : 0);

    // Attach receive interrupt and start listening
    radio.setDio1Action(radio_isr);
    radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════════════════════
// GPS
// ═══════════════════════════════════════════════════════════════════════════════

static void init_gps() {
    // RAK12501 (L76K) default NMEA baud on WisBlock Slot A UART
    Serial1.begin(GPS_BAUD);
}

static void poll_gps() {
    while (Serial1.available()) {
        if (gps.encode(Serial1.read())) {
            if (gps.location.isValid() && gps.location.isUpdated()) {
                g_own_lat   = gps.location.lat();
                g_own_lon   = gps.location.lng();
                g_own_alt_m = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
                g_gps_valid = true;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// LoRa receive
// ═══════════════════════════════════════════════════════════════════════════════

static void poll_lora() {
    if (!g_lora_rx_flag) return;
    g_lora_rx_flag = false;

    uint8_t buf[256];
    int     state = radio.readData(buf, sizeof(buf));

    if (state != RADIOLIB_ERR_NONE) {
        radio.startReceive();
        return;
    }

    int8_t rssi = (int8_t)constrain(radio.getRSSI(), -128, 0);
    int8_t snr  = (int8_t)constrain(radio.getSNR(),  -20,  30);

    size_t len = radio.getPacketLength();
    if (len < sizeof(LoRaHeader)) {
        radio.startReceive();
        return;
    }

    // Validate magic
    if (!lora_magic_ok(buf)) {
        radio.startReceive();
        return;
    }

    LoRaHeader hdr;
    memcpy(&hdr, buf, sizeof(LoRaHeader));

    // Ignore own packets reflected back
    if (hdr.node_id == g_node_id) {
        radio.startReceive();
        return;
    }

    switch (hdr.type) {
        case LORA_PKT_POSITION:
        case LORA_PKT_PING_ACK: {
            if (len < sizeof(LoRaHeader) + sizeof(LoRaPositionPayload)) break;
            LoRaPositionPayload pos;
            memcpy(&pos, buf + sizeof(LoRaHeader), sizeof(LoRaPositionPayload));
            process_position_packet(&hdr, &pos, rssi, snr);
            break;
        }

        case LORA_PKT_PING_REQ: {
            if (len < sizeof(LoRaHeader) + sizeof(LoRaPingReqPayload)) break;
            LoRaPingReqPayload req;
            memcpy(&req, buf + sizeof(LoRaHeader), sizeof(LoRaPingReqPayload));
            // If this ping is for us (or broadcast), respond with our position
            if (req.target_id == g_node_id || req.target_id == 0xFFFFFFFF) {
                if (g_gps_valid) {
                    delay(random(50, 200));  // jitter to avoid collisions
                    broadcast_position_type(LORA_PKT_PING_ACK);
                }
            }
            break;
        }

        case LORA_PKT_MESSAGE: {
            if (len < sizeof(LoRaHeader) + sizeof(LoRaMessageHeader)) break;
            LoRaMessageHeader mhdr;
            memcpy(&mhdr, buf + sizeof(LoRaHeader), sizeof(LoRaMessageHeader));
            if (mhdr.dest_id != g_node_id && mhdr.dest_id != 0xFFFFFFFF) break;

            // Forward message to CM5
            WatchNode* node = find_or_add_node(hdr.node_id);
            if (!node) break;

            uint8_t tlen = min((uint8_t)mhdr.msg_len,
                               (uint8_t)(len - sizeof(LoRaHeader) - sizeof(LoRaMessageHeader)));
            // UsbMessageHeader + text
            uint8_t out[USB_MAX_PAYLOAD];
            UsbMessageHeader umhdr;
            umhdr.local_id       = node->local_id;
            umhdr.unix_timestamp = 0;  // L76K doesn't give us Unix time easily; CM5 uses rx time
            umhdr.msg_len        = tlen;
            memcpy(out, &umhdr, sizeof(umhdr));
            memcpy(out + sizeof(umhdr),
                   buf + sizeof(LoRaHeader) + sizeof(LoRaMessageHeader), tlen);

            uint8_t frame[USB_MAX_FRAME];
            uint8_t flen = usb_build_frame(frame, LORA_CMD_MESSAGE_RECV,
                                            out, sizeof(umhdr) + tlen);
            Serial.write(frame, flen);
            break;
        }

        default:
            break;
    }

    radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Position packet processing → bearing/distance → CM5 USB output
// ═══════════════════════════════════════════════════════════════════════════════

static void process_position_packet(const LoRaHeader* hdr,
                                     const LoRaPositionPayload* pos,
                                     int8_t rssi, int8_t snr) {
    // Resolve or register this node
    WatchNode* node = find_or_add_node(hdr->node_id);
    if (!node) return;  // watchlist full and not previously seen

    // Update stored position
    node->last_lat    = pos->lat_i / 1e7;
    node->last_lon    = pos->lon_i / 1e7;
    node->last_rssi   = rssi;
    node->last_snr    = snr;
    node->last_seen_ms = millis();

    // Store / update name
    uint8_t nlen = min((uint8_t)pos->name_len, (uint8_t)12);
    memcpy(node->name, pos->name, nlen);
    node->name[nlen] = '\0';

    // Send NODE_INFO first if this is the first time we've seen this node,
    // or the name changed.
    if (!node->info_sent) {
        send_node_info(node);
        node->info_sent = true;
    }

    // Calculate bearing + distance only if we have a valid own position.
    if (g_gps_valid) {
        send_location_update(node);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// USB frame building helpers
// ═══════════════════════════════════════════════════════════════════════════════

static void send_location_update(WatchNode* node) {
    double dist_m   = calc_distance_m(g_own_lat, g_own_lon,
                                       node->last_lat, node->last_lon);
    double hdg_deg  = calc_bearing(g_own_lat, g_own_lon,
                                    node->last_lat, node->last_lon);

    UsbLocationPayload p;
    p.local_id      = node->local_id;
    p.heading_deg10 = (uint16_t)(hdg_deg * 10.0 + 0.5) % 3600;
    p.distance_cm   = (uint32_t)constrain(dist_m * 100.0, 0.0, 4294967295.0);
    p.rssi          = node->last_rssi;
    p.snr           = node->last_snr;

    uint8_t frame[USB_MAX_FRAME];
    uint8_t flen = usb_build_frame(frame, LORA_CMD_LOCATION_UPDATE,
                                    (const uint8_t*)&p, sizeof(p));
    Serial.write(frame, flen);
}

static void send_node_info(const WatchNode* node) {
    // Payload: UsbNodeInfoHeader + name bytes
    uint8_t buf[USB_MAX_PAYLOAD];
    UsbNodeInfoHeader h;
    h.local_id  = node->local_id;
    h.node_id   = node->node_id;
    h.name_len  = (uint8_t)strlen(node->name);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), node->name, h.name_len);

    uint8_t frame[USB_MAX_FRAME];
    uint8_t flen = usb_build_frame(frame, LORA_CMD_NODE_INFO,
                                    buf, sizeof(h) + h.name_len);
    Serial.write(frame, flen);
}

static void send_radio_status(int8_t rssi, int8_t snr) {
    UsbRadioStatus s = { rssi, snr };
    uint8_t frame[USB_MAX_FRAME];
    uint8_t flen = usb_build_frame(frame, LORA_CMD_RADIO_STATUS,
                                    (const uint8_t*)&s, sizeof(s));
    Serial.write(frame, flen);
}

// ═══════════════════════════════════════════════════════════════════════════════
// LoRa transmit helpers
// ═══════════════════════════════════════════════════════════════════════════════

/** Build and transmit a position packet (type = LORA_PKT_POSITION or PING_ACK). */
static void broadcast_position_type(uint8_t type) {
    LoRaPositionPacket pkt;
    lora_fill_header(&pkt.hdr, type, g_node_id, g_seq++);

    pkt.pos.lat_i   = (int32_t)(g_own_lat * 1e7);
    pkt.pos.lon_i   = (int32_t)(g_own_lon * 1e7);
    pkt.pos.alt_dm  = (int16_t)constrain(g_own_alt_m * 10.0, -32768, 32767);
    pkt.pos.hdop10  = gps.hdop.isValid() ? (uint8_t)constrain(gps.hdop.value(), 0, 255) : 255;
    pkt.pos.sats    = gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;

    // Embed our own name — could be made configurable; hard-coded for now.
    const char* my_name = "ProtoHUD";
    uint8_t nlen = (uint8_t)strlen(my_name);
    pkt.pos.name_len = nlen;
    memcpy(pkt.pos.name, my_name, nlen);

    radio.transmit((uint8_t*)&pkt, sizeof(pkt));
    radio.startReceive();
}

static void broadcast_position() {
    broadcast_position_type(LORA_PKT_POSITION);
}

static void send_ping(uint32_t target_id) {
    uint8_t buf[sizeof(LoRaHeader) + sizeof(LoRaPingReqPayload)];
    LoRaHeader hdr;
    lora_fill_header(&hdr, LORA_PKT_PING_REQ, g_node_id, g_seq++);
    memcpy(buf, &hdr, sizeof(hdr));

    LoRaPingReqPayload req;
    req.target_id = target_id;
    memcpy(buf + sizeof(hdr), &req, sizeof(req));

    radio.transmit(buf, sizeof(buf));
    radio.startReceive();
}

// ═══════════════════════════════════════════════════════════════════════════════
// USB command receive (framed parser)
// ═══════════════════════════════════════════════════════════════════════════════

static void poll_usb() {
    while (Serial.available()) {
        uint8_t b = Serial.read();

        switch (g_usb_rx_state) {
            case UsbRxState::SYNC_A:
                if (b == USB_SYNC_A) g_usb_rx_state = UsbRxState::SYNC_B;
                break;

            case UsbRxState::SYNC_B:
                g_usb_rx_state = (b == USB_SYNC_B)
                    ? UsbRxState::CMD : UsbRxState::SYNC_A;
                break;

            case UsbRxState::CMD:
                g_usb_rx_cmd   = b;
                g_usb_rx_state = UsbRxState::LEN;
                break;

            case UsbRxState::LEN:
                g_usb_rx_len   = b;
                g_usb_rx_pos   = 0;
                g_usb_rx_state = (b > 0) ? UsbRxState::PAYLOAD : UsbRxState::CRC;
                break;

            case UsbRxState::PAYLOAD:
                if (g_usb_rx_pos < sizeof(g_usb_rx_buf))
                    g_usb_rx_buf[g_usb_rx_pos] = b;
                g_usb_rx_pos++;
                if (g_usb_rx_pos >= g_usb_rx_len)
                    g_usb_rx_state = UsbRxState::CRC;
                break;

            case UsbRxState::CRC: {
                // Verify CRC over [CMD][LEN][PAYLOAD]
                uint8_t crc_input[2 + USB_MAX_PAYLOAD];
                crc_input[0] = g_usb_rx_cmd;
                crc_input[1] = g_usb_rx_len;
                memcpy(crc_input + 2, g_usb_rx_buf, g_usb_rx_len);
                uint8_t expected = usb_crc8(crc_input, 2 + g_usb_rx_len);

                if (b == expected) {
                    handle_usb_frame(g_usb_rx_cmd, g_usb_rx_buf, g_usb_rx_len);
                }
                g_usb_rx_state = UsbRxState::SYNC_A;
                break;
            }
        }
    }
}

static void handle_usb_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    switch (cmd) {

        case LORA_CMD_REQ_STATUS: {
            // Report the last known radio stats (or zeros if not yet received).
            send_radio_status(-100, 0);
            break;
        }

        case LORA_CMD_PING_NODE: {
            // payload: node_id (4 bytes)
            if (len < 4) break;
            uint32_t tid;
            memcpy(&tid, payload, 4);
            send_ping(tid);
            break;
        }

        case LORA_CMD_SET_WATCHLIST: {
            // payload: count(1) + [node_id(4)] * count
            if (len < 1) break;
            uint8_t count = payload[0];
            count = min(count, (uint8_t)MAX_WATCH_NODES);

            if (count == 0) {
                // Empty list = accept all
                g_accept_all  = true;
                g_node_count  = 0;
                g_next_local_id = 1;
                memset(g_nodes, 0, sizeof(g_nodes));
                break;
            }

            g_accept_all    = false;
            g_node_count    = 0;
            g_next_local_id = 1;
            memset(g_nodes, 0, sizeof(g_nodes));

            for (uint8_t i = 0; i < count; i++) {
                if (1 + i * 4 + 4 > len) break;
                uint32_t nid;
                memcpy(&nid, payload + 1 + i * 4, 4);
                g_nodes[g_node_count].node_id  = nid;
                g_nodes[g_node_count].local_id = g_next_local_id++;
                g_node_count++;
            }
            break;
        }

        case LORA_CMD_SEND_MESSAGE: {
            // payload: dest_id(4) len(1) text[len]
            if (len < 5) break;
            uint32_t dest;
            memcpy(&dest, payload, 4);
            uint8_t tlen = payload[4];
            if (tlen > len - 5) tlen = len - 5;

            // Build LoRa MESSAGE packet
            uint8_t pkt[sizeof(LoRaHeader) + sizeof(LoRaMessageHeader) + 240];
            LoRaHeader hdr;
            lora_fill_header(&hdr, LORA_PKT_MESSAGE, g_node_id, g_seq++);
            memcpy(pkt, &hdr, sizeof(hdr));

            LoRaMessageHeader mhdr;
            mhdr.dest_id  = dest;
            mhdr.msg_len  = tlen;
            memcpy(pkt + sizeof(hdr), &mhdr, sizeof(mhdr));
            memcpy(pkt + sizeof(hdr) + sizeof(mhdr), payload + 5, tlen);

            radio.transmit(pkt, sizeof(hdr) + sizeof(mhdr) + tlen);
            radio.startReceive();
            break;
        }

        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Node table helpers
// ═══════════════════════════════════════════════════════════════════════════════

static WatchNode* find_node(uint32_t node_id) {
    for (uint8_t i = 0; i < g_node_count; i++) {
        if (g_nodes[i].node_id == node_id) return &g_nodes[i];
    }
    return nullptr;
}

static WatchNode* find_or_add_node(uint32_t node_id) {
    WatchNode* existing = find_node(node_id);
    if (existing) return existing;

    // If we're not in accept-all mode, only add nodes that were pre-registered
    // (find_node already returned nullptr, so it wasn't in the list).
    if (!g_accept_all) return nullptr;

    // In accept-all mode, add any new node if there's room.
    if (g_node_count >= MAX_WATCH_NODES) return nullptr;

    g_nodes[g_node_count].node_id   = node_id;
    g_nodes[g_node_count].local_id  = g_next_local_id++;
    g_nodes[g_node_count].info_sent = false;
    return &g_nodes[g_node_count++];
}

// ═══════════════════════════════════════════════════════════════════════════════
// Navigation math (double precision — nRF52840 has FPU)
// ═══════════════════════════════════════════════════════════════════════════════

static const double DEG2RAD = M_PI / 180.0;
static const double R_EARTH = 6371000.0;  // metres

static double calc_distance_m(double lat1, double lon1,
                                double lat2, double lon2) {
    double phi1  = lat1 * DEG2RAD, phi2 = lat2 * DEG2RAD;
    double dphi  = (lat2 - lat1) * DEG2RAD;
    double dlam  = (lon2 - lon1) * DEG2RAD;
    double a = sin(dphi / 2.0) * sin(dphi / 2.0)
             + cos(phi1) * cos(phi2) * sin(dlam / 2.0) * sin(dlam / 2.0);
    return R_EARTH * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static double calc_bearing(double lat1, double lon1,
                             double lat2, double lon2) {
    double phi1 = lat1 * DEG2RAD, phi2 = lat2 * DEG2RAD;
    double dlam = (lon2 - lon1) * DEG2RAD;
    double x    = sin(dlam) * cos(phi2);
    double y    = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlam);
    double b    = atan2(x, y) * (180.0 / M_PI);
    return fmod(b + 360.0, 360.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hardware ID
// ═══════════════════════════════════════════════════════════════════════════════

static uint32_t read_hw_id() {
    // nRF52840 FICR: 64-bit device ID — fold to 32 bits for our protocol
    uint32_t id0 = NRF_FICR->DEVICEID[0];
    uint32_t id1 = NRF_FICR->DEVICEID[1];
    return id0 ^ id1;
}
