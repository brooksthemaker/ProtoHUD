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

inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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

// WS2812 test zone — constructed lazily on the first LEDZ command.
Adafruit_NeoPixel* g_ledz = nullptr;

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

// "LEDZ <r> <g> <b> [count]" — fill the test strip with one color (0 0 0 = off).
void ledz_line(const String& line) {
    if (kLedZonePin < 0) return;
    String t[5];
    const int nt = split_ws(line, t, 5);
    if (nt < 4) return;
    const int r = constrain(t[1].toInt(), 0, 255);
    const int g = constrain(t[2].toInt(), 0, 255);
    const int b = constrain(t[3].toInt(), 0, 255);
    const int n = (nt >= 5) ? constrain(t[4].toInt(), 1, 300) : kLedZoneCount;
    if (!g_ledz || g_ledz->numPixels() != (uint16_t)n) {
        delete g_ledz;
        g_ledz = new Adafruit_NeoPixel(n, kLedZonePin, NEO_GRB + NEO_KHZ800);
        g_ledz->begin();
    }
    for (int i = 0; i < n; ++i) g_ledz->setPixelColor(i, g_ledz->Color(r, g, b));
    g_ledz->show();
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
#ifdef PERIPHERAL_HUB
    periph_service();                           // boop poll · one temp step · fans
#endif
}

#ifdef VOICE_CHANGER
// Core1 runs the voice changer independently of the button/protocol loop above.
void setup1() { voice_setup(); }
void loop1()  { voice_service(); }
#endif
