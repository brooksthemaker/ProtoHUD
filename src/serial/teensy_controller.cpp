#include "teensy_controller.h"
#include "../protocols.h"
#include <chrono>
#include <cstring>
#include <thread>

TeensyController::TeensyController(const std::string& port, int baud, AppState& state)
    : port_(port, baud), state_(state) {}

bool TeensyController::start() {
    port_.set_frame_callback([this](uint8_t cmd, const uint8_t* payload, uint8_t len) {
        on_frame(cmd, payload, len);
    });

    bool ok = port_.open();
    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.teensy_ok = ok;
        state_.face.connected   = ok;
    }
    if (ok) {
        poll_running_ = true;
        poll_thread_ = std::thread([this] {
            while (poll_running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (poll_running_) request_status();
            }
        });
    }
    return ok;
}

void TeensyController::stop() {
    poll_running_ = false;
    if (poll_thread_.joinable()) poll_thread_.join();
    port_.close();
}

bool TeensyController::connected() const { return port_.is_open(); }

void TeensyController::set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer) {
    TeensyColorPayload p { r, g, b, layer };
    port_.send(TeensyCmd::SET_COLOR,
               reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.face.hud_control = true;
}

void TeensyController::set_effect(uint8_t effect_id, uint8_t p1, uint8_t p2) {
    TeensyEffectPayload p { effect_id, p1, p2 };
    port_.send(TeensyCmd::SET_EFFECT,
               reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.face.hud_control = true;
}

void TeensyController::play_gif(uint8_t gif_id) {
    TeensyGifPayload p { gif_id };
    port_.send(TeensyCmd::PLAY_GIF,
               reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.face.hud_control = true;
}

void TeensyController::set_brightness(uint8_t value) {
    TeensyBrightnessPayload p { value };
    port_.send(TeensyCmd::SET_BRIGHTNESS,
               reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

void TeensyController::set_palette(uint8_t palette_id) {
    port_.send(TeensyCmd::SET_PALETTE, &palette_id, 1);
}

void TeensyController::set_menu_item(uint8_t menu_index, uint8_t value) {
    TeensyMenuPayload p { menu_index, value };
    port_.send(TeensyCmd::SET_MENU_ITEM,
               reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

void TeensyController::request_status() {
    port_.send(TeensyCmd::REQ_STATUS);
}

void TeensyController::release_control() {
    port_.send(TeensyCmd::RELEASE_CONTROL);
    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.face.hud_control = false;
}

void TeensyController::on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (cmd == TeensyCmd::STATUS && len >= sizeof(TeensyStatusPayload)) {
        TeensyStatusPayload s {};
        std::memcpy(&s, payload, sizeof(s));

        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.face.effect_id    = s.effect_id;
        state_.face.gif_id       = s.gif_id;
        state_.face.r            = s.r;
        state_.face.g            = s.g;
        state_.face.b            = s.b;
        state_.face.brightness   = s.brightness;
        state_.face.palette_id   = s.palette_id;
        state_.face.playing_gif  = (s.flags & 0x01) != 0;
        state_.face.connected    = true;
    }
}
