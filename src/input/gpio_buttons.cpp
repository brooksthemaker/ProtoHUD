#include "gpio_buttons.h"

#include <gpiod.h>
#include <unistd.h>
#include <iostream>

// ── libgpiod v1 implementation ────────────────────────────────────────────────
// Raspberry Pi OS Bookworm ships libgpiod 1.6.x (v1 API).
// The v2 API (gpiod_line_settings_new, gpiod_request_config_new, etc.) is NOT
// available in the Bookworm repositories.

GpioButtons::GpioButtons(int pin_left, int pin_right, int pin_aux,
                         int af_trigger_ms, int pip_trigger_ms)
    : pin_left_(pin_left), pin_right_(pin_right), pin_aux_(pin_aux),
      af_trigger_ms_(af_trigger_ms), pip_trigger_ms_(pip_trigger_ms) {}

GpioButtons::~GpioButtons() {
    shutdown();
}

bool GpioButtons::init() {
    struct gpiod_chip* chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) {
        std::cerr << "[gpio] cannot open gpiochip0\n";
        return false;
    }

    struct gpiod_line* line_left  = gpiod_chip_get_line(chip, static_cast<unsigned>(pin_left_));
    struct gpiod_line* line_right = gpiod_chip_get_line(chip, static_cast<unsigned>(pin_right_));
    struct gpiod_line* line_aux   = gpiod_chip_get_line(chip, static_cast<unsigned>(pin_aux_));

    if (!line_left || !line_right || !line_aux) {
        std::cerr << "[gpio] failed to get GPIO lines\n";
        gpiod_chip_close(chip);
        return false;
    }

    // Build a bulk handle for all three lines
    struct gpiod_line_bulk bulk;
    gpiod_line_bulk_init(&bulk);
    gpiod_line_bulk_add(&bulk, line_left);
    gpiod_line_bulk_add(&bulk, line_right);
    gpiod_line_bulk_add(&bulk, line_aux);

    // Request both-edge events with pull-up (available since libgpiod 1.5).
    // Use explicit field assignment for C++17 compatibility (no designated
    // initializers until C++20).
    struct gpiod_line_request_config config {};
    config.consumer    = "protohud";
    config.request_type = GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
    config.flags        = GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;

    if (gpiod_line_request_bulk(&bulk, &config, nullptr) < 0) {
        std::cerr << "[gpio] failed to request GPIO line events\n";
        gpiod_chip_close(chip);
        return false;
    }

    running_ = true;
    poll_thread_ = std::thread([this, chip, line_left, line_right, line_aux]() {
        struct gpiod_line_bulk bulk_all;
        gpiod_line_bulk_init(&bulk_all);
        gpiod_line_bulk_add(&bulk_all, line_left);
        gpiod_line_bulk_add(&bulk_all, line_right);
        gpiod_line_bulk_add(&bulk_all, line_aux);

        while (running_) {
            // Wait up to 1 s for any edge event
            struct timespec timeout { 1, 0 };
            struct gpiod_line_bulk event_bulk;
            gpiod_line_bulk_init(&event_bulk);

            int ret = gpiod_line_bulk_event_wait(&bulk_all, &timeout, &event_bulk);
            if (ret < 0 || ret == 0) continue;  // error or timeout

            // Drain all pending events from the lines that fired
            unsigned int n = gpiod_line_bulk_num_lines(&event_bulk);
            for (unsigned int i = 0; i < n; i++) {
                struct gpiod_line* line = gpiod_line_bulk_get_line(&event_bulk, i);

                struct gpiod_line_event event;
                if (gpiod_line_event_read(line, &event) < 0) continue;

                unsigned int offset = gpiod_line_offset(line);
                // RISING edge = button released (pull-up, active-low)
                // FALLING edge = button pressed
                int state = (event.event_type == GPIOD_LINE_EVENT_RISING_EDGE) ? 1 : 0;
                handle_button_event(static_cast<int>(offset), state);
            }
        }

        gpiod_line_release(line_left);
        gpiod_line_release(line_right);
        gpiod_line_release(line_aux);
        gpiod_chip_close(chip);
    });

    std::cout << "[gpio] buttons initialised on GPIO " << pin_left_
              << ", " << pin_right_ << ", " << pin_aux_ << "\n";
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
            // Button pressed (falling edge)
            left_press_time_ = std::chrono::steady_clock::now();
            button_left_held_ = true;
            pip_left_threshold_reached_ = false;
        } else {
            // Button released (rising edge)
            auto hold_ms = get_left_hold_ms();
            button_left_held_ = false;
            pip_left_threshold_reached_ = false;

            if (hold_ms >= af_trigger_ms_) {
                // Long press → trigger autofocus
                if (af_left_cb_) af_left_cb_();
            } else {
                // Short press → toggle PiP
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
        // Aux button: trigger select on release
        if (state == 1) {
            if (select_cb_) select_cb_();
        }
    }
}
