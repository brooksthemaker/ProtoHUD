#include "bt_devices.h"
#include "../theme.h"
#include <cstring>
#include <cstdio>

void BtDevicesScreen::create(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, Theme::BG(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lbl_status_ = lv_label_create(parent);
    lv_label_set_text(lbl_status_, "BT Devices (Source mode)");
    lv_obj_set_style_text_color(lbl_status_, Theme::ACCENT(), 0);
    lv_obj_set_style_text_font(lbl_status_, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_status_, LV_ALIGN_TOP_MID, 0, 4);

    list_ = lv_list_create(parent);
    lv_obj_set_size(list_, 320, 200);
    lv_obj_align(list_, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(list_, Theme::BG(), 0);
    lv_obj_set_style_border_width(list_, 0, 0);
}

void BtDevicesScreen::clear_devices() {
    devices_.clear();
    lv_obj_clean(list_);
}

void BtDevicesScreen::add_device(const BtDevice& dev) {
    devices_.push_back(dev);

    char label[64];
    snprintf(label, sizeof(label), "%s  [%d dBm]", dev.name, dev.rssi);

    lv_obj_t* btn = lv_list_add_btn(list_,
                                     dev.connected ? LV_SYMBOL_OK : LV_SYMBOL_BLUETOOTH,
                                     label);
    const lv_color_t col = dev.connected ? Theme::SUCCESS() : Theme::TEXT();
    lv_obj_set_style_text_color(lv_obj_get_child(btn, 1), col, 0);

    const size_t idx = devices_.size() - 1;
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
}

void BtDevicesScreen::set_connected(const uint8_t addr[6], bool connected) {
    // Update status label only — full refresh happens via add_device on next scan.
    char buf[48];
    if (connected) {
        // Find device name.
        for (const auto& d : devices_) {
            if (memcmp(d.addr, addr, 6) == 0) {
                snprintf(buf, sizeof(buf), "Connected: %s", d.name);
                lv_label_set_text(lbl_status_, buf);
                return;
            }
        }
    }
    lv_label_set_text(lbl_status_, "Disconnected");
}

void BtDevicesScreen::event_cb(lv_event_t* e) {
    auto* self = static_cast<BtDevicesScreen*>(lv_event_get_user_data(e));
    auto* btn  = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const size_t idx = static_cast<size_t>(
        reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));

    if (idx >= self->devices_.size()) return;
    const BtDevice& dev = self->devices_[idx];

    if (dev.connected) {
        if (self->on_disconnect_request) self->on_disconnect_request();
    } else {
        if (self->on_connect_request) self->on_connect_request(dev.addr);
    }
}
