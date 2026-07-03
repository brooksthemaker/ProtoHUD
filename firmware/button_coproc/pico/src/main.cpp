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
#include "config.h"
#ifdef VOICE_CHANGER
#include "voice.h"   // optional core1 voice changer (build with -DVOICE_CHANGER)
#endif

namespace {

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
            if (gp >= 0 && gp <= 29 && g_npins < kMaxButtons)
                g_pins[g_npins++] = PinCfg{ static_cast<uint8_t>(gp), pull, alow, -1 };
        } else if (sub == "LED" && nt >= 4) {
            const int id = t[2].toInt();
            const int gp = t[3].toInt();
            if (id >= 0 && id < static_cast<int>(g_npins) && gp >= 0 && gp <= 29)
                g_pins[id].led = static_cast<int8_t>(gp);
        }
    }
    // "PONG" and any unknown line: ignore.
}

void drain_input() {
    static String rx;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (rx.length()) { handle_line(rx); rx = ""; }
        } else if (rx.length() < 64) {
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

    for (size_t i = 0; i < g_npins; ++i) poll_button(i, now);

    if (connected && (now - g_last_ping) >= kPingMs) {
        g_last_ping = now;
        Serial.println("PING");
    }

    drain_input();
}

#ifdef VOICE_CHANGER
// Core1 runs the voice changer independently of the button/protocol loop above.
void setup1() { voice_setup(); }
void loop1()  { voice_service(); }
#endif
