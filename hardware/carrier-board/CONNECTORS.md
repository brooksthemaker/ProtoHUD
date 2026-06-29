# Carrier Board — connector pinouts

Pin-by-pin for every connector in the [block diagram](BLOCK-DIAGRAM.md) and
[connector schedule](BLOCK-DIAGRAM.md#connector-schedule). Signal sources/nets
come from [`PINMAP.md`](PINMAP.md) and [`RP2354-IO.md`](RP2354-IO.md). This is a
**two-brain** board: the **CM5 drives only HUB75 (J2)**; the **RP2354B I/O
coprocessor owns everything else** and links to the CM5 over USB-CDC.

**Domain** = the logic/voltage a pin sits at:

- `5V-buf` — level-shifted 5 V output (HUB75 face '245; RP2354B-side U10/U11)
- `3V3-RP` — RP2354B native 3.3 V (sensors, buttons, ADC, servo signal)
- `PWR` — a power rail (`+5V`, `+5V_PANEL`, `+5V_LED`, `+V_SERVO`, `+3V3_RP`)
- a bus standard (MIPI, USB 2.0, HDMI)

Legend: **Dir** is relative to **whichever brain owns the connector** (noted per
connector) — `OUT` = brain→peripheral, `IN` = peripheral→brain, `BIDIR`, `PWR`.

> **JP1 (old face-backend select) is gone.** HUB75 (CM5) and MAX7219 (RP2354B)
> now live on **different brains** with independent buffers and run
> simultaneously — there is no single buffer to route, so nothing to select.
> The old JP2 (MAX7219 source select) is gone too: SPI0 on the RP2354B is the
> only MAX7219 source. See [`MULTI-BACKEND.md`](MULTI-BACKEND.md).

---

## J1 — Main 5 V input  ·  *PWR*

| Pin | Signal | Dir | Notes |
|----:|--------|-----|-------|
| 1 | +5V | PWR | After fuse → reverse-polarity FET → TVS (REQ R2.1). Feeds `+5V` bus and the local `+3V3_RP` LDO/buck |
| 2 | GND | PWR | |

> `+5V_PANEL` (HUB75), `+5V_LED` (WS2812), and `+V_SERVO` (servos) are separate,
> individually fused rails (REQ R2.3/R2.4) — inject panel/LED/servo power at
> their own headers, **not** through the CM5 rail. Common ground. `+3V3_RP` is a
> local rail off `+5V` that powers the RP2354B, the I²C sensors, and the 3.3 V
> (A-side) of the U10/U11 buffers. See [`POWER.md`](POWER.md).

---

## J2 — HUB75 face panel  ·  2×8 IDC (HUB75E standard)  ·  *5V-buf*  ·  brain: **CM5**

All signals are **CM5 → panel** through the **74AHCT245** face buffer (U1/U2).
The BCM column is the CM5-side source ([`PINMAP.md`](PINMAP.md)); the panel sees
the buffered 5 V copy. **Pinout unchanged.** Dir is CM5-relative.

| Pin | Signal | BCM (src) | Pin | Signal | BCM (src) |
|----:|--------|----------:|----:|--------|----------:|
| 1 | R1 | 5 | 2 | G1 | 13 |
| 3 | B1 | 6 | 4 | GND | — |
| 5 | R2 | 12 | 6 | G2 | 16 |
| 7 | B2 | 23 | 8 | E | 24 |
| 9 | A | 22 | 10 | B | 26 |
| 11 | C | 27 | 12 | D | 20 |
| 13 | CLK | 17 | 14 | LAT/STB | 21 |
| 15 | OE | 4 | 16 | GND | — |

> Pin 8 is **E** on 1/32-scan (64-row) panels and **GND** on 1/16-scan — match
> your panel. Keep buffer→connector traces short; optional 33 Ω series on CLK
> (REQ R3.3). Panel +5 V is fed from the dedicated **`+5V_PANEL`** rail, not this
> header. This is the **only** CM5 GPIO consumer on the board.

---

## J3 — MAX7219 chain header  ·  *5V-buf*  ·  brain: **RP2354B**

DIN/CLK/CS are **RP2354B → driver** through **U10 (74AHCT245)**. The single
source is the RP2354B SPI0 (no jumper/bit-bang/source-select — the old JP2 is
gone). Dir is RP2354B-relative.

| Pin | Signal | GPIO (src) | Net | Dir | Notes |
|----:|--------|-----------:|-----|-----|-------|
| 1 | +5V | — | `+5V_PANEL` | PWR | panel rail, fused |
| 2 | GND | — | — | PWR | |
| 3 | DIN | GP3 | `MX_DIN` | OUT | SPI0 TX, buffered to 5 V |
| 4 | CLK | GP2 | `MX_CLK` | OUT | SPI0 SCK, buffered to 5 V |
| 5 | CS1 | GP7 | `MX_CS1` | OUT | chain 1 |
| 6 | CS2 | GP8 | `MX_CS2` | OUT | chain 2 |
| 7 | CS3 | GP9 | `MX_CS3` | OUT | chain 3 |
| 8 | CS4 | GP10 | `MX_CS4` | OUT | chain 4 |

> MAX7219 VCC = 5 V (input-high ≈ 3.5 V), so DIN/CLK/CS **must** be buffered to
> 5 V — all are MCU → driver (unidirectional). DOUT daisy-chains module→module
> and never returns to the MCU, so no down-shift is needed. No SPI0 contention
> with WS2812 — that bus is on PIO now.

---

## J4 — WS2812 accessory LEDs (×4 zones)  ·  *5V-buf*  ·  brain: **RP2354B**

Four independent addressable zones, each a `LEDn_DAT` line from the RP2354B PIO,
buffered to 5 V through **U11 (74AHCT125)**. One header carries all four data
lines plus the shared fused `+5V_LED` rail and GND (or break out as 4× 3-pin
headers — one per zone). Dir is RP2354B-relative.

| Pin | Signal | GPIO (src) | Net | Dir | Notes |
|----:|--------|-----------:|-----|-----|-------|
| 1 | +5V | — | `+5V_LED` | PWR | WS2812 rail, fused (REQ R2.4) |
| 2 | GND | — | — | PWR | |
| 3 | DIN1 | GP16 | `LED1_DAT` | OUT | zone 1, through U11 → 5 V |
| 4 | DIN2 | GP17 | `LED2_DAT` | OUT | zone 2, through U11 → 5 V |
| 5 | DIN3 | GP18 | `LED3_DAT` | OUT | zone 3, through U11 → 5 V |
| 6 | DIN4 | GP19 | `LED4_DAT` | OUT | zone 4, through U11 → 5 V |

> WS2812 timing comes from the RP2350 **PIO** (not SPI). Add a large bulk cap at
> the `+5V_LED` rail and a series resistor on each DIN. If split into per-zone
> 3-pin headers, repeat `+5V_LED`/GND on each.

---

## J5 — I²C0 sensors  ·  *3V3-RP*  ·  brain: **RP2354B**

Qwiic/STEMMA QT-style pin order so 1 mm JST-SH sensors plug straight in, plus a
sensor-interrupt pin. Dir is RP2354B-relative.

| Pin | Signal | GPIO | Net | Dir | Notes |
|----:|--------|-----:|-----|-----|-------|
| 1 | GND | — | — | PWR | |
| 2 | +3V3 | — | `+3V3_RP` | PWR | from the local `+3V3_RP` rail (**not** the CM5 3V3) |
| 3 | SDA | GP4 | `SDA0` | BIDIR | 4.7 kΩ pull-up to `+3V3_RP` |
| 4 | SCL | GP5 | `SCL0` | BIDIR | 4.7 kΩ pull-up to `+3V3_RP` |
| 5 | INT | GP6 | `SENS_INT` | IN | MPR121 / IMU interrupt to MCU |

> Shared by BNO055 0x28, MPU9250 0x68, MPR121 0x5A, BH1750 0x23, and any
> MCP23017 expanders (0x20–0x22). See [`IO-EXPANSION.md`](IO-EXPANSION.md).

---

## J6 — buttons / boop  ·  *3V3-RP*  ·  brain: **RP2354B**

Direct RP2354B inputs with internal pull-ups; debounce/long-press run on the
MCU. Ten button lines `BTN1..10` (GP28–GP37). Dir is RP2354B-relative.

| Pin | Signal | GPIO | Net | Dir | Notes |
|----:|--------|-----:|-----|-----|-------|
| 1 | +3V3 | — | `+3V3_RP` | PWR | |
| 2 | GND | — | — | PWR | |
| 3 | BTN1 | GP28 | `BTN1` | IN | wire switch to GND (`active_low`) |
| 4 | BTN2 | GP29 | `BTN2` | IN | |
| 5 | BTN3 | GP30 | `BTN3` | IN | |
| 6 | BTN4 | GP31 | `BTN4` | IN | |
| 7 | BTN5 | GP32 | `BTN5` | IN | |
| 8 | BTN6 | GP33 | `BTN6` | IN | |
| 9 | BTN7 | GP34 | `BTN7` | IN | |
| 10 | BTN8 | GP35 | `BTN8` | IN | |
| 11 | BTN9 | GP36 | `BTN9` | IN | |
| 12 | BTN10 | GP37 | `BTN10` | IN | boop / capacitive trigger |

> Switches tie to GND; enable internal pull-ups in firmware (`active_low`).
> Optional series R + ESD array (REQ R6.1). For more inputs use an I²C expander
> on J5 — it costs no MCU GPIO. Analog inputs (`AIN0..7`, GP40–GP47) break out
> separately as needed.

---

## J7 / J8 — CSI cameras  ·  22-pin 0.5 mm FFC (Pi 5 / CM5 standard)  ·  brain: **CM5**

Two CSI eyes (`csi_expected: 2`). **Unchanged.** The 22-pin flat-flex follows
the **Raspberry Pi 5 / CM5 camera standard** — wire per the CM5 datasheet's CAM
lane mapping rather than a hand-typed table (lane numbering is easy to get
wrong). Functional groups carried:

| Group | Signals | Notes |
|-------|---------|-------|
| MIPI data | 2× differential pairs (D0±, D1±) | per CSI lane |
| MIPI clock | 1× differential pair (CK±) | |
| Control I²C | SCL/SDA | sensor config (separate from sensor bus J5) |
| Power | 3V3 (+ enable/regulator) | per camera |
| GPIO | shutdown / LED | optional |

> Use a flip-lock 0.5 mm connector; keep the differential pairs length-matched
> and short. Confirm exact pin numbers against the CM5 datasheet during layout.

---

## U7 / J40–J43 — USB 3.1 hub + downstream USB-C  ·  USB 3.1 Gen1 (5 Gbps)  ·  brain: **CM5**

The on-board hub is a **VIA VL817** 4-port **USB 3.1 Gen1** hub (replaces the old
USB 2.0 USB2514B). Its **upstream** is one CM5 **USB 3.0** port (SuperSpeed +
USB 2.0); its **four downstream ports are USB-C receptacles J40–J43**, each
SuperSpeed-capable and USB 2.0 backward-compatible. The peripheral stack
(RP2350 audio, smart knob, LoRa, VITURE, USB cams) plugs into these.

**Upstream link (CM5 USB3 port #0 ⇄ U7):** USB 2.0 pair `CM5_USB_DP/DM` +
SuperSpeed `CM5_SSTX_±` (CM5→hub) and `CM5_SSRX_±` (hub→CM5). AC-coupling caps
sit at each transmitter (CM5 side for SSTX, hub side for SSRX).

**Each downstream USB-C port (host / DFP) — key pads:**

| Pad | Signal | Net | Notes |
|----:|--------|-----|-------|
| A4/B4/A9/B9 | VBUS | `Pn_VBUS` | from `+5V` via per-port PTC (F4–F7), ~1 A |
| A1/B1/A12/B12 | GND | GND | |
| A5 / B5 | CC1 / CC2 | `Pn_CC1/2` | **Rp pull-ups 56 kΩ → 5 V** (host advertises default current) |
| A6/B6 · A7/B7 | D+ / D− | `Pn_DP/DM` | USB 2.0; bond A&B sides at the footprint for flip; 90 Ω diff |
| A2/A3 | SSTX1 ± | `Pn_TXP/M` | hub TX → port (via AC caps); 90 Ω diff, 85 Ω SS pair |
| B11/B10 | SSRX1 ± | `Pn_RXP/M` | port → hub RX |

> **SS orientation:** one SuperSpeed lane (TX1/RX1) is wired, so **USB 3.x works
> in one USB-C orientation**; USB 2.0 works either way. For full SS flip add a
> per-port 2:1 SS mux (optional/DNP). ESD: USBLC6-2 on D+/D-, SS TVS array on the
> SS pair.

> **RP2354B is no longer behind this hub.** It now has its **own** CM5 USB host
> port (the CM5's 2nd USB 3.0 port, USB 2.0 lanes) via **SW1 position A** — see
> SW1 / J12. This frees all four hub ports for external USB-C.

---

## J10 — HDMI out  ·  HDMI standard (×2)  ·  brain: **CM5**

**Unchanged.** CM5 exposes two HDMI outputs. Use standard HDMI receptacles with
an **ESD protection array** on the TMDS pairs (REQ N10). Pinout is the HDMI
standard — TMDS data 0/1/2 + clock (differential pairs), DDC SCL/SDA, CEC, HPD,
+5 V. Wire per the connector datasheet; route TMDS pairs length-matched with
ground adjacency. Drives VITURE / external display.

---

## J11 — Backpack USB umbilical (phone → CM5 host)  ·  USB 2.0  ·  brain: **CM5**

**Unchanged.** The phone (docked in the backpack) connects to a **CM5 USB host**
port via the tether; ProtoHUD mirrors it with scrcpy/ADB. Dedicate one CM5 port
to this so the long run doesn't share the in-helmet hub (J9).

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | VBUS (+5V) | passive dock: CM5 powers/charges phone. Charge-injection dock: **leave VBUS isolated** from CM5 |
| 2 | D− | shielded twisted pair; keep clear of power conductors |
| 3 | D+ | |
| 4 | GND | tie to system ground |
| — | SHIELD | cable shield → chassis/ground at one end |

> Pairs with the 5 V power feed (J1) as the **backpack→helmet umbilical**. They
> can share one multi-pin circular connector (GX16/GX20 / push-pull) — if so,
> segregate the USB pair from the high-current 5 V pins. See
> [`POWER.md`](POWER.md#umbilical-backpack--helmet).

---

## J12 — USB-C standalone program port  ·  USB 2.0  ·  brain: **RP2354B**

A dedicated USB-C port on the RP2354B's **single** USB pair (`RP_DP/RP_DM`),
reached when **SW1** selects standalone (position B). Flip to standalone → plug
into any computer → flash/`picotool`/UF2, with the CM5 fully out of the loop.
Dir is RP2354B-relative.

| Pin | Signal | Net | Dir | Notes |
|----:|--------|-----|-----|-------|
| 1 | D+ | `RP_DP` | BIDIR | via SW1 (mux/switch); shared with the dedicated CM5-port path |
| 2 | D− | `RP_DM` | BIDIR | via SW1; 90 Ω differential |
| 3 | VBUS (+5V) | — | — | **host-present sense only — isolated.** Do **not** back-feed onto carrier/hub |
| 4 | GND | — | PWR | |

> The RP2354B is self-powered from **`+3V3_RP`**, so J12's VBUS is used only to
> detect a connected host — steer/diode-OR or leave as VBUS-sense. CC pull-downs
> (5.1 kΩ) present the port as a UFP. See
> [`RP2354-IO.md`](RP2354-IO.md#usb-selector-cm5-hub--standalone-port).

---

## J20–J27 — Servo headers ×8  ·  3-pin  ·  *3V3-RP sig / `+V_SERVO` pwr*  ·  brain: **RP2354B**

Eight standard 3-pin servo connectors (24 positions). Each carries one unique
signal `SRVn` (GP20–GP27, PWM, **3.3 V direct — no buffer**); V+ and GND are the
shared, fused, bulk-capped `+V_SERVO` rail. Dir is RP2354B-relative.

| Pin | Signal | Net | Dir | Notes |
|----:|--------|-----|-----|-------|
| 1 | SIG | `SRVn` | OUT | PWM from GPx, 3.3 V signal (no level shift) |
| 2 | +V | `+V_SERVO` | PWR | 5–6 V servo rail, fused (servo power only) |
| 3 | GND | — | PWR | |

| Header | Net | GPIO |
|--------|-----|-----:|
| J20 | `SRV1` | GP20 |
| J21 | `SRV2` | GP21 |
| J22 | `SRV3` | GP22 |
| J23 | `SRV4` | GP23 |
| J24 | `SRV5` | GP24 |
| J25 | `SRV6` | GP25 |
| J26 | `SRV7` | GP26 |
| J27 | `SRV8` | GP27 |

> Servo signal levels are within spec at 3.3 V, so no shifter is fitted. The
> `+V_SERVO` rail is its own fused, bulk-capped domain so stall surges don't
> disturb logic (see [`POWER.md`](POWER.md)). Keep header order = SIG/+V/GND or
> match your servo brand's pinout.

---

## SW1 — USB selector (CM5 hub ⇄ standalone port)  ·  brain: **RP2354B**

The RP2350 has **one** USB controller, so the hub link and the standalone port
are mutually exclusive and share the `RP_DP/RP_DM` pair through SW1.

| Position | Routes RP2354B D+/D− → | Use |
|----------|------------------------|-----|
| A (default) | onboard hub (J9) → CM5 | in-system: USB-CDC link + in-place reflash from the CM5 |
| B | J12 USB-C program port | standalone flash from any computer, CM5 out of the loop |

> SW1 = a **DPDT slide switch** on D+/D− (fine for the RP2350's full-speed USB)
> **or** a `TS3USB221A` analog mux for cleaner switching. VBUS stays isolated per
> J12. See [`RP2354-IO.md`](RP2354-IO.md#usb-selector-cm5-hub--standalone-port).

---

## Debug / programming headers  ·  brain: **RP2354B**

### SWD — debug / flash probe header  ·  *3V3-RP*

Standard SWD for a debug probe (live debug + flashing) plus a **BOOTSEL** button
and a **RUN/reset** button on the board.

| Pin | Signal | Net | Dir | Notes |
|----:|--------|-----|-----|-------|
| 1 | SWCLK | — | IN | SWD clock from probe |
| 2 | SWDIO | — | BIDIR | SWD data |
| 3 | GND | — | PWR | |
| 4 | +3V3 | `+3V3_RP` | PWR | target reference (do not power target from probe) |

> BOOTSEL (hold on reset) enters the UF2 bootloader over whichever USB path SW1
> selects. RUN/reset restarts the MCU.

### DBG — UART debug console  ·  *3V3-RP*

RP2354B UART0 console for logs/serial debug, separate from the USB-CDC link.

| Pin | Signal | GPIO | Net | Dir | Notes |
|----:|--------|-----:|-----|-----|-------|
| 1 | TX | GP0 | `DBG_TX` | OUT | MCU → host console |
| 2 | RX | GP1 | `DBG_RX` | IN | host → MCU |
| 3 | GND | — | — | PWR | common ground with the serial adapter |

> 3.3 V levels — use a 3.3 V USB-UART adapter; do **not** connect a 5 V TTL
> adapter directly to GP0/GP1.

---

## Cooling fan headers  ·  up to 4 fans in 2 zones  ·  brain: **CM5**

**Fans are CM5-local** — driven directly by the existing `sys::FanController`
software PWM on spare CM5 GPIO (**BCM 18 = Zone 1**, **BCM 19 = Zone 2**). This is
the deliberate choice over RP2354B: the CM5 keeps cooling itself even if the
MCU/USB link hangs, and it reuses firmware that already exists. (These two
control lines are the only non-HUB75 CM5 GPIO besides `RP_RUN`/`RP_BOOTSEL`.)

Two zones, each switched by a **low-side N-MOSFET** whose gate is the zone's CM5
GPIO; up to two fan connectors per zone share the zone speed.

Per fan connector (4-pin PC-fan compatible):

| Pin | Signal | Notes |
|----:|--------|-------|
| 1 | GND (switched) | fan return through the zone MOSFET drain |
| 2 | +V (fan rail) | 5 V or 12 V depending on fan; **not** the CM5 rail |
| 3 | PWM/ctrl | tie to GND-switching for 2-pin fans; or drive a 4-pin fan's control line from the zone GPIO (add a buffer if the fan needs 5 V logic) |
| 4 | TACH | optional RPM sense back to a spare GPIO |

| Zone | Default GPIO | Fan headers |
|------|--------------|-------------|
| Zone 1 (e.g. Intake) | BCM 18 | 1–2 |
| Zone 2 (e.g. Exhaust) | BCM 19 | 1–2 |

> Each zone needs one MOSFET (gate ← GPIO, with a gate resistor + pulldown) and a
> flyback diode across each fan. Software PWM ≈100 Hz suits MOSFET-switched 2-pin
> fans; for quiet 4-pin control use a 25 kHz hardware-PWM pin/overlay later.

---

## Expansion headers

### JX1 — I²C0 expansion / STEMMA QT (Qwiic)  ·  *3V3-RP*  ·  brain: **RP2354B**
Solderless drop-in for a 2nd MCP23017, ADS1115, or PCA9685 (REQ N15). Same
electrical net as J5 (`SDA0`/`SCL0`, GP4/GP5); Qwiic pin order:

| Pin | Signal | Net | Notes |
|----:|--------|-----|-------|
| 1 | GND | — | |
| 2 | +3V3 | `+3V3_RP` | from `+3V3_RP` |
| 3 | SDA | `SDA0` (GP4) | |
| 4 | SCL | `SCL0` (GP5) | |

### JX2 — Expander interrupts  ·  *3V3-RP*  ·  brain: **RP2354B**
INT lines from I²C expanders → the RP2354B for interrupt-driven reads. Shares
the sensor INT net or a spare GPIO (GP11–GP15 / GP38–GP39).

| Pin | Signal | GPIO/Net | Notes |
|----:|--------|----------|-------|
| 1 | INT0 | `SENS_INT` (GP6) | primary expander INTA (shared with sensor INT — pick one use) |
| 2 | INT1 | GP11 (spare) | 2nd expander |
| 3 | GND | — | |

> If both an expander and a sensor need a hardware INT, assign INT1 to a spare
> (GP11–GP15) rather than sharing `SENS_INT`.

### SPI0 lane (optional second expansion path)  ·  brain: **RP2354B**
If I²C fills, MCP23S17 / 74HC165 / 74HC595 can ride the RP2354B **SPI0**
(`MX_CLK`/`MX_DIN` GP2/GP3 + a spare CS). The MAX7219 already owns SPI0 with four
CS lines; add a CS on a spare GPIO (GP11–GP15) and share the bus — no contention
beyond CS arbitration.

---

## Keying & orientation (layout reminders)

- HUB75 (J2): boxed/shrouded 2×8 with a key slot; pin 1 silk triangle.
- Power (J1) and LED/MAX7219 (J3/J4): keyed, polarized connectors — reversing
  5 V/GND is destructive.
- Servos (J20–J27): mind the 3-pin SIG/+V/GND order; `+V_SERVO` is 5–6 V — a
  servo plugged backwards can be destroyed.
- I²C/Qwiic (J5/JX1): use the JST-SH 1 mm Qwiic standard so off-the-shelf
  cables work.
- USB-C (J12): position the BOOTSEL/RUN buttons and SW1 selector for access; mark
  SW1 positions A (hub) / B (standalone) on the silk.
- Label every connector with its **J-number and the PINMAP / RP2354-IO net
  names** so the silk matches the firmware's GPIO visualizer.
