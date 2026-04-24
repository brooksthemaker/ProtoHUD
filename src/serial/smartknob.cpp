#include "smartknob.h"
#include "../protocols.h"
#include <cstring>
#include <vector>

SmartKnob::SmartKnob(const std::string& port, int baud, AppState& state)
    : port_(port, baud), state_(state) {}

bool SmartKnob::start() {
    port_.set_frame_callback([this](uint8_t cmd, const uint8_t* p, uint8_t len) {
        on_frame(cmd, p, len);
    });

    bool ok = port_.open();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.knob_ok     = ok;
        state_.knob.connected     = ok;
        state_.knob.awake         = ok;
    }
    return ok;
}

void SmartKnob::stop() { port_.close(); }
bool SmartKnob::connected() const { return port_.is_open(); }

void SmartKnob::set_detents(int count, const std::vector<int16_t>& positions) {
    if (count < 0 || count > 60) return;

    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(count));

    if (positions.empty()) {
        // Evenly spread over 360°
        for (int i = 0; i < count; i++) {
            int16_t angle = (count > 0)
                ? static_cast<int16_t>((360000 / count) * i)
                : 0;
            payload.push_back(static_cast<uint8_t>(angle & 0xFF));
            payload.push_back(static_cast<uint8_t>((angle >> 8) & 0xFF));
        }
    } else {
        for (int16_t a : positions) {
            payload.push_back(static_cast<uint8_t>(a & 0xFF));
            payload.push_back(static_cast<uint8_t>((a >> 8) & 0xFF));
        }
    }

    port_.send(KnobCmd::SET_DETENTS, payload.data(),
               static_cast<uint8_t>(payload.size()));
}

void SmartKnob::wake() {
    port_.send(KnobCmd::WAKE_DEVICE);
}

void SmartKnob::set_sleep_timeout(uint16_t seconds) {
    uint8_t buf[2] = { static_cast<uint8_t>(seconds & 0xFF),
                       static_cast<uint8_t>((seconds >> 8) & 0xFF) };
    port_.send(KnobCmd::SET_SLEEP_TMO, buf, 2);
}

void SmartKnob::set_haptic(uint8_t amplitude, uint8_t frequency, uint8_t detent_strength) {
    uint8_t buf[3] = { amplitude, frequency, detent_strength };
    port_.send(KnobCmd::SET_HAPTIC, buf, 3);
}

void SmartKnob::on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (cmd == KnobCmd::POSITION_UPDATE && len >= sizeof(KnobPositionPayload)) {
        KnobPositionPayload p {};
        std::memcpy(&p, payload, sizeof(p));

        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.knob.direction    = p.direction;
            state_.knob.velocity_rpm = p.velocity_rpm10 / 10.0f;
            state_.knob.detent_index = p.detent_index;
            state_.knob.angle_milli  = p.angle_milli;
            state_.knob.awake        = true;
        }

        if (move_cb_ && p.direction != 0)
            move_cb_(p.direction, p.detent_index);

    } else if (cmd == KnobCmd::WAKE_EVENT) {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.knob.awake = true;
        }
        if (wake_cb_) wake_cb_();

    } else if (cmd == KnobCmd::SLEEP_EVENT) {
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.knob.awake = false;
        }

    } else if (cmd == 0x01) {  // STATUS_READY
        if (status_cb_) status_cb_(0x01, 0);

    } else if (cmd == 0x02) {  // STATUS_ENTERING_SLEEP
        if (status_cb_) status_cb_(0x02, 0);

    } else if (cmd == 0x03) {  // STATUS_WOKE_UP
        uint8_t reason = (len >= 1) ? payload[0] : 0;
        if (status_cb_) status_cb_(0x03, reason);
    }
}
