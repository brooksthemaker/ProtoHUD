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
HELLO proto-buttons v1 fw=<ver> n=<count>   # once on boot
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

## Optional: real-time voice changer (core1)

The same Pico can run a real-time **voice changer** on its second core (mic →
effect → speaker via a TLV320DAC3100), leaving the button/protocol loop on core0
untouched. Build the `rpipico2w_voice` env and see
[`docs/voice-changer.md`](../../docs/voice-changer.md) for wiring, bring-up, and
the control protocol (`VOICE` / `FX` / `PITCH` / `MIX` / `PARAM`).

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

## Board compatibility — Pimoroni Pico LiPo 2 XL W (verified)

Cross-checked against Pimoroni's official board definition
(`pico-lipo` repo, `boards/pimoroni_pico_lipo2xl_w.h`) — the RP2350B pin map
in `include/config.h` has **no collisions** with the board's internal wiring:

| Board reservation (XL W) | Pins | Firmware use of those pins |
|---|---|---|
| RM2 radio (REG_ON / DATA / CS / CLK) | GP23, GP24, GP25, GP29 | none ✓ |
| BOOT button (active low) | GP30 | avoided by design ✓ |
| VBAT sense (cuttable "BtS Cut") | GP43 | avoided (`kAdcPins` stop at GP42) ✓ |
| PSRAM chip select (cuttable "PSRAM Cut") | GP47 | none ✓ |
| User LED / VBUS sense | RM2 WL_GPIO0 / WL_GPIO2 | not bank-0, n/a ✓ |

So the full RP2350B map carries over as-is: **buttons GP2–9**, **all six
TTP223 touch pads (GP0/1/12 + GP31/32/33)**, MPR121 + DAC on I2C0 GP20/21,
MAX bridge GP10/11/13, fans GP14/15, voice I2S GP16–18, 1-Wire GP19,
LED zone GP22(/28), mic + ADC GP40–42.

Two shared-connector nuances (shared, not reserved — fine unless you plug
something into that connector):

- **Qw/ST connector = GP4/GP5**, which the default map uses as buttons 3–4.
  To hang the MPR121 off the handy Qw/ST socket instead of wiring GP20/21,
  move I2C0 to GP4/5 in `config.h` (both pairs are valid I2C0) and remap
  those two buttons to spare pins via `PINCFG` — no firmware logic changes.
- **SP/CE connector = GP32–36**; touch pads 4–5 sit on GP32/33. That collides
  only if SP/CE is in use — and conveniently, the SP/CE socket can serve as
  the physical connector *for* those two pads.

On a **regular RP2350A** (Pico 2 / Pico 2 W): buttons GP2–9 and MPR121 carry
over unchanged; touch pads 0–2 (GP0/1/12) always work; pads 3–5 fall back to
GP16/17/18, which the optional voice changer's I2S also wants — with voice
enabled wire pads 0–2 only (already enforced in `config.h`). The Pico 2 W's
radio also lives on GP23/24/25/29, so no conflict there either.

PlatformIO note: `board = pimoroni_pico_plus_2w` (the closest def) is
functionally correct for the XL W — same radio pins, PSRAM CS (GP47) and
16 MB flash; only its USER_SW define differs (GP45 vs the XL W's GP30
BOOT), and this firmware uses neither.
