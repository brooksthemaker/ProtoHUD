#pragma once
#include <cstdint>
#include "app_state.h"

// Persists playback preferences to /settings.bin on the SD card.
// Uses a 500 ms debounce so rapid encoder turns don't thrash the card.
struct Settings {
    static constexpr const char* PATH = "/settings.bin";

    // Fields that survive a power cycle.
    struct Data {
        uint8_t    volume;    // 0-100
        bool       shuffled;
        RepeatMode repeat;
        EqPreset   eq_preset;
    };

    // Populate out from SD. Silently leaves out unchanged on missing/corrupt file.
    static void load(PlaybackState& out);

    // Queue a write. Actual SD write happens in task() after DEBOUNCE_MS quiet.
    static void request_save(Data d);

    // Call once per loop() iteration.
    static void task();

private:
    static void do_save(const Data& d);

    static Data     s_pending_;
    static bool     s_dirty_;
    static uint32_t s_last_req_ms_;

    static constexpr uint32_t DEBOUNCE_MS = 500;
};
