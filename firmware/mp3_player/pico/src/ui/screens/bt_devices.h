#pragma once
#include <lvgl.h>
#include <functional>
#include <cstdint>

// BT Devices screen — lists discovered A2DP devices (populated by ESP32 bridge).
// Only relevant when mode is BT_SOURCE.

struct BtDevice {
    uint8_t addr[6];
    char    name[33];
    int8_t  rssi;
    bool    connected;
};

class BtDevicesScreen {
public:
    void create(lv_obj_t* parent);

    void clear_devices();
    void add_device(const BtDevice& dev);
    void set_connected(const uint8_t addr[6], bool connected);

    std::function<void(const uint8_t addr[6])> on_connect_request;
    std::function<void()>                       on_disconnect_request;

private:
    static void event_cb(lv_event_t* e);

    lv_obj_t*             list_        = nullptr;
    lv_obj_t*             lbl_status_  = nullptr;
    std::vector<BtDevice> devices_;
};
