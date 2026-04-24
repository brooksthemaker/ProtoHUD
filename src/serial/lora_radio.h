#pragma once
#include "serial_port.h"
#include "../app_state.h"
#include <string>
#include <vector>
#include <utility>  // pair

class LoRaRadio {
public:
    LoRaRadio(const std::string& port, int baud, AppState& state);

    bool start();
    void stop();
    bool connected() const;

    // ── CM5 → Radio commands ─────────────────────────────────────────────────

    // Send a UTF-8 text message to a remote node.
    // dest_id: full 32-bit hardware node ID (0xFFFFFFFF = broadcast).
    void send_message(uint32_t dest_id, const std::string& text);

    // Ask the radio firmware to transmit a LoRa ping to the given node ID,
    // forcing an immediate position response.
    void ping_node(uint32_t node_id);

    // Push a watchlist to the radio firmware.  Only nodes whose full 32-bit ID
    // appears in this list will be forwarded to the HUD.
    // Empty list restores "accept all" mode.
    void set_watchlist(const std::vector<uint32_t>& node_ids);

    // Request a RADIO_STATUS frame (RSSI/SNR of last receive).
    void request_status();

private:
    void on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len);

    SerialPort port_;
    AppState&  state_;
};
