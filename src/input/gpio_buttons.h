#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <chrono>

class GpioButtons {
public:
    using ButtonCallback = std::function<void()>;

    GpioButtons(int pin_left, int pin_right, int pin_aux,
                int af_trigger_ms = 1500, int pip_trigger_ms = 2000);
    ~GpioButtons();

    bool init();
    void shutdown();

    void on_af_left(ButtonCallback cb)   { af_left_cb_   = std::move(cb); }
    void on_af_right(ButtonCallback cb)  { af_right_cb_  = std::move(cb); }
    void on_pip_left(ButtonCallback cb)  { pip_left_cb_  = std::move(cb); }
    void on_pip_right(ButtonCallback cb) { pip_right_cb_ = std::move(cb); }
    void on_select(ButtonCallback cb)    { select_cb_    = std::move(cb); }

    bool button_left_held()  const { return button_left_held_.load(); }
    bool button_right_held() const { return button_right_held_.load(); }
    bool pip_left_active()   const { return pip_left_active_.load(); }
    bool pip_right_active()  const { return pip_right_active_.load(); }

    // Update PiP state based on current hold duration. Call each frame.
    void update_pip_state();

    // Get current hold duration for a button (0 if not held)
    int get_left_hold_ms() const;
    int get_right_hold_ms() const;

private:
    void handle_button_event(int pin, int state);

    int pin_left_, pin_right_, pin_aux_;
    int af_trigger_ms_, pip_trigger_ms_;

    std::atomic<bool> running_ { false };
    std::thread poll_thread_;

    std::atomic<bool> button_left_held_  { false };
    std::atomic<bool> button_right_held_ { false };
    std::atomic<bool> pip_left_active_   { false };
    std::atomic<bool> pip_right_active_  { false };

    std::chrono::steady_clock::time_point left_press_time_;
    std::chrono::steady_clock::time_point right_press_time_;
    bool pip_left_threshold_reached_ { false };
    bool pip_right_threshold_reached_ { false };

    ButtonCallback af_left_cb_;
    ButtonCallback af_right_cb_;
    ButtonCallback pip_left_cb_;
    ButtonCallback pip_right_cb_;
    ButtonCallback select_cb_;
};
