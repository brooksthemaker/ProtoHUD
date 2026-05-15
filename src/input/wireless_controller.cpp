#include "wireless_controller.h"
#include "../protocols.h"

#include <chrono>
#include <cstring>
#include <iostream>

using namespace std::chrono;

static int64_t now_us() {
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

WirelessController::WirelessController(const std::string& port, int baud)
    : port_(port, baud) {}

bool WirelessController::start() {
    port_.set_frame_callback([this](uint8_t cmd, const uint8_t* p, uint8_t len) {
        on_frame(cmd, p, len);
    });
    bool ok = port_.open();
    if (ok)
        std::cout << "[wireless] bridge connected on " << port_.device() << "\n";
    else
        std::cerr << "[wireless] bridge not available on " << port_.device() << "\n";
    return ok;
}

void WirelessController::stop() { port_.close(); }

bool WirelessController::connected() const {
    if (!port_.is_open()) return false;
    int64_t t = last_frame_us_.load();
    if (t == 0) return false;
    return (now_us() - t) < 5'000'000LL;  // 5-second keepalive window
}

void WirelessController::send_haptic(uint8_t pattern, uint16_t duration_ms) {
    WcHapticPayload p { pattern,
                        static_cast<uint16_t>(duration_ms & 0xFFFF) };
    port_.send(WcCmd::HAPTIC, reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

void WirelessController::send_led(uint8_t r, uint8_t g, uint8_t b) {
    WcLedPayload p { r, g, b };
    port_.send(WcCmd::LED, reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

void WirelessController::on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    last_frame_us_.store(now_us());

    if (cmd == WcCmd::BUTTON_EVENT && len >= sizeof(WcButtonPayload)) {
        WcButtonPayload ev {};
        std::memcpy(&ev, payload, sizeof(ev));

        // PiP buttons toggle a persistent atomic — read by the main loop.
        if (ev.state == 1) {
            switch (ev.button_id) {
            case WcButton::PIP_LEFT:  pip_left_  = !pip_left_.load();  return;
            case WcButton::PIP_RIGHT: pip_right_ = !pip_right_.load(); return;
            default: break;
            }
        }

        // Action buttons — fire callback on press only.
        if (ev.state != 1) return;

        auto fire = [](const std::function<void()>& cb) { if (cb) cb(); };

        switch (ev.button_id) {
        case WcButton::SELECT:    fire(select_cb_);    break;
        case WcButton::BACK:      fire(back_cb_);      break;
        case WcButton::MENU:      fire(menu_cb_);      break;
        case WcButton::AF:        fire(af_cb_);        break;
        case WcButton::CAPTURE:   fire(capture_cb_);   break;
        case WcButton::NAV_UP:    fire(nav_up_cb_);    break;
        case WcButton::NAV_DOWN:  fire(nav_down_cb_);  break;
        case WcButton::NAV_LEFT:  fire(nav_left_cb_);  break;
        case WcButton::NAV_RIGHT: fire(nav_right_cb_); break;
        default: break;
        }

    } else if (cmd == WcCmd::BATTERY && len >= 1) {
        battery_pct_.store(static_cast<int>(payload[0]));
    }
}
