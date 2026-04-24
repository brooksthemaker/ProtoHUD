#include "lora_radio.h"
#include "../protocols.h"
#include <cstring>
#include <ctime>
#include <algorithm>

LoRaRadio::LoRaRadio(const std::string& port, int baud, AppState& state)
    : port_(port, baud), state_(state) {}

bool LoRaRadio::start() {
    port_.set_frame_callback([this](uint8_t cmd, const uint8_t* payload, uint8_t len) {
        on_frame(cmd, payload, len);
    });

    bool ok = port_.open();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.lora_ok = ok;
    }
    return ok;
}

void LoRaRadio::stop()          { port_.close(); }
bool LoRaRadio::connected() const { return port_.is_open(); }

// ── CM5 → Radio ──────────────────────────────────────────────────────────────

void LoRaRadio::send_message(uint32_t dest_id, const std::string& text) {
    uint8_t tlen = static_cast<uint8_t>(std::min(text.size(), size_t(240)));
    std::vector<uint8_t> payload(5 + tlen);
    // dest_id (4 bytes LE) + len(1) + text
    payload[0] = static_cast<uint8_t>(dest_id);
    payload[1] = static_cast<uint8_t>(dest_id >> 8);
    payload[2] = static_cast<uint8_t>(dest_id >> 16);
    payload[3] = static_cast<uint8_t>(dest_id >> 24);
    payload[4] = tlen;
    std::memcpy(payload.data() + 5, text.data(), tlen);
    port_.send(LoRaCmd::SEND_MESSAGE, payload.data(),
               static_cast<uint8_t>(payload.size()));
}

void LoRaRadio::ping_node(uint32_t node_id) {
    uint8_t payload[4];
    payload[0] = static_cast<uint8_t>(node_id);
    payload[1] = static_cast<uint8_t>(node_id >> 8);
    payload[2] = static_cast<uint8_t>(node_id >> 16);
    payload[3] = static_cast<uint8_t>(node_id >> 24);
    port_.send(LoRaCmd::PING_NODE, payload, sizeof(payload));
}

void LoRaRadio::set_watchlist(const std::vector<uint32_t>& node_ids) {
    uint8_t count = static_cast<uint8_t>(std::min(node_ids.size(), size_t(8)));
    std::vector<uint8_t> payload(1 + count * 4);
    payload[0] = count;
    for (uint8_t i = 0; i < count; i++) {
        uint32_t id = node_ids[i];
        payload[1 + i * 4 + 0] = static_cast<uint8_t>(id);
        payload[1 + i * 4 + 1] = static_cast<uint8_t>(id >> 8);
        payload[1 + i * 4 + 2] = static_cast<uint8_t>(id >> 16);
        payload[1 + i * 4 + 3] = static_cast<uint8_t>(id >> 24);
    }
    port_.send(LoRaCmd::SET_WATCHLIST, payload.data(),
               static_cast<uint8_t>(payload.size()));
}

void LoRaRadio::request_status() {
    port_.send(LoRaCmd::REQ_STATUS);
}

// ── Radio → CM5 (inbound frames) ─────────────────────────────────────────────

void LoRaRadio::on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {

    switch (cmd) {

    // ── LOCATION_UPDATE ───────────────────────────────────────────────────────
    case LoRaCmd::LOCATION_UPDATE: {
        if (len < sizeof(LoRaLocationPayload)) break;

        LoRaLocationPayload p{};
        std::memcpy(&p, payload, sizeof(p));

        LoRaNode node{};
        node.local_id    = p.local_id;
        node.heading_deg = p.heading_deg10 / 10.0f;
        node.distance_m  = p.distance_cm  / 100.0f;
        node.rssi        = p.rssi;
        node.snr         = p.snr;
        node.last_seen   = std::time(nullptr);

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.health.lora_ok = true;

            // Preserve name and full node_id from any prior NODE_INFO packet.
            for (const auto& existing : state_.lora_nodes) {
                if (existing.local_id == node.local_id) {
                    node.node_id = existing.node_id;
                    node.name    = existing.name;
                    break;
                }
            }
            state_.upsert_lora_node(node);
        }
        break;
    }

    // ── NODE_INFO ─────────────────────────────────────────────────────────────
    case LoRaCmd::NODE_INFO: {
        if (len < sizeof(LoRaNodeInfoHeader)) break;

        LoRaNodeInfoHeader hdr{};
        std::memcpy(&hdr, payload, sizeof(hdr));

        uint8_t available = len - sizeof(LoRaNodeInfoHeader);
        uint8_t nlen      = std::min(hdr.name_len, available);

        std::string name(reinterpret_cast<const char*>(payload + sizeof(hdr)), nlen);

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.health.lora_ok = true;
            state_.upsert_lora_node_info(hdr.local_id, hdr.node_id, name);
        }
        break;
    }

    // ── MESSAGE_RECV ──────────────────────────────────────────────────────────
    case LoRaCmd::MESSAGE_RECV: {
        if (len < sizeof(LoRaMessageHeader)) break;

        LoRaMessageHeader hdr{};
        std::memcpy(&hdr, payload, sizeof(hdr));

        uint8_t available = len - sizeof(LoRaMessageHeader);
        uint8_t text_len  = std::min(hdr.msg_len, available);

        LoRaMessage msg{};
        msg.local_id  = hdr.local_id;
        msg.timestamp = hdr.unix_timestamp
                      ? static_cast<time_t>(hdr.unix_timestamp)
                      : std::time(nullptr);
        msg.text.assign(
            reinterpret_cast<const char*>(payload + sizeof(hdr)), text_len);
        msg.read = false;

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.health.lora_ok = true;
            state_.push_lora_message(std::move(msg));
        }
        break;
    }

    // ── RADIO_STATUS ──────────────────────────────────────────────────────────
    case LoRaCmd::RADIO_STATUS: {
        if (len < 2) break;
        // Currently just marks the radio as alive; could surface RSSI/SNR
        // to a dedicated AppState field if needed.
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.lora_ok = true;
        break;
    }

    default:
        break;
    }
}
