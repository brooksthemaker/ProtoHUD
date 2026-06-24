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

## Buildable firmware: [`pico/`](pico/)

A complete PlatformIO project (RP2350 / **Raspberry Pi Pico 2 W**, earlephilhower
`arduino-pico` core):

```
pico/
  platformio.ini      # board=rpipico2w, USB identity, build/flash notes
  include/config.h     # button pin map + LED pins + debounce/long/ping timing
  src/main.cpp         # debounce → SHORT/LONG, HELLO, PING, CFG/LED handling
```

Build & flash:

```bash
cd firmware/button_coproc/pico
pio run -t upload        # hold BOOTSEL on the first flash; it auto-resets after
pio device monitor       # watch the HELLO / BTN / PING stream (115200)
```

Edit **`include/config.h`** to set your switch GPIOs (the button **id** is the
index in `kButtonPins`), optional per-switch LED pins, and the debounce / long-
press / heartbeat timing. `src/main.cpp` handles the protocol; you usually don't
need to touch it. It also accepts `CFG long_ms=<n>` (retune the threshold live)
and `LED <id> <0|1>` (drive a switch backlight) from the Pi.

Other RP2040/RP2350 boards work too — swap `board` in `platformio.ini`
(`rpipico2`, `rpipicow`, `rpipico`).

## USB identity (stable serial path)

`platformio.ini` sets the USB manufacturer/product so udev exposes a stable
`/dev/serial/by-id/usb-ProtoHUD_Buttons-if00`-style path instead of a racing
`/dev/ttyACMn` (the Teensy / SmartKnob / RAK4631 also enumerate as `ACM*`).

After flashing, confirm the actual path and point the HUD config at it — the
board's unique serial may be appended, so use whatever appears:

```bash
ls -l /dev/serial/by-id/
# inputs.coprocessor.device = the matching ...ProtoHUD_Buttons...-if00 path
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
