#pragma once
#include <Adafruit_TinyUSB.h>
#include <SD.h>
#include <cstdint>
#include <functional>

// USB Mass Storage class backed by the microSD card.
// When the host enumerates the device, the SD is handed off exclusively to
// TinyUSB — audio playback must be stopped and SD.end() called first.
// On USB disconnect, SD.begin() is called again and playback can resume.
//
// Call begin() once from setup(). Then poll usb_connected() in the main loop
// to detect transitions; the class handles the TinyUSB callbacks internally.

class UsbMsc {
public:
    UsbMsc();

    // cs_pin: SD chip-select GPIO used to re-init the SD library on reconnect.
    bool begin(int sd_cs_pin);

    // Must be called from loop() — drives TinyUSB task.
    void task();

    bool usb_connected() const { return connected_; }
    bool msc_active()    const { return msc_active_; }

    // Fired when host connects and MSC becomes active.
    // Caller should stop audio and call SD.end() before returning.
    std::function<void()> on_msc_start;

    // Fired when host disconnects.
    // Caller should re-init SD and resume playback.
    std::function<void()> on_msc_end;

private:
    static int32_t msc_read_cb (uint32_t lba, void* buf, uint32_t bufsize);
    static int32_t msc_write_cb(uint32_t lba, uint8_t* buf, uint32_t bufsize);
    static void    msc_flush_cb();

    Adafruit_USBD_MSC msc_;
    int   sd_cs_    = -1;
    bool  connected_  = false;
    bool  msc_active_ = false;

    static UsbMsc* instance_;
};
