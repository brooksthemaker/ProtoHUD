#pragma once
#include <ArduinoBLE.h>
#include <functional>
#include <cstdint>
#include "../app_state.h"

// BLE GATT server running on the Pico 2 W's CYW43439.
// Service UUID: 12340000-5678-1234-5678-1234567890AB (custom)
//
// Characteristics:
//   PLAY_PAUSE  0x0001  Write: 0x01 play / 0x00 pause
//   SKIP        0x0002  Write: 0x01 next / 0xFF prev
//   VOLUME      0x0003  Write/Notify: uint8_t 0–100
//   MODE        0x0004  Write: AppMode enum value
//   TRACK_INFO  0x0005  Notify: null-separated "title\0artist\0album\0pos_s\0dur_s"
//   STATUS      0x0006  Notify: uint8_t playing + uint8_t mode
//
// Designed to be used with nRF Connect or a custom companion app.

class BleControl {
public:
    explicit BleControl(AppState& state);

    bool begin(const char* device_name = "MP3Player");
    void stop();
    void task();  // call from loop() on Core 0

    // Notify phone of updated track info and playback state.
    void notify_track(const TrackInfo& info);
    void notify_status();

    bool client_connected() const;

    // Callbacks fired when phone writes a characteristic.
    std::function<void(bool play)>       on_play_pause;
    std::function<void(int8_t dir)>      on_skip;      // +1 next, -1 prev
    std::function<void(uint8_t vol)>     on_volume;
    std::function<void(AppMode mode)>    on_mode;

private:
    void handle_write(BLEDevice& central, BLECharacteristic& ch);

    AppState& state_;

    BLEService        svc_  { "12340000-5678-1234-5678-1234567890AB" };
    BLEByteCharacteristic    ch_play_pause_ { "00001001-0000-1000-8000-00805F9B34FB", BLEWrite };
    BLEByteCharacteristic    ch_skip_       { "00001002-0000-1000-8000-00805F9B34FB", BLEWrite };
    BLEByteCharacteristic    ch_volume_     { "00001003-0000-1000-8000-00805F9B34FB",
                                              BLEWrite | BLENotify };
    BLEByteCharacteristic    ch_mode_       { "00001004-0000-1000-8000-00805F9B34FB", BLEWrite };
    BLECharacteristic        ch_track_      { "00001005-0000-1000-8000-00805F9B34FB",
                                              BLENotify, 128 };
    BLECharacteristic        ch_status_     { "00001006-0000-1000-8000-00805F9B34FB",
                                              BLENotify, 2 };
};
