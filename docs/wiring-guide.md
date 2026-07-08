# ProtoHUD Wiring Guide

Every physical connection in the target architecture, board by board. The rule
that shapes all of it: **the CM5's 40-pin header carries only the HUB75 bonnet
and the head-tracking IMU** — every other peripheral wires to the RP2350
coprocessor, which talks to the CM5 over one USB cable.

Pin numbers below match the firmware defaults in
`firmware/button_coproc/pico/include/config.h`. Button/LED pins are runtime-
remappable from the HUD (GPIO → RP2350 GPIO Expander → Pins); the
subsystem pins (voice, MAX7219, 1-Wire, fans, I2C) are compile-time.

---

## 1. The trunk

| From | To | Cable |
|---|---|---|
| RP2350 board USB | CM5 carrier USB-A / hub | one USB cable (data, and it powers the Pico) |

Everything crosses here: button events up, pin maps / voice control / MAX7219
frames / fan duties down, plus firmware flashing. The coprocessor enumerates as
`/dev/serial/by-id/usb-ProtoHUD_Buttons…` — always reference it by that path,
never a bare `ttyACMn`.

---

## 2. CM5 wiring

### HUB75 face panels
Plug the **Adafruit RGB Matrix Bonnet** onto the 40-pin header; panels chain
from its HUB75 connector with their ribbon cables. Panel power comes from the
bonnet's 5 V screw terminals — budget ~4 A per 64×32 panel at full white.

### Head-tracking IMU (BNO086) — I2C1

| BNO086 | CM5 header pin | BCM |
|---|---|---|
| SDA | pin 3 | GPIO2 |
| SCL | pin 5 | GPIO3 |
| VIN | pin 1 | 3V3 |
| GND | pin 6 | — |
| INT (recommended) | any spare | config `bno086.int_line` |
| RST (optional) | any spare | config `bno086.rst_line` |

The VITURE glasses' built-in IMU needs no wiring (USB). With the peripheral
hub in use, **I2C1 belongs to the IMU alone** — no other devices on the bus.

### Legacy CM5 options (only if you're *not* using the coprocessor hub)
- DS18B20 probes: data → **GPIO25** (pin 22) with a 4.7 kΩ pull-up to 3V3, and
  `dtoverlay=w1-gpio,gpiopin=25` in `/boot/firmware/config.txt`.
- Fans: `fans.zones[].gpios` (e.g. BCM 18/19) through a MOSFET as in §3.8.

---

## 3. RP2350 coprocessor wiring

### 3.1 Master pin table (Pico 2 defaults)

| GP | Role | Fixed / remappable |
|---|---|---|
| GP2–GP9 | button switches ×8 | remappable (PINCFG) |
| GP10 | MAX7219 CLK (SPI1 SCK) | fixed |
| GP11 | MAX7219 DIN (SPI1 TX) | fixed |
| GP13 | MAX7219 CS/LOAD | fixed |
| GP14, GP15 | fan PWM zones 0/1 (25 kHz) | fixed |
| GP16 | TLV320 BCLK (I2S) | fixed |
| GP17 | TLV320 WSEL (I2S WS) | fixed |
| GP18 | TLV320 DIN (I2S data) | fixed |
| GP19 | 1-Wire bus (DS18B20) | fixed |
| GP20 | I2C0 SDA — TLV320 + MPR121 | fixed |
| GP21 | I2C0 SCL — TLV320 + MPR121 | fixed |
| GP22 | TLV320 reset | fixed |
| GP26 | mic in (ADC0) | fixed |
| GP0, GP1, GP12, GP27, GP28 | **free** | — |

On an **RP2350B board** (Pimoroni Pico Plus 2 / Pico LiPo 2 XL W) GP30–GP47
are also available (ADC moves to GP40–47 — retarget `kMicAdcPin`), minus each
board's reserved pins; the in-HUD pin editor's Board picker flags them.

### 3.2 Switches (buttons)

```
GP n ────/ switch ────► GND        (INPUT_PULLUP: pressed = LOW)
```
Optional backlight per button: `GP → 220 Ω → LED → GND` and set the slot's LED
pin in the pin editor. Long harnesses: twist the pair, keep returns to a
common ground point.

### 3.3 Voice in — MAX9814 mic

| MAX9814 | RP2350 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| OUT | **GP26** (ADC0) |
| GAIN | float = 60 dB · VDD = 40 dB · GND = 50 dB |

### 3.4 Voice out — TLV320DAC3100 + speaker

| TLV320 | RP2350 | Notes |
|---|---|---|
| BCLK | GP16 | I2S bit clock (PLL source — no MCLK wire) |
| WSEL | GP17 | I2S word select |
| DIN | GP18 | I2S data |
| SDA / SCL | GP20 / GP21 | control, address 0x18 |
| RST | GP22 | active-low reset |
| IOVDD / AVDD | 3V3 | logic + analog |
| SPKVDD | **5 V (VBUS)** | class-D rail — add 220 µF at the pin |
| SPK+ / SPK− | speaker | mono, ~1.3 W into 8 Ω |
| HPL / HPR | (optional) | line out → MAX98306 for loud stereo |

### 3.5 MAX7219 matrices

| Signal | RP2350 | Chain wiring |
|---|---|---|
| CLK | GP10 | bussed to every module |
| DIN | GP11 | first module only; each DOUT → next DIN |
| CS/LOAD | GP13 | bussed to every module |
| VCC | **5 V** | ~250–330 mA per 8×8 at full brightness — budget the rail |
| GND | GND | common with the Pico |

Wire the physical daisy-chain in the same DIN→DOUT order the in-HUD wiring
diagram shows (Face Display → MAX7219 Layout). *Honest note:* at 5 V supply the
MAX7219's V_IH spec is above 3.3 V logic; it almost always works, but if a
chain glitches add a 74HCT125/245 level shifter on CLK/DIN/CS.

### 3.6 Boop pads — MPR121

| MPR121 | RP2350 |
|---|---|
| SDA / SCL | GP20 / GP21 (shared bus — 0x5A sits beside the DAC's 0x18) |
| VDD | 3V3 |
| GND | GND |
| ADDR | leave to GND = 0x5A |
| E0…E11 | conductive pads (snout / cheeks) |

Zero extra GPIOs — it rides the existing I2C0 wires. Map electrodes → zones in
`boop.zones[].electrode` (same config as a CM5-local MPR121). Keep electrode
wires short; they're capacitive antennas.

### 3.6b Boop pads — TTP223 touch modules (preferred, up to 6)

Each TTP223 module is a self-contained capacitive touch switch with a plain
digital output — no shared I2C bus, no electrode tuning, and each pad can sit
right where it's mounted with only 3 wires. The firmware pre-assigns 6 inputs:

| Pad idx | RP2350 | Notes |
|--------:|--------|-------|
| 0 | GP0 | |
| 1 | GP1 | |
| 2 | GP12 | |
| 3 | GP16 | shared with optional voice-changer I2S — voice build: pads 0–2 only |
| 4 | GP17 | " |
| 5 | GP18 | " |

Per module: VCC → 3V3, GND → GND, I/O (OUT) → the listed GP. Stock modules are
**active-high momentary**; if you solder the A/B jumpers for inverted/latching
output, flip `kTouchActiveHigh` in `include/config.h`.

Touch edges stream up as `BOOP <idx> <1|0>` — the SAME verb as MPR121
electrodes — so the Pi config works two ways (both can apply to one pad):

- **Boop zones**: set `boop.zones[].electrode` to the pad INDEX (0–5) and the
  pad behaves exactly like an MPR121 electrode (snout/cheek zones, coalescing,
  reactions).
- **Extra buttons**: map any pad to any function in
  `inputs.coprocessor.touch`, e.g. `{ "3": "face_next", "4": "effect_next" }`
  — fired on touch-down.

### 3.7 Pre-assigned TEST pins (planned peripherals)

Fixed pins for bring-up of the planned carrier-board peripherals, exercised
from **GPIO → RP2350 GPIO Expander → Peripheral Test**:

| Feature | Pins | Verb | Notes |
|---|---|---|---|
| Servos ×4 | GP6–GP9 | `SERVO <ch> <deg\|off>` | shared with buttons 4–7: the slot becomes a servo on its first command (until reboot). Servo V+ from an external 5–6 V rail, grounds common — never the Pico's 3V3. |
| WS2812 zone | GP22 | `LEDZ <r> <g> <b> [n]` | level-shift for long/strict strips; default length in `config.h` (`kLedZoneCount`) |
| ADC ×3 | GP26–GP28 | `ADCREAD` → `ADC <ch> <raw> <mv>` | flex sensors / pots / battery divider; GP26 doubles as the voice mic |

### 3.7 Temperature probes — DS18B20

```
3V3 ──┬───────────────► every probe VDD (red)
      └── 4.7 kΩ ──┐
GP19 ──────────────┴──► every probe DATA (yellow)   (one pull-up for the bus)
GND ───────────────────► every probe GND (black)
```
All probes parallel on the same three wires; each is auto-discovered by ROM id
(the id appears in the coprocessor's `TEMP` lines and the HUD). Powered mode as
shown — avoid parasite power.

### 3.8 Fans

2-pin fans through a logic-level MOSFET per zone:

```
GP14/GP15 ── 100 Ω ──► gate (AO3400 / IRLZ44N)   + 10 kΩ gate→GND
fan− ──► drain      source ──► GND
fan+ ──► 5 V / 12 V supply     flyback diode (1N4007) across fan, stripe to +
```
4-pin PC fans: connect the PWM wire straight to GP14/15 (the 25 kHz frequency
matches the Intel spec), tach unused, fan powered from its own rail.

### 3.9 Power & grounds

- Pico logic and 3V3 peripherals: the board's **3V3(OUT)**.
- Amps and LED matrices: **5 V from VBUS** (USB-powered from the CM5) — watch
  the total budget; a big MAX chain plus the speaker may want its own 5 V rail.
- **Common ground everywhere**: CM5, Pico, every supply, every peripheral.
