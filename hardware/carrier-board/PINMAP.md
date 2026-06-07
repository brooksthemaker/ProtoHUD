# Carrier Board — master pin / net map

The single source of truth for GPIO allocation across the **two brains**:

- **CM5 (RP1 GPIO)** — drives **only HUB75** now. Everything else moved off.
- **RP2354B I/O coprocessor** — owns sensors, WS2812, buttons, MAX7219, servos.
  Its full allocation is in [`RP2354-IO.md`](RP2354-IO.md#pin-allocation-source-of-truth).

Schematic nets, buffer connections, and firmware config must all agree with
these tables. HUB75 values come from `src/main.cpp` `kHub75`.

> Neither chip exposes a physical 40-pin header — the carrier *creates* any
> header. These tables are logical (BCM / GPxx); assign physical connector pins
> during layout.

---

## CM5 (RP1) allocation — HUB75 only

All 14 HUB75 lines are **CM5 → panel** (unidirectional) through the 74AHCT245
face buffer to **J2**. Nothing else is wired to CM5 GPIO.

| BCM | HUB75 signal | On carrier |
|----:|--------------|-----------|
| 4 | OE | →74AHCT245→ J2 |
| 5 | R1 | →74AHCT245→ J2 |
| 6 | B1 | →74AHCT245→ J2 |
| 12 | R2 | →74AHCT245→ J2 |
| 13 | G1 | →74AHCT245→ J2 |
| 16 | G2 | →74AHCT245→ J2 |
| 17 | CLK | →74AHCT245→ J2 (33 Ω series) |
| 20 | D | →74AHCT245→ J2 |
| 21 | STB/LAT | →74AHCT245→ J2 (33 Ω series) |
| 22 | A | →74AHCT245→ J2 |
| 23 | B2 | →74AHCT245→ J2 |
| 24 | E | →74AHCT245→ J2 |
| 26 | B | →74AHCT245→ J2 |
| 27 | C | →74AHCT245→ J2 |

**Everything else on the CM5 is free** — there is no longer any I²C/SPI/WS2812/
button contention on the Pi, because those buses live on the RP2354B. The CM5's
other interfaces used by the carrier are its dedicated blocks, not GPIO:

| CM5 interface | Carrier use |
|---------------|-------------|
| 2× CSI (MIPI) | J7/J8 camera eyes |
| 2× HDMI | J10 / VITURE |
| USB 2.0 host | onboard hub → RP2354B (CDC), RP2350 audio, knob, LoRa, VITURE, cams |
| 3.3 V / 5 V / GND | rails (see [`POWER.md`](POWER.md)) |

> Spare CM5 GPIO (BCM 2/3, 7–11, 14/15, 18/19, 25, …) are now genuinely free —
> break a few to a debug header (R1.3) but they carry no required function.

---

## RP2354B allocation — peripheral I/O

Summary (full table + nets in [`RP2354-IO.md`](RP2354-IO.md)):

| Block | GPIO | Net(s) | Shift |
|-------|------|--------|-------|
| I²C0 sensors | GP4/GP5 (+GP6 INT) | `SDA0` `SCL0` `SENS_INT` | 3.3 V direct |
| MAX7219 (SPI0) | GP2/GP3 + GP7–10 | `MX_CLK` `MX_DIN` `MX_CS1..4` | →5 V (74AHCT245) |
| WS2812 ×4 (PIO) | GP16–GP19 | `LED1..4_DAT` | →5 V (74AHCT125) |
| Servos ×8 (PWM) | GP20–GP27 | `SRV1..8` | 3.3 V direct |
| Buttons/boop | GP28–GP37 | `BTN1..10` | 3.3 V direct |
| ADC / spare | GP38–GP47 | `AIN0..7` | 3.3 V |
| USB CDC | DP/DM | `RP_DP/DM` → selector | — |

---

## Bus / domain summary

| Bus | Brain | Pins | Role |
|-----|-------|------|------|
| HUB75 (14) | CM5 | BCM 4–27 (see above) | face panels via '245 → J2 |
| I²C0 sensors | RP2354B | GP4/GP5 | BNO055 0x28, MPU9250 0x68, MPR121 0x5A, BH1750 0x23 |
| SPI0 MAX7219 | RP2354B | GP2/GP3 + CS | alt/secondary face matrix via '245 → J3 |
| WS2812 | RP2354B | GP16–19 (PIO) | accessory LEDs via '125 → J4 |
| Servo PWM | RP2354B | GP20–27 | 8 servo headers, `+V_SERVO` rail |
| GPIO buttons | RP2354B | GP28–37 | J6 |
| USB | CM5↔hub↔RP2354B | — | CDC link + USB peripheral stack |

## Contention rules (much simpler now)

1. **CM5 = HUB75 only.** Don't hang buttons/LEDs/sensors on CM5 GPIO; they
   belong on the RP2354B. The face buffer ('245) is the only CM5 GPIO consumer.
2. **HUB75 + MAX7219 may run simultaneously** (different brains) — see
   [`MULTI-BACKEND.md`](MULTI-BACKEND.md).
3. **5 V-logic loads shift on the RP2354B side** (MAX7219, WS2812). Servos take
   3.3 V signal directly.
4. **I²C address hygiene** — sensors at 0x23/0x28/0x5A/0x68; reserve 0x20–0x22,
   0x40–0x4B for expanders (see [`IO-EXPANSION.md`](IO-EXPANSION.md)).
5. **One USB owner of the RP2354B pair** — CM5 hub *or* the standalone program
   port, via the SW1 selector (see [`RP2354-IO.md`](RP2354-IO.md#usb-selector-cm5-hub--standalone-port)).
