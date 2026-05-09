#pragma once
#include <pico/mutex.h>
#include <atomic>
#include <cstdint>
#include <cstring>

enum class AppMode : uint8_t {
    SD_PLAYBACK = 0,
    BT_SOURCE,      // stream decoded SD audio to BT headphones via ESP32
    BT_SINK,        // receive A2DP from phone → PCM5102 via ESP32
    USB_MSC,        // SD card presented as USB mass storage
    SETTINGS,
};

enum class RepeatMode : uint8_t { OFF = 0, ONE, ALL };

struct TrackInfo {
    char    path[256]    = {};
    char    title[128]   = {};
    char    artist[128]  = {};
    char    album[128]   = {};
    uint32_t duration_s  = 0;
    uint32_t position_s  = 0;
};

struct PlaybackState {
    bool       playing   = false;
    bool       shuffled  = false;
    RepeatMode repeat    = RepeatMode::OFF;
    uint8_t    volume    = 80;
    TrackInfo  current;
    uint16_t   queue_index = 0;
    uint16_t   queue_total = 0;
};

struct BtBridgeState {
    bool    source_connected = false;
    bool    sink_connected   = false;
    char    peer_name[33]    = {};
    int8_t  rssi             = 0;
    bool    bridge_ready     = false;
};

// Shared state accessed by both cores and all tasks.
// Hot-path fields use std::atomic; multi-field reads use the pico mutex.
struct AppState {
    mutex_t mtx;

    std::atomic<AppMode>  mode          { AppMode::SD_PLAYBACK };
    std::atomic<AppMode>  requested_mode{ AppMode::SD_PLAYBACK };
    std::atomic<bool>     mode_switch_pending { false };

    // Playback (guard with mtx for multi-field reads)
    PlaybackState  playback;
    BtBridgeState  bt;

    bool sd_mounted = false;
    bool usb_active = false;

    void init() { mutex_init(&mtx); }
};

struct AppLock {
    explicit AppLock(AppState& s) : s_(s) { mutex_enter_blocking(&s_.mtx); }
    ~AppLock()                             { mutex_exit(&s_.mtx); }
private:
    AppState& s_;
};
