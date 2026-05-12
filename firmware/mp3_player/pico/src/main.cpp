#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <pico/multicore.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_state.h"
#include "settings.h"
#include "audio/sd_player.h"
#include "bridge/esp32_bridge.h"
#include "storage/sd_card.h"
#include "storage/usb_msc.h"
#include "input/encoder.h"
#include "ble/ble_control.h"
#include "ui/display.h"
#include "ui/toast.h"
#include "ui/screens/now_playing.h"
#include "ui/screens/file_browser.h"
#include "ui/screens/settings.h"
#include "ui/screens/bt_devices.h"
#include "ui/screens/charging_overlay.h"
#include "../include/pins.h"

// ── Global instances ─────────────────────────────────────────────────────

static AppState         g_state;
static SdCard           g_sd;
static SdPlayer         g_player(g_state);
static Esp32Bridge      g_bridge;
static AnoEncoder       g_encoder;
static BleControl       g_ble(g_state);
static UsbMsc           g_msc;
static Display          g_display(g_state, g_encoder);

// UI screens (created on their LVGL parent objects in setup()).
static NowPlayingScreen  g_scr_now;
static FileBrowserScreen g_scr_files;
static SettingsScreen    g_scr_settings;
static BtDevicesScreen   g_scr_bt;

static lv_obj_t* g_scr_now_obj      = nullptr;
static lv_obj_t* g_scr_files_obj    = nullptr;
static lv_obj_t* g_scr_settings_obj = nullptr;
static lv_obj_t* g_scr_bt_obj       = nullptr;

// ── Core 1 — audio decode loop ──────────────────────────────────────────────

void setup1() { /* Core 1 stack is ready */ }

void loop1() {
    g_player.run();
}

// ── Helpers ──────────────────────────────────────────────────────────

static void switch_screen(lv_obj_t* scr) {
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
}

static void apply_mode_switch(AppMode target) {
    const AppMode current = g_state.mode.load();
    if (target == current) return;

    switch (current) {
    case AppMode::SD_PLAYBACK:
        g_player.pause();
        g_bridge.mute(true);
        break;
    case AppMode::BT_SOURCE:
    case AppMode::BT_SINK:
        g_player.pause();
        g_bridge.bt_stop();
        vTaskDelay(pdMS_TO_TICKS(700));
        break;
    case AppMode::USB_MSC:
        break;
    default: break;
    }

    g_state.mode.store(target);

    switch (target) {
    case AppMode::SD_PLAYBACK:
        g_bridge.mute(false);
        g_player.play();
        switch_screen(g_scr_now_obj);
        break;
    case AppMode::BT_SOURCE:
        g_bridge.bt_source_start("MP3Player");
        g_player.play();
        switch_screen(g_scr_bt_obj);
        break;
    case AppMode::BT_SINK:
        g_bridge.bt_sink_start("MP3Player-In");
        switch_screen(g_scr_now_obj);
        break;
    case AppMode::SETTINGS:
        switch_screen(g_scr_settings_obj);
        break;
    default: break;
    }

    g_state.mode_switch_pending.store(false);
}

// ── Setup ────────────────────────────────────────────────────────────

void setup() {
    g_state.init();

    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.begin();
    Wire.setClock(400000);

    SPI.setRX(PIN_SPI_MISO);
    SPI.setTX(PIN_SPI_MOSI);
    SPI.setSCK(PIN_SPI_SCK);
    SPI.begin();

    if (g_sd.begin(PIN_SD_CS)) {
        g_state.sd_mounted = true;

        // Restore persisted settings before building the queue.
        Settings::load(g_state.playback);

        const auto tracks = g_sd.collect_tracks("/");
        TrackQueue q;
        q.load(tracks, g_state.playback.shuffled);
        g_player.load_queue(std::move(q), false);
        {
            AppLock lk(g_state);
            g_state.playback.queue_total = static_cast<uint16_t>(tracks.size());
        }
    }

    g_bridge.begin();
    g_bridge.on_bt_connected = [](bool /*connected*/, const char* name) {
        AppLock lk(g_state);
        strncpy(g_state.bt.peer_name, name, 32);
        g_state.bt.source_connected =
            (g_state.mode.load() == AppMode::BT_SOURCE);
        g_state.bt.sink_connected =
            (g_state.mode.load() == AppMode::BT_SINK);
    };
    g_bridge.on_bt_disconnected = []() {
        AppLock lk(g_state);
        g_state.bt.source_connected = false;
        g_state.bt.sink_connected   = false;
    };

    // Apply stored volume and EQ to player and bridge.
    g_player.set_volume(g_state.playback.volume);
    g_player.set_eq_preset(g_state.playback.eq_preset);
    g_bridge.set_volume(g_state.playback.volume);

    g_player.on_pcm_frame = [](const int16_t* pcm, size_t pairs) {
        g_bridge.send_audio_frame(pcm, pairs);
    };
    g_player.on_track_changed = [](const TrackInfo& info) {
        g_ble.notify_track(info);
    };

    g_display.begin();
    Toast::init();
    ChargingOverlay::init();

    g_scr_now_obj      = lv_obj_create(nullptr);
    g_scr_files_obj    = lv_obj_create(nullptr);
    g_scr_settings_obj = lv_obj_create(nullptr);
    g_scr_bt_obj       = lv_obj_create(nullptr);

    g_scr_now.create(g_scr_now_obj);
    g_scr_files.create(g_scr_files_obj);
    g_scr_settings.create(g_scr_settings_obj);
    g_scr_bt.create(g_scr_bt_obj);

    lv_scr_load(g_scr_now_obj);

    g_encoder.on_up    = []() { switch_screen(g_scr_now_obj);      };
    g_encoder.on_down  = []() { switch_screen(g_scr_files_obj);    };
    g_encoder.on_right = []() { switch_screen(g_scr_settings_obj); };
    g_encoder.on_left  = []() { switch_screen(g_scr_bt_obj);       };
    g_encoder.on_select_long = []() {
        g_player.toggle();
        g_bridge.play();
    };
    if (!g_encoder.begin(PIN_ANO_INT))
        ;

    g_scr_files.on_play = [](const std::string& path) {
        SdCard sd;
        auto tracks = sd.collect_tracks(path.substr(0, path.rfind('/')));
        TrackQueue q;
        q.load(tracks, g_state.playback.shuffled);
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i] == path) { q.jump(i); break; }
        }
        g_player.load_queue(std::move(q), true);
        switch_screen(g_scr_now_obj);
    };

    g_scr_settings.on_mode_change = [](AppMode m) {
        g_state.requested_mode.store(m);
        g_state.mode_switch_pending.store(true);
    };
    g_scr_settings.on_volume_change = [](uint8_t v) {
        g_player.set_volume(v);
        g_bridge.set_volume(v);
        char buf[24];
        snprintf(buf, sizeof(buf), "Volume: %u%%", v);
        Toast::show(buf, 1500);
        Settings::request_save({v, g_state.playback.shuffled,
                                g_state.playback.repeat, g_state.playback.eq_preset});
    };
    g_scr_settings.on_shuffle_change = [](bool s) {
        {
            AppLock lk(g_state);
            g_state.playback.shuffled = s;
        }
        Settings::request_save({g_state.playback.volume, s,
                                g_state.playback.repeat, g_state.playback.eq_preset});
    };
    g_scr_settings.on_repeat_change = [](RepeatMode r) {
        {
            AppLock lk(g_state);
            g_state.playback.repeat = r;
        }
        Settings::request_save({g_state.playback.volume, g_state.playback.shuffled,
                                r, g_state.playback.eq_preset});
    };
    g_scr_settings.on_eq_change = [](EqPreset p) {
        g_player.set_eq_preset(p);  // thread-safe: writes atomic, Core 1 picks up
        char buf[24];
        const char* names[] = {"Flat", "Bass Boost", "Vocal", "Treble", "Custom"};
        snprintf(buf, sizeof(buf), "EQ: %s",
                 names[static_cast<int>(p) < 5 ? static_cast<int>(p) : 0]);
        Toast::show(buf, 1500);
        Settings::request_save({g_state.playback.volume, g_state.playback.shuffled,
                                g_state.playback.repeat, p});
    };

    g_ble.begin("MP3Player");
    g_ble.on_play_pause = [](bool play) {
        if (play) { g_player.play(); g_bridge.play(); }
        else       { g_player.pause(); g_bridge.pause(); }
    };
    g_ble.on_skip = [](int8_t dir) {
        if (dir > 0) g_player.skip_next(); else g_player.skip_prev();
    };
    g_ble.on_volume = [](uint8_t v) {
        g_player.set_volume(v);
        g_bridge.set_volume(v);
    };
    g_ble.on_mode = [](AppMode m) {
        g_state.requested_mode.store(m);
        g_state.mode_switch_pending.store(true);
    };

    g_msc.on_msc_start = []() {
        g_player.pause();
        g_bridge.mute(true);
        g_state.usb_active = true;
        g_state.mode.store(AppMode::USB_MSC);
        SD.end();
    };
    g_msc.on_msc_end = []() {
        g_sd.begin(PIN_SD_CS);
        g_state.sd_mounted = true;
        g_state.usb_active = false;
        g_state.mode.store(AppMode::SD_PLAYBACK);
        g_bridge.mute(false);
        g_player.play();
    };
    g_msc.begin(PIN_SD_CS);

    if (g_state.sd_mounted)
        g_player.play();
}

// ── Main loop (Core 0) ──────────────────────────────────────────────────

void loop() {
    if (g_state.mode_switch_pending.load()) {
        apply_mode_switch(g_state.requested_mode.load());
    }

    static bool s_last_usb = false;
    const bool  usb_now    = g_state.usb_active;
    if (usb_now != s_last_usb) {
        s_last_usb = usb_now;
        ChargingOverlay::set_visible(usb_now);
    }

    g_msc.task();
    g_ble.task();
    g_bridge.poll();
    g_display.task();
    Toast::task();
    Settings::task();

    static uint32_t last_ui_update = 0;
    const uint32_t now = millis();
    if (now - last_ui_update >= 1000) {
        last_ui_update = now;
        g_scr_now.update(g_state);
        g_scr_settings.update(g_state);
    }

    vTaskDelay(1);
}
