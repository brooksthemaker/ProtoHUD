// ── peripherals.cpp — coprocessor peripheral hub (core0) ─────────────────────
// See peripherals.h. Three services, all cooperative so the button loop never
// stalls more than one bit-bang transaction (~7 ms worst case):
//   * MPR121 boop pads on the shared I2C0 bus (same wires as the voice DAC —
//     0x5A next to 0x18). Touch/release edges → "BOOP <electrode> <1|0>".
//   * DS18B20 probes on a bit-banged 1-Wire bus. A convert-all is issued, then
//     ONE probe's scratchpad is read per service pass → "TEMP <rom> <milli°C>".
//   * Fan PWM zones driven by "FAN <zone> <duty%>" from the Pi (the temp→duty
//     curve stays on the CM5; this end just holds the requested duty).

#ifdef PERIPHERAL_HUB

#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "peripherals.h"

namespace {

// ── MPR121 boop pads ──────────────────────────────────────────────────────────
bool     g_mpr_ok      = false;
uint16_t g_mpr_last    = 0;
uint32_t g_mpr_next_ms = 2500;   // first attempt after the DAC (core1) owns I2C0

void mpr_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(kMpr121Addr);
    Wire.write(reg); Wire.write(val);
    Wire.endTransmission();
}

bool mpr_init() {
    mpr_write(0x80, 0x63);          // soft reset
    delay(1);
    mpr_write(0x5E, 0x00);          // stop mode (electrodes off) while configuring
    // Baseline/filter defaults per the MPR121 datasheet / common breakout usage.
    const uint8_t cfg[][2] = {
        {0x2B,0x01},{0x2C,0x01},{0x2D,0x0E},{0x2E,0x00},   // rising
        {0x2F,0x01},{0x30,0x05},{0x31,0x01},{0x32,0x00},   // falling
        {0x33,0x00},{0x34,0x00},{0x35,0x00},               // touched
        {0x5B,0x00},{0x5C,0x10},{0x5D,0x20},               // debounce, CDC, CDT
    };
    for (const auto& c : cfg) mpr_write(c[0], c[1]);
    for (int i = 0; i < 12; ++i) {                          // touch=12 / release=6
        mpr_write(0x41 + 2 * i, 12);
        mpr_write(0x42 + 2 * i, 6);
    }
    mpr_write(0x5E, 0x8F);          // run: baseline tracking + 12 electrodes
    // Presence check: CDT/CONFIG2 must read back what we wrote.
    Wire.beginTransmission(kMpr121Addr);
    Wire.write(0x5D);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(kMpr121Addr), 1) != 1) return false;
    return Wire.read() == 0x20;
}

void mpr_service(uint32_t now) {
    if (static_cast<int32_t>(now - g_mpr_next_ms) < 0) return;
    if (!g_mpr_ok) {
        g_mpr_ok = mpr_init();
        g_mpr_next_ms = now + (g_mpr_ok ? kBoopPollMs : 5000);   // retry if absent
        return;
    }
    g_mpr_next_ms = now + kBoopPollMs;
    Wire.beginTransmission(kMpr121Addr);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) { g_mpr_ok = false; return; }
    if (Wire.requestFrom(static_cast<int>(kMpr121Addr), 2) != 2) { g_mpr_ok = false; return; }
    uint16_t t = Wire.read();
    t |= static_cast<uint16_t>(Wire.read()) << 8;
    t &= 0x0FFF;
    const uint16_t changed = t ^ g_mpr_last;
    for (int e = 0; e < 12; ++e)
        if (changed & (1u << e)) {
            Serial.print("BOOP "); Serial.print(e);
            Serial.print(' ');     Serial.println((t >> e) & 1);
        }
    g_mpr_last = t;
}

// ── 1-Wire (bit-banged) + DS18B20 ────────────────────────────────────────────
// Timing per the DS18B20 datasheet; interrupts are masked only inside each
// ~70 µs bit slot so USB keeps running between bits.

bool ow_reset() {
    pinMode(kOneWirePin, OUTPUT);
    digitalWrite(kOneWirePin, LOW);
    delayMicroseconds(480);
    pinMode(kOneWirePin, INPUT_PULLUP);
    delayMicroseconds(70);
    const bool presence = (digitalRead(kOneWirePin) == LOW);
    delayMicroseconds(410);
    return presence;
}
void ow_write_bit(bool b) {
    noInterrupts();
    pinMode(kOneWirePin, OUTPUT);
    digitalWrite(kOneWirePin, LOW);
    delayMicroseconds(b ? 6 : 60);
    pinMode(kOneWirePin, INPUT_PULLUP);
    delayMicroseconds(b ? 64 : 10);
    interrupts();
}
bool ow_read_bit() {
    noInterrupts();
    pinMode(kOneWirePin, OUTPUT);
    digitalWrite(kOneWirePin, LOW);
    delayMicroseconds(3);
    pinMode(kOneWirePin, INPUT_PULLUP);
    delayMicroseconds(10);
    const bool b = digitalRead(kOneWirePin);
    delayMicroseconds(53);
    interrupts();
    return b;
}
void ow_write(uint8_t v) {
    for (int i = 0; i < 8; ++i) { ow_write_bit(v & 1); v >>= 1; }
}
uint8_t ow_read() {
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) { v >>= 1; if (ow_read_bit()) v |= 0x80; }
    return v;
}

// Dallas CRC-8 (poly 0x8C reflected) — validates ROM ids and scratchpads.
uint8_t ow_crc8(const uint8_t* d, int n) {
    uint8_t crc = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t b = d[i];
        for (int j = 0; j < 8; ++j) {
            const uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

// Maxim SEARCH ROM: enumerate every device on the bus (bit-by-bit binary
// tree walk). Returns the count stored into roms[][8], LSB-first per byte.
int ow_search_all(uint8_t roms[][8], int max_dev) {
    uint8_t rom[8] = {0};
    int  count     = 0;
    int  last_disc = 0;
    bool done      = false;
    while (!done && count < max_dev) {
        if (!ow_reset()) return count;
        ow_write(0xF0);                              // SEARCH ROM
        int disc_mark = 0;
        for (int bit = 1; bit <= 64; ++bit) {
            const bool b  = ow_read_bit();           // bit as seen on the bus
            const bool cb = ow_read_bit();           // complement
            bool dir;
            if (b && cb) return count;               // no device answered
            if (b != cb) {
                dir = b;                             // all remaining agree
            } else {                                 // discrepancy: both 0
                if (bit < last_disc)
                    dir = (rom[(bit - 1) >> 3] >> ((bit - 1) & 7)) & 1;
                else
                    dir = (bit == last_disc);        // take the 1-branch once
                if (!dir) disc_mark = bit;
            }
            if (dir) rom[(bit - 1) >> 3] |=  (1u << ((bit - 1) & 7));
            else     rom[(bit - 1) >> 3] &= ~(1u << ((bit - 1) & 7));
            ow_write_bit(dir);
        }
        last_disc = disc_mark;
        if (last_disc == 0) done = true;
        if (ow_crc8(rom, 7) == rom[7])               // keep only CRC-valid ids
            memcpy(roms[count++], rom, 8);
    }
    return count;
}

uint8_t  g_roms[kMaxOwDevices][8];
int      g_rom_count   = 0;
uint32_t g_next_search = 0;      // re-enumerate the bus occasionally
uint32_t g_temp_at     = 0;      // when the current phase may proceed
int      g_read_idx    = -1;     // -1 = idle, else next probe to read

void temp_service(uint32_t now) {
    if (static_cast<int32_t>(now - g_temp_at) < 0) return;

    if (g_read_idx < 0) {
        // Idle → (re)enumerate if due, then kick a convert-all on every probe.
        if (static_cast<int32_t>(now - g_next_search) >= 0 || g_rom_count == 0) {
            g_rom_count   = ow_search_all(g_roms, kMaxOwDevices);
            g_next_search = now + 60000;
        }
        if (g_rom_count == 0) { g_temp_at = now + 5000; return; }
        if (!ow_reset())      { g_temp_at = now + 5000; return; }
        ow_write(0xCC);                              // SKIP ROM
        ow_write(0x44);                              // CONVERT T (all probes)
        g_read_idx = 0;
        g_temp_at  = now + 800;                      // 12-bit conversion ≤ 750 ms
        return;
    }

    // One probe per pass (~7 ms of bit-banging) so buttons stay responsive.
    const uint8_t* rom = g_roms[g_read_idx];
    if (ow_reset()) {
        ow_write(0x55);                              // MATCH ROM
        for (int i = 0; i < 8; ++i) ow_write(rom[i]);
        ow_write(0xBE);                              // READ SCRATCHPAD
        uint8_t sp[9];
        for (int i = 0; i < 9; ++i) sp[i] = ow_read();
        if (ow_crc8(sp, 8) == sp[8]) {
            const int16_t raw   = static_cast<int16_t>((sp[1] << 8) | sp[0]);
            const long    milli = static_cast<long>(raw) * 1000 / 16;
            Serial.print("TEMP ");
            for (int i = 0; i < 8; ++i) {            // rom as 16 hex chars
                if (rom[i] < 16) Serial.print('0');
                Serial.print(rom[i], HEX);
            }
            Serial.print(' ');
            Serial.println(milli);
        }
    }
    if (++g_read_idx >= g_rom_count) {               // cycle done
        g_read_idx = -1;
        g_temp_at += (kTempPeriodMs > 800 ? kTempPeriodMs - 800 : 0);
    }
}

// ── Fans ─────────────────────────────────────────────────────────────────────
constexpr int kNumFans = sizeof(kFanPins) / sizeof(kFanPins[0]);

void fan_setup() {
    analogWriteFreq(25000);                          // above audible
    analogWriteRange(100);                           // duty in percent
    for (int i = 0; i < kNumFans; ++i) {
        pinMode(kFanPins[i], OUTPUT);
        analogWrite(kFanPins[i], 0);
    }
}

}  // namespace

void periph_setup() {
#ifndef VOICE_CHANGER
    // Without the voice build, nobody else has brought up I2C0 for the pads.
    Wire.setSDA(kDacSdaPin);
    Wire.setSCL(kDacSclPin);
    Wire.setClock(100000);
    Wire.begin();
#endif
    fan_setup();
}

void periph_service() {
    const uint32_t now = millis();
    mpr_service(now);
    temp_service(now);
}

bool periph_handle_command(const String& line) {
    if (!line.startsWith("FAN ")) return false;
    const int sp = line.indexOf(' ', 4);
    if (sp > 0) {
        const int zone = line.substring(4, sp).toInt();
        const int duty = constrain(line.substring(sp + 1).toInt(), 0, 100);
        if (zone >= 0 && zone < kNumFans) analogWrite(kFanPins[zone], duty);
    }
    return true;
}

#endif  // PERIPHERAL_HUB
