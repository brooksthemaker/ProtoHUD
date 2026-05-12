#include "ble_control.h"
#include <cstring>
#include <cstdio>

BleControl::BleControl(AppState& state) : state_(state) {}

bool BleControl::begin(const char* device_name) {
    if (!BLE.begin()) return false;
    BLE.setLocalName(device_name);
    BLE.setAdvertisedService(svc_);

    svc_.addCharacteristic(ch_play_pause_);
    svc_.addCharacteristic(ch_skip_);
    svc_.addCharacteristic(ch_volume_);
    svc_.addCharacteristic(ch_mode_);
    svc_.addCharacteristic(ch_track_);
    svc_.addCharacteristic(ch_status_);
    BLE.addService(svc_);

    ch_volume_.writeValue(state_.playback.volume);

    // Register write callbacks.
    ch_play_pause_.setEventHandler(BLEWritten,
        [this](BLEDevice c, BLECharacteristic ch) { handle_write(c, ch); });
    ch_skip_.setEventHandler(BLEWritten,
        [this](BLEDevice c, BLECharacteristic ch) { handle_write(c, ch); });
    ch_volume_.setEventHandler(BLEWritten,
        [this](BLEDevice c, BLECharacteristic ch) { handle_write(c, ch); });
    ch_mode_.setEventHandler(BLEWritten,
        [this](BLEDevice c, BLECharacteristic ch) { handle_write(c, ch); });

    BLE.advertise();
    return true;
}

void BleControl::stop() {
    BLE.stopAdvertise();
    BLE.end();
}

void BleControl::task() {
    BLE.poll();
}

bool BleControl::client_connected() const {
    return BLE.connected();
}

void BleControl::notify_track(const TrackInfo& info) {
    if (!client_connected()) return;
    // Pack null-separated: title\0artist\0album\0pos_s\0dur_s
    char buf[128];
    const int n = snprintf(buf, sizeof(buf), "%s|%s|%s|%lu|%lu",
                           info.title, info.artist, info.album,
                           static_cast<unsigned long>(info.position_s),
                           static_cast<unsigned long>(info.duration_s));
    if (n > 0)
        ch_track_.writeValue(reinterpret_cast<uint8_t*>(buf),
                             static_cast<int>(n > 127 ? 127 : n));
}

void BleControl::notify_status() {
    if (!client_connected()) return;
    uint8_t buf[2] = {
        state_.playback.playing ? 1u : 0u,
        static_cast<uint8_t>(state_.mode.load())
    };
    ch_status_.writeValue(buf, 2);
}

void BleControl::handle_write(BLEDevice& /*central*/, BLECharacteristic& ch) {
    const uint8_t val = ch.value()[0];

    if (ch.uuid() == ch_play_pause_.uuid()) {
        if (on_play_pause) on_play_pause(val != 0);
    } else if (ch.uuid() == ch_skip_.uuid()) {
        if (on_skip) on_skip(val == 0xFF ? -1 : 1);
    } else if (ch.uuid() == ch_volume_.uuid()) {
        if (on_volume) on_volume(val);
    } else if (ch.uuid() == ch_mode_.uuid()) {
        if (on_mode) on_mode(static_cast<AppMode>(val));
    }
}
