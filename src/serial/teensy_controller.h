#pragma once
#include "face_controller.h"
#include "serial_port.h"
#include "../app_state.h"
#include <atomic>
#include <string>
#include <thread>

// Manages bidirectional communication with the Teensy 4.0 running Prototracer.
// Reads status frames and updates AppState::face.
// Exposes typed command methods for the CM5 to call.
class TeensyController : public IFaceController {
public:
    TeensyController(const std::string& port, int baud, AppState& state);

    bool start()          override;
    void stop()           override;
    bool connected() const override;

    // Commands: CM5 → Teensy
    void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer = 0) override;
    void set_effect(uint8_t effect_id, uint8_t p1 = 0, uint8_t p2 = 0) override;
    void play_gif(uint8_t gif_id)              override;
    void set_brightness(uint8_t value)         override;
    void set_palette(uint8_t palette_id)       override;
    void set_menu_item(uint8_t menu_index, uint8_t value) override;
    void request_status()                      override;
    void release_control()                     override;

private:
    void on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len);

    SerialPort           port_;
    AppState&            state_;
    std::thread          poll_thread_;
    std::atomic<bool>    poll_running_ { false };
};
