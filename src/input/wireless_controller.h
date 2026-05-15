#pragma once
#include "../serial/serial_port.h"
#include <atomic>
#include <functional>
#include <string>

// Wireless in-paw / handheld controller receiver.
//
// Reads from an ESP32-C3 USB-serial bridge that:
//   (a) receives ESP-NOW button packets from the remote controller, and
//   (b) wraps them in the standard ProtoHUD serial frame for delivery here.
//
// The same bridge forwards HAPTIC and LED frames back to the controller.
//
// THREADING: action callbacks fire on the SerialPort reader thread.
//   — Navigation callbacks (menu/toast) are safe to call there (same pattern
//     as SmartKnob's on_move callback).
//   — pip_left/right_active() are std::atomic<bool> — safe to read on the
//     main thread each frame, exactly like GpioButtons::pip_left_active().
//   — Callbacks touching state under a mutex must take the lock themselves.

class WirelessController {
public:
    using Cb = std::function<void()>;

    WirelessController(const std::string& port, int baud);

    bool start();
    void stop();

    // True when the bridge serial port is open AND a frame arrived within 5 s.
    bool connected() const;

    // Battery percentage from the controller (0–100); -1 if never received.
    int  battery_pct() const { return battery_pct_.load(); }

    // PiP toggle state — read on the main thread each frame, OR'd with other
    // pip sources, exactly as GpioButtons pip state is used.
    bool pip_left_active()  const { return pip_left_.load(); }
    bool pip_right_active() const { return pip_right_.load(); }

    // Outbound feedback (no-op when not connected).
    void send_haptic(uint8_t pattern, uint16_t duration_ms);
    void send_led(uint8_t r, uint8_t g, uint8_t b);

    // Action callbacks — fire on the reader thread on button press only.
    void on_select    (Cb cb) { select_cb_    = std::move(cb); }
    void on_back      (Cb cb) { back_cb_      = std::move(cb); }
    void on_menu      (Cb cb) { menu_cb_      = std::move(cb); }
    void on_af        (Cb cb) { af_cb_        = std::move(cb); }
    void on_capture   (Cb cb) { capture_cb_   = std::move(cb); }
    void on_nav_up    (Cb cb) { nav_up_cb_    = std::move(cb); }
    void on_nav_down  (Cb cb) { nav_down_cb_  = std::move(cb); }
    void on_nav_left  (Cb cb) { nav_left_cb_  = std::move(cb); }
    void on_nav_right (Cb cb) { nav_right_cb_ = std::move(cb); }

private:
    void on_frame(uint8_t cmd, const uint8_t* payload, uint8_t len);

    SerialPort         port_;
    std::atomic<bool>  pip_left_  { false };
    std::atomic<bool>  pip_right_ { false };
    std::atomic<int>   battery_pct_ { -1 };
    std::atomic<int64_t> last_frame_us_ { 0 };  // steady_clock µs; 0 = never

    Cb select_cb_, back_cb_, menu_cb_;
    Cb af_cb_, capture_cb_;
    Cb nav_up_cb_, nav_down_cb_, nav_left_cb_, nav_right_cb_;
};
