# button_coproc — optional button/switch coprocessor

Reference firmware for the **optional** button coprocessor described in
[`docs/coprocessor-input.md`](../../docs/coprocessor-input.md). A small MCU
(**RP2350** or an earlier **RP2040/Pico**) debounces the physical switches,
classifies short vs long press, and streams events to the Pi over **USB CDC**.
ProtoHUD dispatches them through the same `input::GpioFunc` path as on-board
GPIO — so the coprocessor is never required and can be toggled off in
**System → GPIO Buttons → Button Coprocessor**.

## Protocol (v1, newline-delimited ASCII)

MCU → Pi:
```
HELLO proto-buttons v1 n=<count>   # once on boot
BTN <id> SHORT                     # debounced, held < long_ms
BTN <id> LONG                      # debounced, held >= long_ms (fires once)
PING                               # heartbeat ~1 Hz
```
Pi → MCU (optional): `PONG` (heartbeat ack). The Pi maps `<id>` → function via
`inputs.coprocessor.buttons`, so the firmware stays "dumb about meaning".

## USB identity

Set the USB **serial string** (and ideally product = `ProtoHUD_Buttons`) so the
Pi can bind a stable `/dev/serial/by-id/usb-ProtoHUD_Buttons-if00` path instead
of a racing `/dev/ttyACMn`. In Arduino-Pico: `Serial` is USB CDC by default; set
the board's USB product/serial in the IDE or via `TinyUSB`.

## Reference sketch (Arduino — RP2040/RP2350 "arduino-pico" core)

```cpp
// button_coproc.ino — debounce N switches, stream SHORT/LONG over USB CDC.
#include <Arduino.h>

constexpr uint8_t  PINS[]    = {2, 3, 4};        // GPIOs wired to switches (to GND)
constexpr size_t   N         = sizeof(PINS);
constexpr uint32_t DEBOUNCE  = 15;               // ms
constexpr uint32_t LONG_MS   = 600;              // short/long threshold
constexpr uint32_t HEARTBEAT = 1000;             // ms between PINGs

bool     stable[N], lastRaw[N], longSent[N];
uint32_t tEdge[N], tDown[N], tPing;

void setup() {
  Serial.begin(115200);                          // USB CDC
  for (size_t i = 0; i < N; ++i) {
    pinMode(PINS[i], INPUT_PULLUP);              // pressed = LOW
    stable[i] = lastRaw[i] = true;               // released (HIGH)
    longSent[i] = false;
  }
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}     // wait briefly for host
  Serial.print("HELLO proto-buttons v1 n="); Serial.println(N);
  tPing = millis();
}

void loop() {
  const uint32_t now = millis();
  for (size_t i = 0; i < N; ++i) {
    const bool raw = digitalRead(PINS[i]);       // HIGH=released, LOW=pressed
    if (raw != lastRaw[i]) { lastRaw[i] = raw; tEdge[i] = now; }   // bounce timer
    if (now - tEdge[i] >= DEBOUNCE && raw != stable[i]) {
      stable[i] = raw;
      if (!raw) {                                // pressed (falling edge)
        tDown[i] = now; longSent[i] = false;
      } else {                                   // released (rising edge)
        if (!longSent[i]) { Serial.print("BTN "); Serial.print(i); Serial.println(" SHORT"); }
      }
    }
    if (!stable[i] && !longSent[i] && now - tDown[i] >= LONG_MS) {
      longSent[i] = true;                        // fire LONG once while held
      Serial.print("BTN "); Serial.print(i); Serial.println(" LONG");
    }
  }
  if (now - tPing >= HEARTBEAT) { tPing = now; Serial.println("PING"); }

  while (Serial.available()) Serial.read();      // drain PONG/etc.
}
```

## Wiring notes

- Switches to MCU GPIO with `INPUT_PULLUP`, other side to GND (`active_low`).
- USB-C from the MCU to the Pi/CM5 (consider the carrier's onboard USB hub —
  `hardware/carrier-board/`).
- This offloads the scarce GPIO that HUB75 leaves free on the Pi; see
  [`hardware/carrier-board/MULTI-BACKEND.md`](../../hardware/carrier-board/MULTI-BACKEND.md).
- If you'd rather not add a board, the same `BTN`/`PING` CDC interface can be a
  **second USB interface** on the RP2350 helmet-audio processor (composite
  UAC2 + CDC).
