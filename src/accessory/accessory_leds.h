#pragma once
// ── accessory_leds.h ──────────────────────────────────────────────────────────
// Zone-aware manager for the accessory LED chain — cheekhubs + fins on a
// single WS2812 daisy-chain off the Pi's SPI0 MOSI line. Owns the LedStrip
// + a render thread that pushes a frame every ~16 ms with the current
// per-zone pattern composited in. Patterns are stateless functions of (t,
// zone params); future audio/event hooks (volume → Level, boop → Flash) only
// have to flip a zone's pattern and update its color.
//
// Threading: all menu / hook calls go through set_zone_*; they only touch
// the per-zone Pattern state under cfg_mtx_. The render thread reads under
// the same lock, composites, and writes the strip with no other locks held.

#include "led_strip.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace accessory {

enum class Zone : uint8_t {
    LeftCheekhub  = 0,
    RightCheekhub = 1,
    LeftFin       = 2,
    RightFin      = 3,
};
constexpr int ZoneCount = 4;

enum class Pattern : uint8_t {
    Off     = 0,   // zone stays dark regardless of color
    Solid   = 1,   // fill with (r, g, b)
    Breathe = 2,   // (r, g, b) × half-cosine envelope at breathe_hz
    Level   = 3,   // (r, g, b) × current audio volume (mic-reactive bar)
};

struct ZoneConfig {
    std::string name;
    int         start = 0;
    int         count = 0;
    Pattern     pattern    = Pattern::Solid;
    uint8_t     r = 0, g = 220, b = 180;        // teal default
    float       breathe_hz = 0.5f;              // half-cycle per second
};

class AccessoryLeds {
public:
    struct Config {
        bool                          enabled = false;
        LedStrip::Config              strip;
        std::array<ZoneConfig, ZoneCount> zones{};
        uint8_t                       global_brightness = 64;
        double                        frame_hz          = 60.0;
    };

    explicit AccessoryLeds(Config cfg);
    ~AccessoryLeds();

    AccessoryLeds(const AccessoryLeds&)            = delete;
    AccessoryLeds& operator=(const AccessoryLeds&) = delete;

    bool start();
    void stop();

    bool is_running() const { return running_.load(); }

    // Per-zone tunables — picked up by the next render tick (≈ next 16 ms).
    void set_zone_pattern  (Zone, Pattern);
    void set_zone_color    (Zone, uint8_t r, uint8_t g, uint8_t b);
    void set_zone_breathe_hz(Zone, float hz);
    void set_global_brightness(uint8_t b);

    // Live audio volume in [0, 1] — drives Pattern::Level brightness. Audio
    // thread writes; render thread reads. Atomic, no lock.
    void set_audio_volume(float v) { audio_volume_.store(v); }

    // Fire a brief white flash on a zone (event overlay; survives the
    // current Pattern). duration_s controls the fade-out time. Boop events
    // call this from the sensor thread in main.cpp; the menu may also have
    // a "Test Flash" leaf later.
    void trigger_flash(Zone z, double duration_s);

    // Read-only snapshot for the menu.
    ZoneConfig zone(Zone z) const;
    uint8_t    global_brightness() const;

    // Range of LED indices for each zone, for callers that want to do their
    // own writes (e.g. event-flash overlays from main.cpp). Indices are into
    // the underlying strip's pixel array.
    int zone_start(Zone z) const { return cfg_.zones[static_cast<int>(z)].start; }
    int zone_count(Zone z) const { return cfg_.zones[static_cast<int>(z)].count; }

private:
    void render_loop();

    Config            cfg_;
    LedStrip          strip_;
    std::mutex        cfg_mtx_;       // guards cfg_.zones + global_brightness
    std::atomic<bool> running_ { false };
    std::thread       thread_;

    std::atomic<float> audio_volume_ { 0.0f };

    // Flash overlay state — paired (start, end) microseconds since steady
    // clock epoch. atomic<int64_t> keeps trigger_flash lock-free.
    using us_t = int64_t;
    std::atomic<us_t> flash_start_us_[ZoneCount]{};
    std::atomic<us_t> flash_end_us_  [ZoneCount]{};
};

} // namespace accessory
