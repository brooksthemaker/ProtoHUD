// ── ProtoHUD button coprocessor — RP2350 / Raspberry Pi Pico 2 W ─────────────
// Debounces N physical switches, classifies short vs long press, and streams the
// events to the Pi over USB CDC. ProtoHUD's input::CoprocInputs (host side)
// dispatches them through the SAME input::GpioFunc path as on-board GPIO, so the
// coprocessor is never required — it just offloads debounce/long-press timing
// and frees the scarce GPIO that HUB75 leaves on the Pi.
//
// Protocol v1 (newline-delimited ASCII — see docs/coprocessor-input.md):
//   coproc → Pi : "HELLO proto-buttons v1 n=<N>"   (on connect)
//                 "BTN <id> SHORT" | "BTN <id> LONG"
//                 "BTN <id> DOWN"  | "BTN <id> UP"  (advisory; off by default)
//                 "PING"                            (heartbeat ~1 Hz)
//   Pi → coproc : "PONG"             (ack — ignored)
//                 "CFG long_ms=<n>"  (push the short/long threshold)
//                 "LED <id> <0|1>"   (drive a switch backlight, if wired)
//                 "PINS"             (live per-pin readout — role + level/mV,
//                                     "PIN <gp> <val> <roles>" per GP, PINS END)
//
// The firmware is "dumb about meaning": it reports button id + SHORT/LONG; the
// Pi decides what each id does (remappable in the HUD config). Bytes from the
// host are treated as untrusted: line length is bounded and unknown lines are
// ignored.

#include <Arduino.h>
#include <Wire.h>    // I2CSCAN bus test (core lib, no extra dependency)
#include <Servo.h>   // servo TEST channels (bundled with arduino-pico)
#include <Adafruit_NeoPixel.h>   // WS2812 TEST zone (see platformio.ini)
#include "config.h"
#ifdef VOICE_CHANGER
#include "voice.h"   // optional core1 voice changer (build with -DVOICE_CHANGER)
#endif
#ifdef MAX_BRIDGE
#include <SPI.h>     // optional MAX7219 USB→SPI bridge (build with -DMAX_BRIDGE)
#endif
#ifdef PERIPHERAL_HUB
#include "peripherals.h"  // boop pads + DS18B20 + fans (build with -DPERIPHERAL_HUB)
#endif

namespace {

inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#ifdef MAX_BRIDGE
constexpr size_t kMaxCs = sizeof(kMaxCsPins) / sizeof(kMaxCsPins[0]);

void max_bridge_setup() {
    SPI1.setSCK(kMaxSpiSck);
    SPI1.setTX(kMaxSpiTx);
    SPI1.begin();
    for (size_t i = 0; i < kMaxCs; ++i) {
        pinMode(kMaxCsPins[i], OUTPUT);
        digitalWrite(kMaxCsPins[i], HIGH);   // MAX7219 latches on CS rising edge
    }
}

// "SPI <cs> <hexbytes>" — shift the decoded bytes out SPI1 with kMaxCsPins[cs]
// held low, then latch. The bytes are already a full MAX7219 chain register
// write formatted by the host (src/face/max7219_chain.cpp), so we stay dumb.
void max_bridge_line(const String& line) {
    int sp = line.indexOf(' ', 4);
    if (sp < 0) return;
    const int cs = line.substring(4, sp).toInt();
    if (cs < 0 || cs >= static_cast<int>(kMaxCs)) return;
    const int hstart = sp + 1;
    const int hlen   = line.length() - hstart;
    if (hlen < 2) return;
    const uint8_t pin = kMaxCsPins[cs];
    SPI1.beginTransaction(SPISettings(kMaxSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(pin, LOW);
    for (int i = 0; i + 1 < hlen; i += 2) {
        const int hi = hexval(line[hstart + i]);
        const int lo = hexval(line[hstart + i + 1]);
        if (hi < 0 || lo < 0) break;
        SPI1.transfer(static_cast<uint8_t>((hi << 4) | lo));
    }
    digitalWrite(pin, HIGH);      // latch into the chips
    SPI1.endTransaction();
}
#endif  // MAX_BRIDGE

// Tunable at runtime via "CFG long_ms=<n>"; starts at the configured default.
uint32_t g_long_ms = kLongMsInit;

// ── Runtime pin map ──────────────────────────────────────────────────────────
// Pin ROLES start from config.h's defaults but can be redefined live by the Pi
// over "PINCFG …" (see docs/coprocessor-input.md) — which GPIO is a button, its
// pull bias / polarity, and its backlight LED can all change with NO reflash.
// The firmware stays "dumb about pins": it applies whatever map it is handed (or
// the compiled defaults if the Pi never pushes one).
constexpr size_t kMaxButtons = 16;

struct PinCfg {
    uint8_t gp         = 0;
    uint8_t pull       = 0;      // 0 = up (INPUT_PULLUP), 1 = down, 2 = none
    bool    active_low = true;   // pressed reads LOW (true) or HIGH (false)
    int8_t  led        = -1;     // optional backlight GPIO, -1 = none
};
PinCfg   g_pins[kMaxButtons];
size_t   g_npins = 0;

// Per-button debounce + press state (true = released throughout).
struct Button {
    bool     raw        = true;
    bool     stable     = true;
    uint32_t edge_ms    = 0;      // time of last raw change (debounce timer)
    uint32_t down_ms    = 0;      // when the debounced press began
    bool     long_fired = false;  // LONG already emitted for the current hold
};
Button   g_btn[kMaxButtons];

uint32_t g_last_ping = 0;
bool     g_was_connected = false;   // tracks the USB CDC (DTR) edge for re-HELLO

int split_ws(const String& s, String* out, int maxn);   // defined below

// ── Peripheral TEST state (pre-assigned pins — see config.h) ─────────────────
// TTP223 touch pads: plain GPIO, always compiled. Debounced edges stream up as
// "BOOP <idx> <1|0>" so the Pi-side zone/function mapping is transport-agnostic.
struct Touch {
    bool stable = false;      // debounced touched state
    bool raw    = false;
    uint32_t edge_ms = 0;
};
Touch g_touch[6];

// ── Servos ───────────────────────────────────────────────────────────────────
// Two backends behind one channel-addressed API. A PCA9685 (16 hardware PWM
// channels, own servo power rail) is used whenever one answers on I2C; if none
// is fitted the four direct-GPIO channels on kServoPins take over. The Pi only
// ever addresses CHANNEL NUMBERS, so nothing above this layer changes when the
// board is fitted or removed.
constexpr int kServoMax  = kServoChannels;   // logical channels (PCA9685: 16)
constexpr int kServoGpio = 4;                // direct-pin fallback channels

Servo    g_servo[kServoGpio];                 // fallback backend only
bool     g_servo_on[kServoMax]  = {};
// Smooth motion: SERVOM sets a target + slew speed; servo_service() eases the
// current angle toward the target at speed deg/s each tick so ears move
// naturally instead of snapping. speed 0 = instant (plain SERVO behaviour).
float    g_servo_cur[kServoMax];              // current (smoothed) angle
float    g_servo_tgt[kServoMax];              // commanded target angle
float    g_servo_spd[kServoMax] = {};         // slew speed deg/s (0 = snap)
uint32_t g_servo_tick = 0;                    // last servo_service tick (millis)
// Pulse width range per channel, microseconds — this is what sets how much TRAVEL
// a servo actually has, since 0-180 deg maps linearly onto it. 500-2500 is the
// usual full range for a hobby servo; the Pi retunes per channel with SERVOCAL.
// MEASURED (2026-07-31) on the direct-GPIO backend: the generated pulse tracks
// this window to within 1 us across 0/45/90/135/180 deg, so short travel means
// the window is too narrow for the servo (or it lacks the torque to get there),
// never a mapping error. The PCA9685 path uses the same numbers.
int16_t  g_servo_min_us[kServoMax];
int16_t  g_servo_max_us[kServoMax];
// Is the channel really being driven? On the GPIO fallback, Servo::attach()
// allocates a PIO state machine and returns -1 if none is free, after which
// write() SILENTLY DROPS every angle — so without this a channel looks live
// while the horn never moves. On the PCA9685 there is nothing to claim, so the
// equivalent question is whether the I2C write was acknowledged.
// -1 = never tried, 0 = failed, 1 = driving.
int8_t   g_servo_att[kServoMax];
bool     g_pca_ok = false;                    // a PCA9685 answered on I2C

// ── PCA9685 register driver ──────────────────────────────────────────────────
// Deliberately not a library dependency: the pulse-width math is the part that
// matters here (a whole debugging session went into proving pulse widths), so it
// stays visible and measurable rather than buried behind someone else's API.
constexpr uint8_t kPcaMode1    = 0x00;
constexpr uint8_t kPcaPrescale = 0xFE;
constexpr uint8_t kPcaLed0     = 0x06;   // 4 bytes/channel: ON_L, ON_H, OFF_L, OFF_H

bool pca_write8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(kPcaAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

int pca_read8(uint8_t reg) {
    Wire.beginTransmission(kPcaAddr);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return -1;
    if (Wire.requestFrom((uint8_t)kPcaAddr, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

bool pca_responds() {
    Wire.beginTransmission(kPcaAddr);
    return Wire.endTransmission() == 0;
}

// Set the PWM frame rate. PRESCALE is only writable while the oscillator is
// asleep, so this parks it, writes, wakes, and waits the datasheet's 500 us
// before setting RESTART + auto-increment.
bool pca_set_freq(uint16_t hz) {
    float pre_f = 25000000.0f / (4096.0f * (float)hz) - 1.0f;
    int   pre   = (int)lroundf(pre_f);
    pre = constrain(pre, 3, 255);
    const int m1 = pca_read8(kPcaMode1);
    if (m1 < 0) return false;
    const uint8_t wake  = (uint8_t)(m1 & ~0x10);
    const uint8_t sleep = (uint8_t)(wake | 0x10);
    if (!pca_write8(kPcaMode1, sleep))            return false;
    if (!pca_write8(kPcaPrescale, (uint8_t)pre))  return false;
    if (!pca_write8(kPcaMode1, wake))             return false;
    delayMicroseconds(500);
    return pca_write8(kPcaMode1, (uint8_t)(wake | 0xA0));  // RESTART | AI
}

// Drive one channel to a pulse width. The frame is 4096 counts, so
// counts = us * 4096 * freq / 1e6 (at 50 Hz: us * 0.2048).
bool pca_set_us(int ch, int us) {
    const int counts = (int)lroundf((float)us * 4096.0f *
                                    (float)kPcaFreqHz / 1000000.0f);
    const uint16_t off = (uint16_t)constrain(counts, 0, 4095);
    Wire.beginTransmission(kPcaAddr);
    Wire.write((uint8_t)(kPcaLed0 + 4 * ch));
    Wire.write(0); Wire.write(0);                    // ON = 0 (rise at frame start)
    Wire.write((uint8_t)(off & 0xFF));
    Wire.write((uint8_t)(off >> 8));
    return Wire.endTransmission() == 0;
}

// Stop driving a channel so the servo goes limp — the FULL_OFF bit, not a 0 %
// duty (which would still be a valid frame the servo holds against).
bool pca_set_off(int ch) {
    Wire.beginTransmission(kPcaAddr);
    Wire.write((uint8_t)(kPcaLed0 + 4 * ch));
    Wire.write(0); Wire.write(0);
    Wire.write(0); Wire.write(0x10);                 // OFF_H bit4 = full off
    return Wire.endTransmission() == 0;
}

// Probe for the board and configure it. Safe to call repeatedly — used at boot
// and from SERVOBUS, so fitting the board doesn't need a reflash or a reboot.
bool pca_begin() {
    Wire.setSDA(kPcaSdaPin);
    Wire.setSCL(kPcaSclPin);
    Wire.setClock(400000);      // PCA9685 is good to 1 MHz; 400 kHz is ample
    Wire.begin();
    if (!pca_responds()) { g_pca_ok = false; return false; }
    g_pca_ok = pca_set_freq(kPcaFreqHz);
    return g_pca_ok;
}

// The channel's calibrated pulse width for an angle. One place, both backends.
int servo_us_for(int ch, float deg) {
    const float t = constrain(deg, 0.f, 180.f) / 180.f;
    return (int)lroundf((float)g_servo_min_us[ch] +
                        t * (float)(g_servo_max_us[ch] - g_servo_min_us[ch]));
}

// Start driving a channel. On the PCA9685 there is nothing to claim, so this
// only fails if the board stops acknowledging; on the GPIO fallback it really
// can fail (no free PIO state machine), and that failure must never be silent.
bool servo_attach(int ch) {
    if (g_pca_ok) {
        const bool ok = pca_set_us(ch, servo_us_for(ch, g_servo_cur[ch]));
        g_servo_att[ch] = ok ? 1 : 0;
        if (!ok) {
            Serial.print("SERVOERR "); Serial.print(ch);
            Serial.println(" PCA9685 did not acknowledge (I2C)");
        }
        return ok;
    }
    if (ch >= kServoGpio || kServoPins[ch] < 0) {
        g_servo_att[ch] = 0;
        Serial.print("SERVOERR "); Serial.print(ch);
        Serial.println(" no PCA9685 fitted and no direct pin for this channel");
        return false;
    }
    const int rc = g_servo[ch].attach(kServoPins[ch],
                                      g_servo_min_us[ch], g_servo_max_us[ch]);
    g_servo_att[ch] = (rc < 0) ? 0 : 1;
    if (rc < 0) {
        Serial.print("SERVOERR "); Serial.print(ch);
        Serial.print(" gp="); Serial.print(kServoPins[ch]);
        Serial.println(" attach failed (no free PIO state machine)");
    }
    return rc >= 0;
}

// Write an angle out through whichever backend is live.
void servo_out_deg(int ch, float deg) {
    if (g_pca_ok) { pca_set_us(ch, servo_us_for(ch, deg)); return; }
    if (ch < kServoGpio) g_servo[ch].write((int)lroundf(deg));
}

// Release a channel (limp).
void servo_out_off(int ch) {
    if (g_pca_ok) { pca_set_off(ch); return; }
    if (ch < kServoGpio && g_servo[ch].attached()) g_servo[ch].detach();
}

void servo_setup() {
    for (int ch = 0; ch < kServoMax; ++ch) {
        g_servo_cur[ch]    = 90.f;
        g_servo_tgt[ch]    = 90.f;
        g_servo_min_us[ch] = 500;
        g_servo_max_us[ch] = 2500;
        g_servo_att[ch]    = -1;
    }
    pca_begin();     // quiet if absent; the GPIO fallback covers channels 0-3
}

// ── Digital addressable LED zone (WS2812/NeoPixel or APA102/DotStar) ─────────
// One frame buffer + two output paths. WS2812 goes through Adafruit_NeoPixel
// (PIO timing); APA102 is bit-banged — its protocol is clocked, so bit-banging
// is timing-free and needs no library. Patterns animate locally (~30 fps) so
// the USB link stays idle; LEDF/LEDSHOW lets the Pi stream arbitrary frames
// for custom panels instead.
Adafruit_NeoPixel* g_ledz = nullptr;          // WS2812 backend (lazy)
uint8_t  g_led_px[kLedZoneMax][3];            // frame buffer, RGB
uint16_t g_led_n      = kLedZoneCount;
uint8_t  g_led_bright = 255;                  // software brightness 0-255
uint8_t  g_led_mode   = 0;                    // 0 off, 1 solid, 2 rainbow, 3 chase, 4 breathe
uint8_t  g_led_r = 255, g_led_g = 255, g_led_b = 255;
uint8_t  g_led_speed  = 50;                   // pattern speed 1-255
uint32_t g_led_last   = 0;                    // last pattern tick
float    g_led_phase  = 0.f;
bool     g_led_dirty  = false;                // LEDF wrote pixels; LEDSHOW latches

// ── Per-zone autonomous animation (coproc_local transport) ───────────────────
// The Pi sends high-level per-zone descriptors (LZONE/LZP/LZG/LZF/LVOL) and this
// MCU runs the animation off its own clock, mirroring the host renderer's math:
// Solid/Breathe/Chase/Sparkle/Gradient/Wave/Level, linked hub+fin areas, the
// sound-trigger gate + complete-on-trigger pulses, and the flash overlay. Live
// mic volume arrives ~30 Hz as LVOL; only follow-face colour is still deferred.
// Coexists with the legacy whole-chain path (LEDZ/LEDP + LEDF streaming): the
// first LZ* command flips g_zone_active on and the compositor takes over; a
// legacy LEDZ/LEDP/LEDF flips it back off.
constexpr int kLedZones     = 5;               // mirrors accessory::ZoneCount
constexpr int kLedMaxPulses = 16;              // live complete-on-trigger sweeps per zone
constexpr int kLedMaxStops  = 8;               // multi-stop gradient stops per zone (mirrors the host)
struct LedZoneState {
    uint16_t start = 0, count = 0;             // slice on the shared chain
    uint8_t  pattern = 0;                      // mirrors accessory::Pattern (0..7)
    uint8_t  r = 0, g = 0, b = 0;              // primary colour
    uint8_t  r2 = 0, g2 = 0, b2 = 0;           // legacy gradient endpoint (unused; kept for LZP compat)
    uint8_t  nstops = 0;                       // multi-stop gradient: 0 = none, else 2..kLedMaxStops
    uint8_t  stops[8][3] = {};                 // stop colours, base -> tip
    int32_t  breathe_mhz = 500;                // Breathe/Chase rate, milli-Hz
    int32_t  wave_mhz    = 500;                // Gradient scroll / Wave travel, milli-Hz (signed)
    uint8_t  zbright   = 255;                   // per-zone brightness
    uint8_t  min_level = 0;                     // idle floor 0..255
    uint8_t  linked = 0;                        // hub+fin passthrough (no wrap) for Chase/Wave/Gradient
    uint8_t  level_style = 0;                   // Level reaction 0..4 (glow/meter/center/peak/pulse)
    uint8_t  sound_trigger = 0;                 // gate the pattern on the mic
    uint8_t  sound_complete = 0;                // each trigger emits a full travelling pulse
    uint8_t  sound_thresh = 77;                 // mic gate open level, 0..255
    int32_t  sound_decay_mhz = 2000;            // gate fall rate, milli per second
    int32_t  pal_drift_mhz   = 0;               // palette scroll, milli-cycles/s
    // Overlay layer: a colour composited OVER this zone's pattern, covering part
    // of it while the rest shows through. Params arrive on LZOV; the two ANIMATED
    // scalars (amount/phase) stream on LZOA at the host's command rate, the same
    // shape as the LVOL mic feed. Timing lives on the host so there is only one
    // state machine — see src/accessory/led_overlay.h.
    uint8_t  ov_on = 0;
    uint8_t  ov_r = 0, ov_g = 0, ov_b = 0;
    uint8_t  ov_shape = 0;                      // 0 Rise, 1 Sweep, 2 Bloom
    uint8_t  ov_opacity = 0;                    // 0..255
    uint8_t  ov_soft = 46;                      // 0..255 (~18%)
    uint8_t  ov_amount = 0;                     // 0..255 animation progress
    uint8_t  ov_phase = 0;                      // 0..255 travelling position
    uint32_t flash_start = 0, flash_end = 0;    // white flash overlay window (millis)
    // runtime (compositor-owned)
    float    sound_env = 0.f;                   // gate envelope 0..1
    uint8_t  prev_above = 0;                    // rising-edge detect
    uint8_t  npulses = 0;                       // live pulses
    float    pulses[kLedMaxPulses] = {};        // pulse head positions 0..1
};
LedZoneState g_lz[kLedZones];
uint8_t  g_lz_frac[kLedZoneMax] = {};          // per-pixel length fraction 0..255 (Level/Gradient/Wave/linked)
// Per-pixel fraction along the OVERLAY's own axis. A separate table because the
// overlay has its own angle — a blush rises vertically while the zone's gradient
// runs along the appendage — so it cannot share g_lz_frac.
uint8_t  g_lz_ovfrac[kLedZoneMax] = {};
bool     g_zone_active = false;                // compositor owns the strip
bool     g_zone_sync   = false;                // side zones (0..3) share a phase origin
uint32_t g_zone_t0     = 0;                    // shared phase origin (millis)
uint32_t g_zone_tick   = 0;                    // compositor throttle / last-tick millis
float    g_zone_vol    = 0.f;                  // live mic volume 0..1 (LVOL)
float    g_zone_peak   = 0.f;                  // held mic peak 0..1 (LVOL), for Level "Peak"

void led_show() {
    if (kLedZonePin < 0) return;
    if (kLedZoneType == 0) {
        if (!g_ledz || g_ledz->numPixels() != g_led_n) {
            delete g_ledz;
            g_ledz = new Adafruit_NeoPixel(g_led_n, kLedZonePin, NEO_GRB + NEO_KHZ800);
            g_ledz->begin();
        }
        for (uint16_t i = 0; i < g_led_n; ++i)
            g_ledz->setPixelColor(i, g_ledz->Color(
                (uint16_t)g_led_px[i][0] * g_led_bright / 255,
                (uint16_t)g_led_px[i][1] * g_led_bright / 255,
                (uint16_t)g_led_px[i][2] * g_led_bright / 255));
        g_ledz->show();
    } else {
        // APA102: 32-bit start frame, per-LED 0xE0|global(5b) B G R, end clocks.
        static bool init = false;
        if (!init) {
            pinMode(kLedZonePin, OUTPUT);
            pinMode(kLedZoneClkPin, OUTPUT);
            digitalWrite(kLedZoneClkPin, LOW);
            init = true;
        }
        auto out = [](uint8_t byte) {
            for (int bit = 7; bit >= 0; --bit) {
                digitalWrite(kLedZonePin, (byte >> bit) & 1);
                digitalWrite(kLedZoneClkPin, HIGH);
                digitalWrite(kLedZoneClkPin, LOW);
            }
        };
        for (int i = 0; i < 4; ++i) out(0x00);                    // start frame
        const uint8_t g5 = 0xE0 | (uint8_t)((g_led_bright >> 3) & 0x1F);
        for (uint16_t i = 0; i < g_led_n; ++i) {
            out(g5);
            out(g_led_px[i][2]);   // B
            out(g_led_px[i][1]);   // G
            out(g_led_px[i][0]);   // R
        }
        for (uint16_t i = 0; i < (g_led_n + 15) / 16 + 1; ++i) out(0x00);  // end
    }
}

void led_fill(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < g_led_n; ++i) {
        g_led_px[i][0] = r; g_led_px[i][1] = g; g_led_px[i][2] = b;
    }
}

// Local pattern animation (~30 fps), driven from loop(). Runs standalone —
// the Pi only sends mode changes.
void led_service(uint32_t now) {
    if (g_zone_active) return;                 // per-zone engine owns the strip
    if (kLedZonePin < 0 || g_led_mode < 2) return;
    if (now - g_led_last < 33) return;
    g_led_last = now;
    g_led_phase += g_led_speed / 255.f * 0.25f;
    if (g_led_phase >= 1.f) g_led_phase -= 1.f;
    if (g_led_mode == 2) {                                        // rainbow
        for (uint16_t i = 0; i < g_led_n; ++i) {
            const float h = g_led_phase + (float)i / g_led_n;
            const float hh = (h - (int)h) * 6.f;
            const int   sec = (int)hh;
            const uint8_t f = (uint8_t)((hh - sec) * 255);
            uint8_t r = 0, g = 0, b = 0;
            switch (sec % 6) {
                case 0: r = 255;     g = f;       b = 0;       break;
                case 1: r = 255 - f; g = 255;     b = 0;       break;
                case 2: r = 0;       g = 255;     b = f;       break;
                case 3: r = 0;       g = 255 - f; b = 255;     break;
                case 4: r = f;       g = 0;       b = 255;     break;
                default: r = 255;    g = 0;       b = 255 - f; break;
            }
            g_led_px[i][0] = r; g_led_px[i][1] = g; g_led_px[i][2] = b;
        }
    } else if (g_led_mode == 3) {                                 // chase
        led_fill(g_led_r / 10, g_led_g / 10, g_led_b / 10);
        const int head = (int)(g_led_phase * g_led_n);
        for (int t = 0; t < 4; ++t) {
            const int i = (head - t + g_led_n) % g_led_n;
            const uint8_t k = 255 - t * 60;
            g_led_px[i][0] = (uint16_t)g_led_r * k / 255;
            g_led_px[i][1] = (uint16_t)g_led_g * k / 255;
            g_led_px[i][2] = (uint16_t)g_led_b * k / 255;
        }
    } else if (g_led_mode == 4) {                                 // breathe
        const float t = g_led_phase < 0.5f ? g_led_phase * 2.f
                                           : (1.f - g_led_phase) * 2.f;
        const uint8_t k = (uint8_t)(20 + t * 235);
        led_fill((uint16_t)g_led_r * k / 255, (uint16_t)g_led_g * k / 255,
                 (uint16_t)g_led_b * k / 255);
    }
    led_show();
}

// Composite every live zone into g_led_px and latch, once per tick (~60 fps).
// Called from loop(); a no-op until the first LZ* command arrives.
// Sample a zone's multi-stop gradient at length fraction f (0..1). Mirrors the
// host's sample_stops() in accessory_leds.cpp so the Pico and the CM5 preview
// resolve the same colour for the same stops — keep the two in step.
static void lz_sample_stops(const LedZoneState& z, float f,
                            float& r, float& g, float& b) {
    const int n = z.nstops;
    if (n <= 0) return;                        // caller's flat colour stands
    if (n == 1) { r = z.stops[0][0]; g = z.stops[0][1]; b = z.stops[0][2]; return; }
    f = fminf(fmaxf(f, 0.f), 1.f);
    const float x  = f * (n - 1);
    const int   i0 = (int)floorf(x);
    const int   i1 = (i0 + 1 < n) ? i0 + 1 : n - 1;
    const float fr = x - i0;
    r = z.stops[i0][0] * (1.f - fr) + z.stops[i1][0] * fr;
    g = z.stops[i0][1] * (1.f - fr) + z.stops[i1][1] * fr;
    b = z.stops[i0][2] * (1.f - fr) + z.stops[i1][2] * fr;
}

void led_zone_render(uint32_t now) {
    if (!g_zone_active || kLedZonePin < 0) return;
    if (now - g_zone_tick < 16) return;                 // ~60 fps
    float dt = (g_zone_tick == 0) ? 0.016f : (now - g_zone_tick) / 1000.f;
    if (dt > 0.1f) dt = 0.1f;                            // clamp after a stall
    g_zone_tick = now;

    const float vol  = g_zone_vol;
    const float peak = g_zone_peak;

    for (uint16_t i = 0; i < g_led_n; ++i) { g_led_px[i][0] = g_led_px[i][1] = g_led_px[i][2] = 0; }

    for (int zi = 0; zi < kLedZones; ++zi) {
        LedZoneState& z = g_lz[zi];
        if (z.count == 0) continue;
        // Side zones (0..3) optionally share a phase origin so their time-based
        // effects start aligned; Blush (zi 4) is always free-running.
        const uint32_t base = (g_zone_sync && zi < 4) ? g_zone_t0 : 0u;
        const float t          = (now - base) / 1000.f;
        const float breathe_hz = z.breathe_mhz / 1000.f;
        const float wave_hz    = z.wave_mhz    / 1000.f;
        const float zbf        = z.zbright  / 255.f;
        const float floor_lvl  = z.min_level / 255.f;

        // Sound trigger: gate the pattern on the mic, or (complete mode) emit a
        // travelling pulse on each rising edge that finishes its own sweep.
        float sgate = 1.f;
        bool  use_pulses = false;
        if (z.sound_trigger) {
            const float thr   = z.sound_thresh / 255.f;
            const bool  above = (vol >= thr);
            const bool  rising = above && !z.prev_above;
            if (z.sound_complete) {
                float sp = wave_hz; if (fabsf(sp) < 0.05f) sp = 1.f;
                const float step = sp * dt;
                uint8_t w = 0;                            // advance + prune live pulses
                for (uint8_t k = 0; k < z.npulses; ++k) {
                    const float ph = z.pulses[k] + step;
                    if (ph <= 1.2f && ph >= -0.2f) z.pulses[w++] = ph;
                }
                z.npulses = w;
                if (rising && z.npulses < kLedMaxPulses) z.pulses[z.npulses++] = (sp >= 0.f ? 0.f : 1.f);
                use_pulses = true;
            } else {
                if (above) z.sound_env = 1.f;
                else       z.sound_env = fmaxf(0.f, z.sound_env - (z.sound_decay_mhz / 1000.f) * dt);
                sgate = z.sound_env;
                z.npulses = 0;
            }
            z.prev_above = above ? 1 : 0;
        } else {
            z.sound_env = 0.f; z.prev_above = 0; z.npulses = 0;
        }

        if (z.pattern != 0) {                           // 0 = Off → slice stays dark
            float env = 1.f;
            if (z.pattern == 2)                          // Breathe
                env = 0.5f * (1.f - cosf(2.f * (float)PI * breathe_hz * t));
            const float edge = 1.f / fmaxf(1.f, (float)z.count);   // ~1-LED soft edge (Level)
            for (uint16_t i = 0; i < z.count; ++i) {
                const uint16_t o = z.start + i;
                const float lf = g_lz_frac[o] / 255.f;
                float px = 1.f;
                float cr = z.r, cg = z.g, cb = z.b;
                // The multi-stop gradient sets the BASE colour of every pattern
                // (host parity); Gradient scrolls the stops in its own case.
                // Palette drift scrolls WHICH colour each LED takes, independently
                // of the pattern — so Breathe/Solid show flowing colours instead of
                // a frozen ramp. Minus matches the host and the Wave/Chase/pulse
                // travel convention. KEEP IN STEP with accessory_leds.cpp.
                float pal_f = lf;
                if (z.pal_drift_mhz != 0) {
                    pal_f = lf - (z.pal_drift_mhz / 1000.f) * t;
                    pal_f -= floorf(pal_f);
                }
                if (z.nstops >= 2 && z.pattern != 6) lz_sample_stops(z, pal_f, cr, cg, cb);

                // Complete-on-trigger: overlapping pulses REPLACE the base
                // envelope; brightness is the strongest travelling band.
                if (use_pulses) {
                    if (z.pattern == 6 && z.nstops >= 2)  // gradient colours the band
                        lz_sample_stops(z, lf, cr, cg, cb);
                    float best = 0.f;
                    for (uint8_t k = 0; k < z.npulses; ++k) {
                        const float d = fabsf(lf - z.pulses[k]);
                        const float bb = fmaxf(0.f, 1.f - d / 0.18f);
                        best = fmaxf(best, bb * bb);
                    }
                    const float e = (floor_lvl + (1.f - floor_lvl) * best) * zbf;
                    g_led_px[o][0] = (uint8_t)(cr * e);
                    g_led_px[o][1] = (uint8_t)(cg * e);
                    g_led_px[o][2] = (uint8_t)(cb * e);
                    continue;
                }

                switch (z.pattern) {
                case 4: {                                // Chase: dot + fading tail
                    if (z.linked) {                      // walks the pair's fraction 0→1
                        float head = fmodf(breathe_hz * t, 1.f); if (head < 0) head += 1.f;
                        float d = head - lf; if (d < 0) d += 1.f;
                        px = fmaxf(0.f, 1.f - d / 0.35f); px *= px;
                    } else {
                        float head = fmodf(breathe_hz * t, 1.f); if (head < 0) head += 1.f;
                        head *= z.count;
                        float d = head - i; if (d < 0) d += z.count;
                        const float tail = fmaxf(3.f, z.count * 0.5f);
                        px = fmaxf(0.f, 1.f - d / tail); px *= px;
                    }
                    break;
                }
                case 5: {                                // Sparkle: per-pixel twinkle
                    const uint32_t h = ((uint32_t)o * 2654435761u) ^ 0x9E3779B9u;
                    const float rate  = 0.5f + (h & 0xFF) / 96.f;
                    const float phase = ((h >> 8) & 0xFFFF) / 65536.f;
                    const float v = 0.5f * (1.f - cosf(2.f * (float)PI * (rate * t + phase)));
                    px = v * v * v; break;
                }
                case 6: {                                // Gradient: scroll the stops
                    float m = lf;
                    if (wave_hz != 0.f) {
                        // MINUS: matches the host (accessory_leds.cpp) — a positive
                        // speed carries the gradient the same way the Wave band,
                        // Chase dot and sound pulses travel. KEEP THE TWO IN STEP.
                        float f = lf - wave_hz * t; f -= floorf(f);
                        m = z.linked ? f : (1.f - fabsf(2.f * f - 1.f));   // passthrough vs ping-pong
                    }
                    // <2 stops = a flat Color (the Color 2 ramp is gone).
                    if (z.nstops >= 2) lz_sample_stops(z, m, cr, cg, cb);
                    break;
                }
                case 7: {                                // Wave: bright band travels the length
                    float head = fmodf(wave_hz * t, 1.f); if (head < 0) head += 1.f;
                    float d = fabsf(lf - head);
                    if (!z.linked) d = fminf(d, 1.f - d);   // standalone wraps; linked passes through
                    px = fmaxf(0.f, 1.f - d / 0.18f); px *= px; break;
                }
                case 3: {                                // Level: mic-reactive (LVOL feed)
                    const float L = vol;
                    switch (z.level_style) {
                    case 1: px = fminf(fmaxf((L - lf) / edge + 0.5f, 0.f), 1.f); break;                 // Meter
                    case 2: { const float d = fabsf(lf - 0.5f) * 2.f;
                              px = fminf(fmaxf((L - d) / edge + 0.5f, 0.f), 1.f); break; }              // CenterMeter
                    case 3: { const float fill = fminf(fmaxf((L - lf) / edge + 0.5f, 0.f), 1.f);
                              const float mk = (fabsf(lf - peak) <= 1.5f * edge) ? 1.f : 0.f;
                              px = fmaxf(fill * 0.65f, mk); break; }                                    // Peak
                    case 4: { const float d = fabsf(lf - 0.5f) * 2.f;
                              px = fminf(fmaxf(L * 1.5f - d * (1.f - L), 0.f), 1.f); break; }           // Pulse
                    default: px = L; break;                                                            // Glow
                    }
                    break;
                }
                default: break;                          // Solid: px = 1
                }
                const float m = env * px * sgate;
                const float e = (floor_lvl + (1.f - floor_lvl) * m) * zbf;
                g_led_px[o][0] = (uint8_t)(cr * e);
                g_led_px[o][1] = (uint8_t)(cg * e);
                g_led_px[o][2] = (uint8_t)(cb * e);
            }
        }

        // Overlay layer — a colour covering PART of the zone with the pattern
        // showing through underneath. Outside the `pattern != 0` guard above, so
        // it also shows on an Off zone (a blush on a dark cheek).
        // ⚠️ MIRRORS accessory_leds.cpp overlay_cover()/blend_overlay() EXACTLY.
        // KEEP THE TWO IN STEP — the host preview and the hardware diverge
        // silently otherwise.
        if (z.ov_on && z.ov_amount > 0 && z.ov_opacity > 0) {
            const float a    = z.ov_amount / 255.f;
            const float soft = fmaxf(0.02f, z.ov_soft / 255.f);
            const float op   = z.ov_opacity / 255.f;
            const float ph   = z.ov_phase / 255.f;
            for (uint16_t i = 0; i < z.count; ++i) {
                const uint16_t o = z.start + i;
                if (o >= kLedZoneMax) break;
                const float f = g_lz_ovfrac[o] / 255.f;
                float c;
                switch (z.ov_shape) {
                case 1: {                                  // Sweep: travelling band
                    const float d = fabsf(f - ph);
                    c = fmaxf(0.f, 1.f - d / soft) * a;
                    break;
                }
                case 2: {                                  // Bloom: grows from centre
                    const float d = fabsf(f - 0.5f) * 2.f;
                    c = (a * (1.f + soft) - d) / soft;
                    break;
                }
                default:                                   // Rise: fills along axis
                    c = (a * (1.f + soft) - f) / soft;
                    break;
                }
                c = fminf(fmaxf(c, 0.f), 1.f) * op;
                if (c <= 0.f) continue;
                const float inv = 1.f - c;
                g_led_px[o][0] = (uint8_t)(g_led_px[o][0] * inv + z.ov_r * c);
                g_led_px[o][1] = (uint8_t)(g_led_px[o][1] * inv + z.ov_g * c);
                g_led_px[o][2] = (uint8_t)(g_led_px[o][2] * inv + z.ov_b * c);
            }
        }

        // White flash overlay — survives whatever base pattern is on, fades out.
        if (z.flash_end > now && z.flash_end > z.flash_start) {
            float fl = (float)(z.flash_end - now) / (float)(z.flash_end - z.flash_start);
            if (fl > 1.f) fl = 1.f;
            const float inv = 1.f - fl;
            for (uint16_t i = 0; i < z.count; ++i) {
                const uint16_t o = z.start + i;
                for (int c = 0; c < 3; ++c)
                    g_led_px[o][c] = (uint8_t)(g_led_px[o][c] * inv + 255.f * fl);
            }
        }
    }
    led_show();
}

// Parse a 6-char "RRGGBB" hex colour. Leaves the outputs untouched on bad input.
bool parse_hex_rgb(const String& s, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (s.length() < 6) return false;
    int v[6];
    for (int k = 0; k < 6; ++k) { v[k] = hexval(s[k]); if (v[k] < 0) return false; }
    r = (uint8_t)((v[0] << 4) | v[1]);
    g = (uint8_t)((v[2] << 4) | v[3]);
    b = (uint8_t)((v[4] << 4) | v[5]);
    return true;
}

// "LZONE <zi> <start> <count>" — place/resize zone zi on the shared chain
// (count 0 disables it). Grows g_led_n so led_show() covers the whole chain.
void lzone_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[4];
    if (split_ws(line, t, 4) < 4) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    g_lz[zi].start = (uint16_t)constrain(t[2].toInt(), 0, (int)kLedZoneMax - 1);
    g_lz[zi].count = (uint16_t)constrain(t[3].toInt(), 0, (int)kLedZoneMax);
    const int end = g_lz[zi].start + g_lz[zi].count;
    if (end > (int)g_led_n) g_led_n = (uint16_t)constrain(end, 1, (int)kLedZoneMax);
    g_zone_active = true;
}

// "LZP <zi> <pattern> <RRGGBB> <RRGGBB2> <breathe_mHz> <wave_mHz> <zbright>
//      <minlevel> [flags] [level_style] [sound_thresh] [sound_decay_mHz]"
// — the zone's look. This is the "change pattern / colours" command. The tail is
// optional (older senders stop at minlevel). flags: bit0 linked, bit1
// sound_trigger, bit2 sound_complete.
void lzp_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[14];
    const int nt = split_ws(line, t, 14);
    if (nt < 9) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    LedZoneState& z = g_lz[zi];
    z.pattern = (uint8_t)constrain(t[2].toInt(), 0, 7);
    parse_hex_rgb(t[3], z.r,  z.g,  z.b);
    parse_hex_rgb(t[4], z.r2, z.g2, z.b2);
    z.breathe_mhz = t[5].toInt();
    z.wave_mhz    = t[6].toInt();
    z.zbright     = (uint8_t)constrain(t[7].toInt(), 0, 255);
    z.min_level   = (uint8_t)constrain(t[8].toInt(), 0, 255);
    const uint8_t flags = (nt >= 10) ? (uint8_t)t[9].toInt() : 0;
    z.linked          = (flags & 0x01) ? 1 : 0;
    z.sound_trigger   = (flags & 0x02) ? 1 : 0;
    z.sound_complete  = (flags & 0x04) ? 1 : 0;
    z.level_style     = (nt >= 11) ? (uint8_t)constrain(t[10].toInt(), 0, 4) : 0;
    z.sound_thresh    = (nt >= 12) ? (uint8_t)constrain(t[11].toInt(), 0, 255) : 77;
    z.sound_decay_mhz = (nt >= 13) ? t[12].toInt() : 2000;
    // Palette drift (milli-cycles/s). Optional tail: an older host that stops
    // short leaves it at 0, i.e. a static palette exactly as before.
    z.pal_drift_mhz   = (nt >= 14) ? t[13].toInt() : 0;
    if (g_zone_sync && zi < 4) g_zone_t0 = millis();     // realign side zones on change
    g_zone_active = true;
}

// "LVOL <vol0-255> [peak0-255]" — live mic level for Level + the sound gate.
void lvol_line(const String& line) {
    String t[3];
    const int nt = split_ws(line, t, 3);
    if (nt < 2) return;
    g_zone_vol  = constrain(t[1].toInt(), 0, 255) / 255.f;
    g_zone_peak = (nt >= 3) ? constrain(t[2].toInt(), 0, 255) / 255.f : g_zone_vol;
    g_zone_active = true;
}

// "LZG <start> <hexfrac...>" — per-LED length fraction (00..FF) from absolute
// index `start`, chunkable. Only Gradient/Wave zones need it.
void lzg_line(const String& line) {
    int sp = line.indexOf(' ', 4);
    if (sp < 0) return;
    int idx = line.substring(4, sp).toInt();
    const int hstart = sp + 1;
    for (int i = hstart; i + 1 < (int)line.length() && idx < (int)kLedZoneMax; i += 2, ++idx) {
        const int hi = hexval(line[i]), lo = hexval(line[i + 1]);
        if (hi < 0 || lo < 0) break;
        if (idx >= 0) g_lz_frac[idx] = (uint8_t)((hi << 4) | lo);
    }
    g_zone_active = true;
}

// "LZOG <start> <hexfrac...>" — per-LED fraction along the overlay axis, same
// wire shape as LZG. Sent only when an overlay's angle/geometry changes.
void lzog_line(const String& line) {
    int sp = line.indexOf(' ', 5);
    if (sp < 0) return;
    int idx = line.substring(5, sp).toInt();
    const int hstart = sp + 1;
    for (int i = hstart; i + 1 < (int)line.length() && idx < (int)kLedZoneMax; i += 2, ++idx) {
        const int hi = hexval(line[i]), lo = hexval(line[i + 1]);
        if (hi < 0 || lo < 0) break;
        if (idx >= 0) g_lz_ovfrac[idx] = (uint8_t)((hi << 4) | lo);
    }
}

// "LZOV <zi> <on> <RRGGBB> <shape> <opacity0-255> <soft0-255>" — overlay params.
void lzov_line(const String& line) {
    String t[7];
    if (split_ws(line, t, 7) < 7) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    LedZoneState& z = g_lz[zi];
    z.ov_on = t[2].toInt() ? 1 : 0;
    parse_hex_rgb(t[3], z.ov_r, z.ov_g, z.ov_b);
    z.ov_shape   = (uint8_t)constrain(t[4].toInt(), 0, 2);
    z.ov_opacity = (uint8_t)constrain(t[5].toInt(), 0, 255);
    z.ov_soft    = (uint8_t)constrain(t[6].toInt(), 1, 255);
    if (!z.ov_on) { z.ov_amount = 0; z.ov_phase = 0; }
    g_zone_active = true;
}

// "LZOA <zi> <amount0-255> <phase0-255>" — the animated pair, streamed while an
// overlay runs (host owns the rise/hold/retract timing).
void lzoa_line(const String& line) {
    String t[4];
    if (split_ws(line, t, 4) < 4) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    g_lz[zi].ov_amount = (uint8_t)constrain(t[2].toInt(), 0, 255);
    g_lz[zi].ov_phase  = (uint8_t)constrain(t[3].toInt(), 0, 255);
}

// "SERVOCAL <ch> <min_us> <max_us>" — pulse-width calibration for one channel.
// This is what actually sets how much TRAVEL a servo has: the library maps
// 0-180 deg linearly onto [min_us, max_us], so too narrow a window looks like a
// servo that "only moves a little". Applied on the next attach; re-attaches
// immediately if the channel is already live so the change is visible at once.
void servocal_line(const String& line) {
    String t[4];
    if (split_ws(line, t, 4) < 4) return;
    const int ch = t[1].toInt();
    if (ch < 0 || ch >= kServoMax) return;
    int lo = t[2].toInt(), hi = t[3].toInt();
    if (hi < lo) { const int sw = lo; lo = hi; hi = sw; }
    g_servo_min_us[ch] = (int16_t)constrain(lo, 200, 3000);
    g_servo_max_us[ch] = (int16_t)constrain(hi, 250, 3000);
    if (!g_servo_on[ch]) return;
    // Apply immediately so the change can be felt while scrubbing the slider.
    // The PCA9685 takes the new width on the next write; the GPIO backend bakes
    // the window into attach(), so it has to be re-attached.
    if (g_pca_ok) {
        servo_out_deg(ch, g_servo_cur[ch]);
    } else if (ch < kServoGpio) {
        g_servo[ch].detach();
        servo_attach(ch);
        servo_out_deg(ch, g_servo_cur[ch]);
    }
}

// "PROBE <gp>" — measure the pulse train actually present on ANY GPIO, using
// pulseIn. This is the instrument-free way to answer "is a signal being generated
// here at all, and if not, where did it go?" — a PIO whose pin field was
// mis-programmed drives a DIFFERENT pad, and probing the suspected alias finds it.
// Reports the HIGH and LOW widths in microseconds (0 = no edge within the
// timeout) plus the instantaneous level.
//   PROBE <gp> hi_us=<n> lo_us=<n> lvl=<0|1>
// A servo signal reads hi_us ~= 1000-2000 and lo_us ~= 18000-19000 (50 Hz frame).
void probe_line(const String& line) {
    String t[2];
    if (split_ws(line, t, 2) < 2) return;
    const int gp = t[1].toInt();
    if (gp < 0 || gp >= (int)NUM_BANK0_GPIOS) return;
    const unsigned long hi = pulseIn((uint8_t)gp, HIGH, 40000);
    const unsigned long lo = pulseIn((uint8_t)gp, LOW, 40000);
    Serial.print("PROBE ");     Serial.print(gp);
    Serial.print(" hi_us=");    Serial.print(hi);
    Serial.print(" lo_us=");    Serial.print(lo);
    Serial.print(" lvl=");      Serial.print(digitalRead((uint8_t)gp));
    Serial.println();
}

// "SWEEPPIN <gp>" — wiggle a servo on an ARBITRARY GPIO, to find which header pin
// a servo is really plugged into. The four servo channels are NOT on contiguous
// header pins (a GND and the NeoPixel line sit in the middle of the run), so a
// plausible-looking wiring job can put a servo on a pin no channel drives. This
// attaches a temporary Servo, sweeps it visibly, then releases the pin — so
// sweeping each candidate in turn identifies the real wiring by eye.
void sweeppin_line(const String& line) {
    String t[2];
    if (split_ws(line, t, 2) < 2) return;
    const int gp = t[1].toInt();
    if (gp < 0 || gp >= (int)NUM_BANK0_GPIOS) return;
    // Don't fight a channel that already owns this pin (GPIO backend only — with
    // a PCA9685 fitted no servo channel touches a Pico pin).
    for (int ch = 0; ch < kServoGpio; ++ch)
        if (!g_pca_ok && g_servo_on[ch] && kServoPins[ch] == (int8_t)gp) {
            g_servo[ch].detach();
            g_servo_on[ch] = false;
            g_servo_att[ch] = -1;
        }
    Servo s;
    if (s.attach(gp, 1000, 2000) < 0) {
        Serial.print("SWEEPPIN "); Serial.print(gp);
        Serial.println(" attach-failed");
        return;
    }
    for (int i = 0; i < 3; ++i) {
        s.write(60);  delay(400);
        s.write(120); delay(400);
    }
    s.write(90); delay(300);
    s.detach();                        // frees the PIO state machine
    Serial.print("SWEEPPIN "); Serial.print(gp); Serial.println(" done");
}

// "SERVOBUS" — which backend is live, and re-probe for the board. Safe to call
// any time: fitting the PCA9685 and sending this switches over with no reflash
// and no reboot. Prints a summary line either way.
//   SERVOBUS pca=<0|1> addr=<hex> hz=<n> channels=<n> backend=<pca9685|gpio>
void servobus_dump(bool reprobe) {
    if (reprobe) pca_begin();
    Serial.print("SERVOBUS pca=");   Serial.print(g_pca_ok ? 1 : 0);
    Serial.print(" addr=0x");        Serial.print(kPcaAddr, HEX);
    Serial.print(" hz=");            Serial.print(kPcaFreqHz);
    Serial.print(" channels=");      Serial.print(g_pca_ok ? kServoMax : kServoGpio);
    Serial.print(" backend=");       Serial.println(g_pca_ok ? "pca9685" : "gpio");
}

// "SERVOSTAT" — per-channel truth for diagnosing a servo that won't move.
// `att` is the one that matters: on the GPIO backend a failed attach still
// leaves the channel looking driven while every write is silently dropped.
//   SERVO <ch> gp=<n|-1> on=<0|1> att=<-1|0|1> cur=<deg> tgt=<deg> spd=<deg/s>
//         us=<min>-<max>
// gp is the driving pin on the GPIO backend, or -1 on the PCA9685 (where the
// channel is a board output, not a Pico pin).
void servostat_dump() {
    servobus_dump(false);
    // With no board fitted only the four direct-pin channels exist; listing all
    // 16 then would just be noise.
    const int n = g_pca_ok ? kServoMax : kServoGpio;
    for (int ch = 0; ch < n; ++ch) {
        const int gp = g_pca_ok ? -1
                     : (ch < kServoGpio ? kServoPins[ch] : -1);
        Serial.print("SERVO ");
        Serial.print(ch);
        Serial.print(" gp=");   Serial.print(gp);
        Serial.print(" on=");   Serial.print(g_servo_on[ch] ? 1 : 0);
        Serial.print(" att=");  Serial.print(g_servo_att[ch]);
        Serial.print(" cur=");  Serial.print((int)lroundf(g_servo_cur[ch]));
        Serial.print(" tgt=");  Serial.print((int)lroundf(g_servo_tgt[ch]));
        Serial.print(" spd=");  Serial.print((int)lroundf(g_servo_spd[ch]));
        Serial.print(" us=");   Serial.print(g_servo_min_us[ch]);
        Serial.print("-");      Serial.print(g_servo_max_us[ch]);
        Serial.println();
    }
    Serial.println("SERVOSTAT END");
}

// "LZS <zi> <n> <RRGGBB> ..." — set zone zi's multi-stop gradient (n stops,
// base -> tip). n = 0 clears it, which drops the zone back to its flat Color.
// This is the ONLY gradient source now: LZP still carries a colour2 field for
// wire compatibility, but nothing renders from it.
void lzs_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[3 + kLedMaxStops];
    const int nt = split_ws(line, t, 3 + kLedMaxStops);
    if (nt < 3) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    LedZoneState& z = g_lz[zi];
    int n = t[2].toInt();
    if (n < 0) n = 0;
    if (n > kLedMaxStops) n = kLedMaxStops;
    if (n > nt - 3) n = nt - 3;                // fewer colours arrived than claimed
    for (int k = 0; k < n; ++k)
        parse_hex_rgb(t[3 + k], z.stops[k][0], z.stops[k][1], z.stops[k][2]);
    z.nstops = (uint8_t)n;
    g_zone_active = true;
}

// "LZF <zi> <ms>" — fire a white flash overlay on zone zi.
void lzf_line(const String& line) {
    String t[3];
    if (split_ws(line, t, 3) < 3) return;
    const int zi = t[1].toInt();
    if (zi < 0 || zi >= kLedZones) return;
    const uint32_t ms = (uint32_t)constrain(t[2].toInt(), 1, 10000);
    g_lz[zi].flash_start = millis();
    g_lz[zi].flash_end   = g_lz[zi].flash_start + ms;
    g_zone_active = true;
}

// "LZSYNC <0|1>" — share one phase origin across the four side zones so their
// time-based effects start aligned.
void lzsync_line(const String& line) {
    String t[2];
    if (split_ws(line, t, 2) < 2) return;
    g_zone_sync = (t[1].toInt() != 0);
    if (g_zone_sync) g_zone_t0 = millis();
    g_zone_active = true;
}

void touch_setup() {
    for (size_t i = 0; i < 6; ++i)
        if (kTouchPins[i] >= 0)
            pinMode(kTouchPins[i], kTouchActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
}

void poll_touch(uint32_t now) {
    for (size_t i = 0; i < 6; ++i) {
        if (kTouchPins[i] < 0) continue;
        Touch& t = g_touch[i];
        const bool high = (digitalRead(kTouchPins[i]) == HIGH);
        const bool raw  = kTouchActiveHigh ? high : !high;     // true = touched
        if (raw != t.raw) { t.raw = raw; t.edge_ms = now; }
        if ((now - t.edge_ms) < kTouchDebounceMs) continue;
        if (raw != t.stable) {
            t.stable = raw;
            Serial.print("BOOP "); Serial.print(i);
            Serial.println(raw ? " 1" : " 0");
        }
    }
}

// Is this channel drivable by whichever backend is live? With a PCA9685 fitted
// every channel is; on the GPIO fallback only the four with a real pin are.
bool servo_channel_ok(int ch) {
    if (ch < 0 || ch >= kServoMax) return false;
    return g_pca_ok || (ch < kServoGpio && kServoPins[ch] >= 0);
}

// "SERVO <ch> <0-180 | off>" — drive a servo INSTANTLY (snap). On the GPIO
// fallback, attaching claims the pin from the button scanner (see poll guard).
void servo_line(const String& line) {
    String t[3];
    if (split_ws(line, t, 3) < 3) return;
    const int ch = t[1].toInt();
    if (!servo_channel_ok(ch)) return;
    if (t[2] == "off") {
        if (g_servo_on[ch]) servo_out_off(ch);
        g_servo_on[ch]  = false;
        g_servo_spd[ch] = 0.f;
        g_servo_att[ch] = -1;
        // On the GPIO backend the pin stays retired from button duty until
        // reboot/APPLY (predictable); the PCA9685 borrows no button pins at all.
        return;
    }
    const int deg = constrain(t[2].toInt(), 0, 180);
    g_servo_cur[ch] = g_servo_tgt[ch] = deg;      // snap; stop any in-flight slew
    g_servo_spd[ch] = 0.f;
    if (!g_servo_on[ch]) {
        servo_attach(ch);                          // seeds the output at g_servo_cur
        g_servo_on[ch] = true;
    }
    servo_out_deg(ch, deg);
}

// "SERVOM <ch> <0-180> <speed>" — move to the angle at `speed` deg/s (0 = snap).
// The MCU eases toward it in servo_service(), so the CM5 sends one target per
// move instead of streaming angles.
void servom_line(const String& line) {
    String t[4];
    if (split_ws(line, t, 4) < 4) return;
    const int ch = t[1].toInt();
    if (!servo_channel_ok(ch)) return;
    const int   deg = constrain(t[2].toInt(), 0, 180);
    const float spd = t[3].toFloat();
    if (!g_servo_on[ch]) {                         // first command: snap-attach here
        g_servo_cur[ch] = deg;
        servo_attach(ch);
        g_servo_on[ch] = true;
        servo_out_deg(ch, deg);
    }
    g_servo_tgt[ch] = deg;
    g_servo_spd[ch] = (spd > 0.f) ? spd : 0.f;
    if (g_servo_spd[ch] <= 0.f) {                  // no speed → snap now
        g_servo_cur[ch] = deg;
        servo_out_deg(ch, deg);
    }
}

// Ease every slewing servo toward its target, ~66 Hz. Idle once it arrives.
void servo_service(uint32_t now) {
    if (now - g_servo_tick < 15) return;
    float dt = (g_servo_tick == 0) ? 0.015f : (now - g_servo_tick) / 1000.f;
    if (dt > 0.1f) dt = 0.1f;
    g_servo_tick = now;
    for (int ch = 0; ch < kServoMax; ++ch) {
        if (!g_servo_on[ch] || g_servo_spd[ch] <= 0.f) continue;
        if (g_servo_cur[ch] == g_servo_tgt[ch]) continue;   // arrived → hold (PWM keeps position)
        const float d = g_servo_tgt[ch] - g_servo_cur[ch];
        const float step = g_servo_spd[ch] * dt;
        g_servo_cur[ch] += (fabsf(d) <= step) ? d : (d > 0.f ? step : -step);
        servo_out_deg(ch, g_servo_cur[ch]);
    }
}

// True while any servo channel has claimed this GPIO — the button poller skips
// it. Only the GPIO fallback can claim a pin; with a PCA9685 fitted the servos
// use no Pico GPIO at all beyond the shared I2C pair, so buttons keep every pin.
bool gp_claimed_by_servo(uint8_t gp) {
    if (g_pca_ok) return false;
    for (int ch = 0; ch < kServoGpio; ++ch)
        if (g_servo_on[ch] && kServoPins[ch] == (int8_t)gp) return true;
    return false;
}

// "LEDZ <r> <g> <b> [count]" — solid fill (0 0 0 = off; stops any pattern).
void ledz_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[5];
    const int nt = split_ws(line, t, 5);
    if (nt < 4) return;
    const uint8_t r = constrain(t[1].toInt(), 0, 255);
    const uint8_t g = constrain(t[2].toInt(), 0, 255);
    const uint8_t b = constrain(t[3].toInt(), 0, 255);
    if (nt >= 5) g_led_n = constrain(t[4].toInt(), 1, (int)kLedZoneMax);
    g_zone_active = false;                       // legacy whole-chain path reclaims the strip
    g_led_mode = (r || g || b) ? 1 : 0;
    led_fill(r, g, b);
    led_show();
}

// "LEDP <mode> [r g b] [speed]" — local pattern: 0 off, 1 solid, 2 rainbow,
// 3 chase, 4 breathe. Animates on the MCU so the USB link stays idle.
void ledp_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[7];
    const int nt = split_ws(line, t, 7);
    if (nt < 2) return;
    g_zone_active = false;                       // legacy whole-chain path reclaims the strip
    g_led_mode = constrain(t[1].toInt(), 0, 4);
    if (nt >= 5) {
        g_led_r = constrain(t[2].toInt(), 0, 255);
        g_led_g = constrain(t[3].toInt(), 0, 255);
        g_led_b = constrain(t[4].toInt(), 0, 255);
    }
    if (nt >= 6) g_led_speed = constrain(t[5].toInt(), 1, 255);
    if (g_led_mode == 0) { led_fill(0, 0, 0); led_show(); }
    if (g_led_mode == 1) { led_fill(g_led_r, g_led_g, g_led_b); led_show(); }
}

// "LEDB <0-255>" — software brightness (APA102 also maps it to the 5-bit
// per-LED global, keeping PWM resolution).
void ledb_line(const String& line) {
    String t[2];
    if (split_ws(line, t, 2) < 2) return;
    g_led_bright = constrain(t[1].toInt(), 0, 255);
    led_show();
}

// Per-pixel streaming for CUSTOM PANELS / Pi-driven content:
//   "LEDF <start> <hexRRGGBB...>"  write pixels from index start (chunkable)
//   "LEDSHOW"                      latch the assembled frame to the LEDs
void ledf_line(const String& line) {
    int sp = line.indexOf(' ', 5);
    if (sp < 0) return;
    int idx = line.substring(5, sp).toInt();
    const int hstart = sp + 1;
    // Cap on the buffer, not g_led_n: a streamed frame auto-sizes the zone
    // (grows g_led_n to the highest index written) so the Pi can drive the
    // whole accessory chain over this link without a separate count command.
    for (int i = hstart; i + 5 < (int)line.length() && idx < (int)kLedZoneMax; i += 6, ++idx) {
        int v[6];
        bool ok = true;
        for (int k = 0; k < 6; ++k) { v[k] = hexval(line[i + k]); if (v[k] < 0) ok = false; }
        if (!ok) break;
        if (idx >= 0) {
            g_led_px[idx][0] = (uint8_t)((v[0] << 4) | v[1]);
            g_led_px[idx][1] = (uint8_t)((v[2] << 4) | v[3]);
            g_led_px[idx][2] = (uint8_t)((v[4] << 4) | v[5]);
            if (idx + 1 > (int)g_led_n) g_led_n = idx + 1;
        }
    }
    g_zone_active = false;              // legacy frame-streaming path reclaims the strip
    g_led_mode = 5;                     // frame-streamed: stop local patterns
    g_led_dirty = true;
}

// "ADCREAD" — one-shot report of the three test ADC channels.
void adc_read() {
    analogReadResolution(12);
    for (int ch = 0; ch < 3; ++ch) {
        const int raw = analogRead(kAdcPins[ch]);
        const long mv = (long)raw * 3300 / 4095;
        Serial.print("ADC "); Serial.print(ch); Serial.print(' ');
        Serial.print(raw);    Serial.print(' '); Serial.println(mv);
    }
}

// "PINS" — live readout of every GP the package exposes: its configured role(s)
// and its current level, one "PIN <gp> <val> <roles>" line each, bracketed by
// "PINS <ngpio> n=<buttons>" and "PINS END". ADC-role pins report millivolts
// ("812mv"); everything else reports the input level (0/1). The dump only ever
// READS: digitalRead touches no pin config, and the input buffer of unassigned
// pins is enabled just for the read (then restored) so a jumper wiggled on a
// free pin shows up without leaving floating inputs enabled (RP2350-E9).
void pins_dump() {
    Serial.print("PINS "); Serial.print((int)NUM_BANK0_GPIOS);
    Serial.print(" n=");   Serial.println((int)g_npins);
    analogReadResolution(12);
    for (int gp = 0; gp < (int)NUM_BANK0_GPIOS; ++gp) {
        String role;
        auto add = [&role](const String& r) {
            if (role.length()) role += '+';
            role += r;
        };
        bool adc_role = false;
        bool btn_role = false;
        for (size_t i = 0; i < g_npins; ++i) {
            if (g_pins[i].gp == gp) {
                add(String("btn") + i + (gp_claimed_by_servo(gp) ? "(servo)" : ""));
                btn_role = true;
            }
            if (g_pins[i].led >= 0 && g_pins[i].led == gp) add(String("led") + i);
        }
        // Servo role. The suppression here must key off the pin ALSO being a
        // button (that branch already tagged it "btnN(servo)") — the old test was
        // !gp_claimed_by_servo(gp), which is true exactly WHEN a servo owns the
        // pin, so it cancelled itself and the role could never print. That went
        // unnoticed while servos shared GP6-9 with buttons and the btn label
        // covered for it; on dedicated pins it left them reported as "free".
        // Idle channels are listed too, so an allocated-but-not-yet-driven servo
        // pin doesn't look unused.
        // Only the GPIO fallback puts a servo on a pin; with a PCA9685 fitted
        // these pins are genuinely free, so don't claim them in the dump.
        if (!g_pca_ok)
            for (int ch = 0; ch < kServoGpio; ++ch)
                if (kServoPins[ch] == (int8_t)gp && !btn_role)
                    add(String("servo") + ch +
                        (!g_servo_on[ch]          ? "(idle)"
                         : g_servo_att[ch] == 0   ? "(ATTACH-FAILED)"
                                                  : ""));
        if (kPcaSdaPin == gp) add("pca_sda");
        if (kPcaSclPin == gp) add("pca_scl");
        for (int i = 0; i < 6; ++i)
            if (kTouchPins[i] == gp) add(String("touch") + i);
        if (kLedZonePin == gp) add("ledz");
        if (kLedZoneType == 1 && kLedZoneClkPin == gp) add("ledclk");
        for (int ch = 0; ch < 3; ++ch)
            if (kAdcPins[ch] == gp) { add(String("adc") + ch); adc_role = true; }
#ifdef VOICE_CHANGER
        if (kMicAdcPin == gp)      { add("mic"); adc_role = true; }
        if (kI2sBclkPin == gp)       add("i2s_bclk");
        if (kI2sBclkPin + 1 == gp)   add("i2s_ws");
        if (kI2sDoutPin == gp)       add("i2s_din");
        if (kDacSdaPin == gp)        add("dac_sda");
        if (kDacSclPin == gp)        add("dac_scl");
        if (kDacResetPin == gp)      add("dac_rst");
#endif
#ifdef MAX_BRIDGE
        if (kMaxSpiSck == gp)        add("max_clk");
        if (kMaxSpiTx == gp)         add("max_din");
        for (size_t i = 0; i < kMaxCs; ++i)
            if (kMaxCsPins[i] == gp) add(String("max_cs") + i);
#endif
#ifdef PERIPHERAL_HUB
        if (kOneWirePin == gp)       add("1wire");
        for (size_t i = 0; i < sizeof(kFanPins); ++i)
            if (kFanPins[i] == gp)   add(String("fan") + i);
#endif
        const bool free_pin = !role.length();
        if (free_pin) role = "free";

        Serial.print("PIN "); Serial.print(gp); Serial.print(' ');
        if (adc_role) {
            const long mv = (long)analogRead((pin_size_t)gp) * 3300 / 4095;
            Serial.print(mv); Serial.print("mv");
        } else {
            if (free_pin) gpio_set_input_enabled(gp, true);
            Serial.print(digitalRead((pin_size_t)gp) == HIGH ? 1 : 0);
            if (free_pin) gpio_set_input_enabled(gp, false);
        }
        Serial.print(' '); Serial.println(role);
    }
    Serial.println("PINS END");
}

// Seed the live map from config.h's compiled-in defaults.
void load_default_pins() {
    g_npins = 0;
    const size_t n = sizeof(kButtonPins) / sizeof(kButtonPins[0]);
    for (size_t i = 0; i < n && g_npins < kMaxButtons; ++i) {
        g_pins[g_npins] = PinCfg{ kButtonPins[i], /*pull=up*/ 0, /*active_low*/ true,
                                  kLedPins[i] };
        ++g_npins;
    }
}

// (Re)apply pinModes for the current map and reset all debounce state. Called at
// boot and on "PINCFG APPLY".
void apply_pins() {
    for (size_t i = 0; i < g_npins; ++i) {
        const uint8_t mode = g_pins[i].pull == 1 ? INPUT_PULLDOWN
                           : g_pins[i].pull == 2 ? INPUT
                           :                       INPUT_PULLUP;
        pinMode(g_pins[i].gp, mode);
        if (g_pins[i].led >= 0) {
            pinMode(g_pins[i].led, OUTPUT);
            digitalWrite(g_pins[i].led, LOW);
        }
        g_btn[i] = Button{};   // fresh debounce state for the (possibly new) pin
    }
}

// One message per line. `value` is appended unsigned-decimal where used.
void emit(const char* verb, size_t id, const char* evt) {
    Serial.print(verb); Serial.print(' '); Serial.print(id);
    Serial.print(' ');  Serial.println(evt);
}

void send_hello() {
    Serial.print("HELLO proto-buttons v1 fw=");
    Serial.print(kFwVersion);
    Serial.print(" n=");
    Serial.println(g_npins);
}

// Split a String on runs of spaces into up to maxn tokens; returns the count.
int split_ws(const String& s, String* out, int maxn) {
    int n = 0, i = 0; const int len = s.length();
    while (i < len && n < maxn) {
        while (i < len && s[i] == ' ') ++i;
        if (i >= len) break;
        int j = i;
        while (j < len && s[j] != ' ') ++j;
        out[n++] = s.substring(i, j);
        i = j;
    }
    return n;
}
uint8_t pull_from(const String& t) {          // "up|down|none" or "0|1|2"
    if (t == "1" || t.equalsIgnoreCase("down")) return 1;
    if (t == "2" || t.equalsIgnoreCase("none")) return 2;
    return 0;                                  // up (default)
}

// "I2CSCAN [sda] [scl]" — probe 0x08-0x77 on the given GPIOs (default GP20/21,
// the voice DAC's I2C0 bus) and reply "I2C <hex> <hex> …" ("I2C none" if quiet,
// "I2C err bad-pins" if the pair is invalid). The RP2350's I2C mux is fixed —
// SDA on even GPs, SCL on odd, controller = GP bit 1 — and we validate BEFORE
// touching the bus: arduino-pico's setSDA/setSCL assert-halt on a pin the
// instance can't use, which would freeze buttons/voice/MAX along with the scan.
void i2c_scan(const String& line) {
    int sda = 20, scl = 21;
    String t[3];
    const int nt = split_ws(line, t, 3);       // t[0]="I2CSCAN"
    if (nt >= 3) { sda = t[1].toInt(); scl = t[2].toInt(); }
    const bool pair_ok = sda >= 0 && sda <= 47 && scl >= 0 && scl <= 47 &&
                         (sda & 1) == 0 && (scl & 1) == 1 &&   // SDA even, SCL odd
                         (sda & 2) == (scl & 2);               // same controller
    if (!pair_ok) { Serial.println("I2C err bad-pins"); return; }

    TwoWire& w = (sda & 2) ? Wire1 : Wire;     // GP bit 1 → I2C1
    w.end();
    w.setSDA(sda); w.setSCL(scl); w.setClock(100000); w.begin();
    Serial.print("I2C");
    int found = 0;
    for (int a = 0x08; a <= 0x77; ++a) {
        w.beginTransmission(static_cast<uint8_t>(a));
        if (w.endTransmission() == 0) { Serial.print(' '); Serial.print(a, HEX); ++found; }
    }
    if (!found) Serial.print(" none");
    Serial.println();
    w.end();
#ifdef VOICE_CHANGER
    // Scanning I2C0 borrows the voice DAC's controller (and may have remapped
    // it); hand it back on its own pins so volume/mute keep working.
    if (!(sda & 2)) {
        Wire.setSDA(kDacSdaPin); Wire.setSCL(kDacSclPin);
        Wire.setClock(100000);   Wire.begin();
    }
#endif
}

void poll_button(size_t i, uint32_t now) {
    Button& b = g_btn[i];
    const bool high = (digitalRead(g_pins[i].gp) == HIGH);
    const bool raw  = g_pins[i].active_low ? high : !high;    // true = released
    if (raw != b.raw) { b.raw = raw; b.edge_ms = now; }       // bounce → restart timer
    if ((now - b.edge_ms) < kDebounceMs) return;              // still settling

    if (raw != b.stable) {                                    // debounced edge
        b.stable = raw;
        if (!raw) {                                           // pressed (falling)
            b.down_ms = now; b.long_fired = false;
            if (kEmitDownUp) emit("BTN", i, "DOWN");
        } else {                                              // released (rising)
            if (kEmitDownUp) emit("BTN", i, "UP");
            if (!b.long_fired) {
                emit("BTN", i, "SHORT");                      // released before LONG
#ifdef VOICE_CHANGER
                // Standalone control: toggle/cycle the voice changer locally so
                // it works without the Pi (the SHORT is still reported above).
                if (static_cast<int>(i) == kVoiceToggleBtn) voice_local_toggle();
                if (static_cast<int>(i) == kVoiceCycleBtn)  voice_local_cycle();
#endif
            }
        }
        return;
    }

    // Steady held state: fire LONG exactly once at the threshold.
    if (!b.stable && !b.long_fired && (now - b.down_ms) >= g_long_ms) {
        b.long_fired = true;
        emit("BTN", i, "LONG");
    }
}

// Parse one inbound line from the Pi. Everything optional / forward-compatible.
void handle_line(const String& line) {
#ifdef MAX_BRIDGE
    if (line.startsWith("SPI ")) { max_bridge_line(line); return; }  // high-rate; first
#endif
    if (line.startsWith("I2CSCAN")) { i2c_scan(line); return; }
    if (line.startsWith("SERVOM ")) { servom_line(line); return; }   // smooth move
    if (line.startsWith("SERVOCAL ")) { servocal_line(line); return; } // pulse range
    if (line == "SERVOSTAT")          { servostat_dump();   return; } // per-ch truth
    if (line == "SERVOBUS")           { servobus_dump(true); return; } // re-probe PCA
    if (line.startsWith("PROBE "))    { probe_line(line);   return; } // measure a pin
    if (line.startsWith("SWEEPPIN ")) { sweeppin_line(line); return; } // find wiring
    if (line.startsWith("SERVO "))  { servo_line(line); return; }
    if (line.startsWith("LEDZ "))   { ledz_line(line);  return; }
    if (line.startsWith("LEDP "))   { ledp_line(line);  return; }
    if (line.startsWith("LEDB "))   { ledb_line(line);  return; }
    if (line.startsWith("LEDF "))   { ledf_line(line);  return; }
    if (line == "LEDSHOW")          { if (g_led_dirty) { led_show(); g_led_dirty = false; } return; }
    if (line.startsWith("LZONE "))  { lzone_line(line);  return; }   // coproc_local: per-zone descriptors
    if (line.startsWith("LZP "))    { lzp_line(line);    return; }
    if (line.startsWith("LZOG "))   { lzog_line(line);   return; }
    if (line.startsWith("LZOV "))   { lzov_line(line);   return; }
    if (line.startsWith("LZOA "))   { lzoa_line(line);   return; }
    if (line.startsWith("LZG "))    { lzg_line(line);    return; }
    if (line.startsWith("LZS "))    { lzs_line(line);    return; }
    if (line.startsWith("LZF "))    { lzf_line(line);    return; }
    if (line.startsWith("LZSYNC ")) { lzsync_line(line); return; }
    if (line.startsWith("LVOL "))   { lvol_line(line);   return; }
    if (line == "ADCREAD")          { adc_read();       return; }
    if (line == "PINS")             { pins_dump();      return; }
#ifdef PERIPHERAL_HUB
    if (periph_handle_command(line)) return;  // FAN <zone> <duty%>
#endif
#ifdef VOICE_CHANGER
    if (voice_handle_command(line)) return;   // VOICE/FX/PITCH/MIX/PARAM
#endif
    if (line.startsWith("CFG long_ms=")) {
        long v = line.substring(12).toInt();
        if (v >= 100 && v <= 5000) g_long_ms = static_cast<uint32_t>(v);
    } else if (line.startsWith("LED ")) {
        int sp = line.indexOf(' ', 4);
        if (sp > 0) {
            int id = line.substring(4, sp).toInt();
            int on = line.substring(sp + 1).toInt();
            if (id >= 0 && id < static_cast<int>(g_npins) && g_pins[id].led >= 0)
                digitalWrite(g_pins[id].led, on ? HIGH : LOW);
        }
    } else if (line.startsWith("PINCFG ")) {
        // Runtime pin map (see docs/coprocessor-input.md):
        //   PINCFG CLR                         start an empty map
        //   PINCFG BTN <gp> [pull] [alow]      append a button (its index = id)
        //                                      pull=up|down|none, alow=1|0
        //   PINCFG LED <id> <gp>               backlight pin for a button
        //   PINCFG APPLY                       (re)init pinModes + re-HELLO
        String t[5];
        const int nt = split_ws(line, t, 5);            // t[0] = "PINCFG"
        const String sub = nt > 1 ? t[1] : String("");
        if (sub == "CLR") {
            g_npins = 0;
        } else if (sub == "APPLY") {
            apply_pins();
            send_hello();
        } else if (sub == "BTN" && nt >= 3) {
            const int gp = t[2].toInt();
            const uint8_t pull = nt >= 4 ? pull_from(t[3]) : 0;
            const bool    alow = nt >= 5 ? (t[4].toInt() != 0) : true;
            if (gp >= 0 && gp <= 47 && g_npins < kMaxButtons)   // RP2350A 0-29, RP2350B 0-47
                g_pins[g_npins++] = PinCfg{ static_cast<uint8_t>(gp), pull, alow, -1 };
        } else if (sub == "LED" && nt >= 4) {
            const int id = t[2].toInt();
            const int gp = t[3].toInt();
            if (id >= 0 && id < static_cast<int>(g_npins) && gp >= 0 && gp <= 47)
                g_pins[id].led = static_cast<int8_t>(gp);
        }
    }
    // "PONG" and any unknown line: ignore.
}

void drain_input() {
    static String rx;
    static bool   rx_reserved = false;
    if (!rx_reserved) { rx.reserve(600); rx_reserved = true; }  // avoid per-char reallocs on long SPI lines
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (rx.length()) { handle_line(rx); rx = ""; }
        } else if (rx.length() < 600) {   // SPI <cs> <hex> frames are long
            rx += c;
        } else {
            rx = "";   // overflow → resync on next newline
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);                       // USB CDC (baud is nominal for CDC)
    load_default_pins();                        // config.h defaults; Pi may re-push
    apply_pins();
    servo_setup();                              // probe the PCA9685; GPIO fallback
    touch_setup();                              // TTP223 test pads (config.h)
#ifdef MAX_BRIDGE
    max_bridge_setup();                         // MAX7219 USB→SPI bridge (SPI1)
#endif
#ifdef PERIPHERAL_HUB
    periph_setup();                             // boop pads + DS18B20 + fan PWM
#endif
}

void loop() {
    const uint32_t now = millis();

    // (Re)greet whenever the host opens the CDC port (DTR asserted). Re-arming on
    // the disconnect→connect edge means a HUD restart re-reads our button count.
    const bool connected = static_cast<bool>(Serial);
    if (connected && !g_was_connected) {
        send_hello();
        g_last_ping = now;
    }
    g_was_connected = connected;

    for (size_t i = 0; i < g_npins; ++i) {
        if (gp_claimed_by_servo(g_pins[i].gp)) continue;   // slot became a servo
        poll_button(i, now);
    }
    poll_touch(now);

    if (connected && (now - g_last_ping) >= kPingMs) {
        g_last_ping = now;
        Serial.println("PING");
    }

    drain_input();
    led_service(now);                           // legacy whole-chain patterns (~30 fps)
    led_zone_render(now);                        // coproc_local per-zone compositor (~60 fps)
    servo_service(now);                          // smooth servo slew (~66 fps)
#ifdef PERIPHERAL_HUB
    periph_service();                           // boop poll · one temp step · fans
#endif
}

#ifdef VOICE_CHANGER
// Core1 runs the voice changer independently of the button/protocol loop above.
void setup1() { voice_setup(); }
void loop1()  { voice_service(); }
#endif
