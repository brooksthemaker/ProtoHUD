#include "gpio_buttons.h"

#include <gpiod.h>
#include <iostream>

// ── libgpiod v2 implementation ────────────────────────────────────────────────
// libgpiod v2 (installed on this system: 2.2.1) completely rewrote the API.
// v1 concepts replaced:
//   gpiod_chip_open_by_name()  → gpiod_chip_open("/dev/gpiochip0")
//   gpiod_chip_get_line()      → gpiod_line_config + gpiod_chip_request_lines()
//   gpiod_line_bulk            → gpiod_edge_event_buffer
//   gpiod_line_request_bulk()  → gpiod_chip_request_lines()
//   gpiod_line_event_read()    → gpiod_line_request_read_edge_events()
//   GPIOD_LINE_EVENT_*         → GPIOD_EDGE_EVENT_*

GpioButtons::GpioButtons(int pin_left, int pin_right, int pin_aux,
                         int af_trigger_ms, int pip_trigger_ms)
    : pin_left_(pin_left), pin_right_(pin_right), pin_aux_(pin_aux),
      af_trigger_ms_(af_trigger_ms), pip_trigger_ms_(pip_trigger_ms) {}

GpioButtons::~GpioButtons() {
    shutdown();
}

bool GpioButtons::init() {
    // ── Open chip ─────────────────────────────────────────────────────────────
    gpiod_chip* chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        std::cerr << "[gpio] cannot open /dev/gpiochip0\n";
        return false;
    }

    // ── Line settings: input, pull-up, both-edge detection ───────────────────
    gpiod_line_settings* settings = gpiod_line_settings_new();
    if (!settings) {
        gpiod_chip_close(chip);
        return false;
    }
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);

    // ── Line config: apply settings to our three offsets ─────────────────────
    gpiod_line_config* line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return false;
    }
    unsigned int offsets[3] = {
        static_cast<unsigned int>(pin_left_),
        static_cast<unsigned int>(pin_right_),
        static_cast<unsigned int>(pin_aux_)
    };
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 3, settings) < 0) {
        std::cerr << "[gpio] failed to configure GPIO lines\n";
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return false;
    }

    // ── Request config: consumer name ─────────────────────────────────────────
    gpiod_request_config* req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(chip);
        return false;
    }
    gpiod_request_config_set_consumer(req_cfg, "protohud");

    // ── Request the lines ─────────────────────────────────────────────────────
    gpiod_line_request* request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);   // chip can be closed once lines are requested

    if (!request) {
        std::cerr << "[gpio] failed to request GPIO lines "
                  << pin_left_ << ", " << pin_right_ << ", " << pin_aux_ << "\n"
                  << "       Check that the user is in the 'gpio' group\n";
        return false;
    }

    // ── Poll thread ───────────────────────────────────────────────────────────
    running_ = true;
    poll_thread_ = std::thread([this, request]() {
        constexpr int BUF_CAPACITY = 16;
        gpiod_edge_event_buffer* buf = gpiod_edge_event_buffer_new(BUF_CAPACITY);

        while (running_) {
            // Wait up to 1 second for any edge event (timeout in nanoseconds)
            int ret = gpiod_line_request_wait_edge_events(request, 1'000'000'000LL);
            if (ret < 0) break;    // error — exit thread
            if (ret == 0) continue; // timeout — check running_ and loop

            // Read all pending events
            int n = gpiod_line_request_read_edge_events(request, buf, BUF_CAPACITY);
            for (int i = 0; i < n; i++) {
                gpiod_edge_event* ev = gpiod_edge_event_buffer_get_event(buf, i);
                unsigned int offset  = gpiod_edge_event_get_line_offset(ev);
                auto type            = gpiod_edge_event_get_event_type(ev);

                // Active-low wiring (button → GND, pull-up):
                //   FALLING edge = button pressed  (state 0)
                //   RISING  edge = button released (state 1)
                int state = (type == GPIOD_EDGE_EVENT_RISING_EDGE) ? 1 : 0;
                handle_button_event(static_cast<int>(offset), state);
            }
        }

        gpiod_edge_event_buffer_free(buf);
        gpiod_line_request_release(request);
    });

    std::cout << "[gpio] buttons initialised on GPIO "
              << pin_left_ << ", " << pin_right_ << ", " << pin_aux_ << "\n";
    return true;
}

void GpioButtons::shutdown() {
    running_ = false;
    if (poll_thread_.joinable()) poll_thread_.join();
}

int GpioButtons::get_left_hold_ms() const {
    if (!button_left_held_.load()) return 0;
    auto elapsed = std::chrono::steady_clock::now() - left_press_time_;
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

int GpioButtons::get_right_hold_ms() const {
    if (!button_right_held_.load()) return 0;
    auto elapsed = std::chrono::steady_clock::now() - right_press_time_;
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void GpioButtons::update_pip_state() {
    // PiP is toggled on short-press in handle_button_event();
    // this hook is available for hold-based state logic if needed in future.
}

void GpioButtons::handle_button_event(int pin, int state) {
    if (pin == pin_left_) {
        if (state == 0) {
            // Pressed
            left_press_time_ = std::chrono::steady_clock::now();
            button_left_held_ = true;
            pip_left_threshold_reached_ = false;
        } else {
            // Released
            auto hold_ms = get_left_hold_ms();
            button_left_held_ = false;
            pip_left_threshold_reached_ = false;

            if (hold_ms >= af_trigger_ms_) {
                if (af_left_cb_) af_left_cb_();
            } else {
                pip_left_active_ = !pip_left_active_.load();
                if (pip_left_cb_) pip_left_cb_();
            }
        }

    } else if (pin == pin_right_) {
        if (state == 0) {
            right_press_time_ = std::chrono::steady_clock::now();
            button_right_held_ = true;
            pip_right_threshold_reached_ = false;
        } else {
            auto hold_ms = get_right_hold_ms();
            button_right_held_ = false;
            pip_right_threshold_reached_ = false;

            if (hold_ms >= af_trigger_ms_) {
                if (af_right_cb_) af_right_cb_();
            } else {
                pip_right_active_ = !pip_right_active_.load();
                if (pip_right_cb_) pip_right_cb_();
            }
        }

    } else if (pin == pin_aux_) {
        // Aux/select: fire callback on release
        if (state == 1) {
            if (select_cb_) select_cb_();
        }
    }
}
