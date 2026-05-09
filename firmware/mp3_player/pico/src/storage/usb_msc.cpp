#include "usb_msc.h"
#include <SPI.h>

UsbMsc* UsbMsc::instance_ = nullptr;

UsbMsc::UsbMsc() { instance_ = this; }

bool UsbMsc::begin(int sd_cs_pin) {
    sd_cs_ = sd_cs_pin;

    // Vendor / product strings shown in the host OS.
    USBDevice.setManufacturerDescriptor("ProtoHUD");
    USBDevice.setProductDescriptor("MP3 Player SD Card");

    msc_.setID("ProtoHUD", "SD Card", "1.0");
    msc_.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

    // Expose the SD card as a single unit (512-byte sectors, total set after SD init).
    if (SD.begin(sd_cs_)) {
        const uint64_t sectors = SD.totalBytes() / 512;
        msc_.setCapacity(static_cast<uint32_t>(sectors), 512);
        msc_.setUnitReady(true);
    }

    msc_.begin();
    TinyUSBDevice.begin(0);  // 0 = CDC descriptor index; MSC uses next slot
    return true;
}

void UsbMsc::task() {
    TinyUSBDevice.task();
    const bool now_connected = TinyUSBDevice.mounted();
    if (now_connected == connected_) return;
    connected_ = now_connected;
    if (connected_) {
        msc_active_ = true;
        if (on_msc_start) on_msc_start();
    } else {
        msc_active_ = false;
        if (on_msc_end) on_msc_end();
    }
}

int32_t UsbMsc::msc_read_cb(uint32_t lba, void* buf, uint32_t bufsize) {
    const uint32_t n = bufsize / 512;
    if (SD.card()->readBlocks(lba, static_cast<uint8_t*>(buf), n))
        return static_cast<int32_t>(bufsize);
    return -1;
}

int32_t UsbMsc::msc_write_cb(uint32_t lba, uint8_t* buf, uint32_t bufsize) {
    const uint32_t n = bufsize / 512;
    if (SD.card()->writeBlocks(lba, buf, n))
        return static_cast<int32_t>(bufsize);
    return -1;
}

void UsbMsc::msc_flush_cb() {
    SD.card()->syncBlocks();
}
