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

namespace {

constexpr size_t kNumButtons = sizeof(kButtonPins) / sizeof(kButtonPins[0]);

// Tunable at runtime via "CFG long_ms=<n>"; starts at the configured default.
uint32_t g_long_ms = kLongMsInit;

// Per-button debounce + press state.
struct Button {
    bool     raw        = true;   // last raw read (true = released, INPUT_PULLUP HIGH)
    bool     stable     = true;   // debounced state (true = released)
    uint32_t edge_ms    = 0;      // time of last raw change (debounce timer)
    uint32_t down_ms    = 0;      // when the debounced press began
    bool     long_fired = false;  // LONG already emitted for the current hold
};
Button   g_btn[kNumButtons];

uint32_t g_last_ping = 0;
bool     g_was_connected = false;   // tracks the USB CDC (DTR) edge for re-HELLO

// One message per line. `value` is appended unsigned-decimal where used.
void emit(const char* verb, size_t id, const char* evt) {
    Serial.print(verb); Serial.print(' '); Serial.print(id);
    Serial.print(' ');  Serial.println(evt);
}

void send_hello() {
    Serial.print("HELLO proto-buttons v1 n=");
    Serial.println(kNumButtons);
}

void poll_button(size_t i, uint32_t now) {
    Button& b = g_btn[i];
    const bool raw = (digitalRead(kButtonPins[i]) == HIGH);   // HIGH = released
    if (raw != b.raw) { b.raw = raw; b.edge_ms = now; }       // bounce → restart timer
    if ((now - b.edge_ms) < kDebounceMs) return;              // still settling

    if (raw != b.stable) {                                    // debounced edge
        b.stable = raw;
        if (!raw) {                                           // pressed (falling)
            b.down_ms = now; b.long_fired = false;
            if (kEmitDownUp) emit("BTN", i, "DOWN");
        } else {                                              // released (rising)
            if (kEmitDownUp) emit("BTN", i, "UP");
            if (!b.long_fired) emit("BTN", i, "SHORT");       // released before LONG
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
    if (line.startsWith("CFG long_ms=")) {
        long v = line.substring(12).toInt();
        if (v >= 100 && v <= 5000) g_long_ms = static_cast<uint32_t>(v);
    } else if (line.startsWith("LED ")) {
        int sp = line.indexOf(' ', 4);
        if (sp > 0) {
            int id = line.substring(4, sp).toInt();
            int on = line.substring(sp + 1).toInt();
            if (id >= 0 && id < static_cast<int>(kNumButtons) && kLedPins[id] >= 0)
                digitalWrite(kLedPins[id], on ? HIGH : LOW);
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
    for (size_t i = 0; i < kNumButtons; ++i) {
        pinMode(kButtonPins[i], INPUT_PULLUP);  // pressed = LOW
        if (kLedPins[i] >= 0) {
            pinMode(kLedPins[i], OUTPUT);
            digitalWrite(kLedPins[i], LOW);
        }
    }
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

    for (size_t i = 0; i < kNumButtons; ++i) poll_button(i, now);

    if (connected && (now - g_last_ping) >= kPingMs) {
        g_last_ping = now;
        Serial.println("PING");
    }

    drain_input();
}
