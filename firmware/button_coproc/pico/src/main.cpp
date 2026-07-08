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

// Servo test channels — attach lazily on the first SERVO command; a servo pin
// shared with a button silently retires that button until reboot/APPLY.
Servo g_servo[4];
bool  g_servo_on[4] = {};

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

// "SERVO <ch> <0-180 | off>" — drive a test servo. Attaching claims the pin
// from the button scanner (see poll loop guard).
void servo_line(const String& line) {
    String t[3];
    if (split_ws(line, t, 3) < 3) return;
    const int ch = t[1].toInt();
    if (ch < 0 || ch >= 4 || kServoPins[ch] < 0) return;
    if (t[2] == "off") {
        if (g_servo_on[ch]) g_servo[ch].detach();
        // pin stays retired from button duty until reboot/APPLY (predictable)
        return;
    }
    const int deg = constrain(t[2].toInt(), 0, 180);
    if (!g_servo_on[ch]) { g_servo[ch].attach(kServoPins[ch]); g_servo_on[ch] = true; }
    g_servo[ch].write(deg);
}

// True while any servo channel has claimed this GPIO — the button poller skips it.
bool gp_claimed_by_servo(uint8_t gp) {
    for (int ch = 0; ch < 4; ++ch)
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
    for (int i = hstart; i + 5 < (int)line.length() && idx < (int)g_led_n; i += 6, ++idx) {
        int v[6];
        bool ok = true;
        for (int k = 0; k < 6; ++k) { v[k] = hexval(line[i + k]); if (v[k] < 0) ok = false; }
        if (!ok) break;
        if (idx >= 0) {
            g_led_px[idx][0] = (uint8_t)((v[0] << 4) | v[1]);
            g_led_px[idx][1] = (uint8_t)((v[2] << 4) | v[3]);
            g_led_px[idx][2] = (uint8_t)((v[4] << 4) | v[5]);
        }
    }
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
    if (line.startsWith("SERVO "))  { servo_line(line); return; }
    if (line.startsWith("LEDZ "))   { ledz_line(line);  return; }
    if (line.startsWith("LEDP "))   { ledp_line(line);  return; }
    if (line.startsWith("LEDB "))   { ledb_line(line);  return; }
    if (line.startsWith("LEDF "))   { ledf_line(line);  return; }
    if (line == "LEDSHOW")          { if (g_led_dirty) { led_show(); g_led_dirty = false; } return; }
    if (line == "ADCREAD")          { adc_read();       return; }
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
    led_service(now);                           // local LED patterns (~30 fps)
#ifdef PERIPHERAL_HUB
    periph_service();                           // boop poll · one temp step · fans
#endif
}

#ifdef VOICE_CHANGER
// Core1 runs the voice changer independently of the button/protocol loop above.
void setup1() { voice_setup(); }
void loop1()  { voice_service(); }
#endif
