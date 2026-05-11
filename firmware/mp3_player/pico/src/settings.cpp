#include "settings.h"
#include <SD.h>
#include <Arduino.h>

Settings::Data  Settings::s_pending_     = {};
bool            Settings::s_dirty_       = false;
uint32_t        Settings::s_last_req_ms_ = 0;

// Binary record layout (10 bytes):
//   [0..2]  magic: 'P' 'H' 'S'
//   [3]     version: 0x01
//   [4]     volume  (0-100)
//   [5]     shuffled (0/1)
//   [6]     repeat  (RepeatMode)
//   [7..8]  reserved (0)
//   [9]     XOR checksum of bytes 0..8
static constexpr uint8_t MAGIC[3] = {'P', 'H', 'S'};
static constexpr uint8_t VERSION  = 0x01;

void Settings::load(PlaybackState& out) {
    File f = SD.open(PATH, FILE_READ);
    if (!f) return;
    uint8_t buf[10];
    const size_t n = f.read(buf, 10);
    f.close();
    if (n != 10) return;
    if (buf[0] != MAGIC[0] || buf[1] != MAGIC[1] || buf[2] != MAGIC[2]) return;
    if (buf[3] != VERSION) return;
    uint8_t csum = 0;
    for (int i = 0; i < 9; ++i) csum ^= buf[i];
    if (csum != buf[9]) return;

    if (buf[4] <= 100) out.volume = buf[4];
    out.shuffled = (buf[5] != 0);
    if (buf[6] <= static_cast<uint8_t>(RepeatMode::ALL))
        out.repeat = static_cast<RepeatMode>(buf[6]);
}

void Settings::request_save(Data d) {
    s_pending_     = d;
    s_dirty_       = true;
    s_last_req_ms_ = millis();
}

void Settings::task() {
    if (!s_dirty_) return;
    if (millis() - s_last_req_ms_ < DEBOUNCE_MS) return;
    s_dirty_ = false;
    do_save(s_pending_);
}

void Settings::do_save(const Data& in) {
    uint8_t buf[10] = {};
    buf[0] = MAGIC[0]; buf[1] = MAGIC[1]; buf[2] = MAGIC[2];
    buf[3] = VERSION;
    buf[4] = in.volume;
    buf[5] = in.shuffled ? 1u : 0u;
    buf[6] = static_cast<uint8_t>(in.repeat);
    // buf[7] and buf[8] are reserved, already 0
    uint8_t csum = 0;
    for (int i = 0; i < 9; ++i) csum ^= buf[i];
    buf[9] = csum;

    // Overwrite existing file (SD library FILE_WRITE truncates on open).
    SD.remove(PATH);
    File f = SD.open(PATH, FILE_WRITE);
    if (!f) return;
    f.write(buf, 10);
    f.close();
}
