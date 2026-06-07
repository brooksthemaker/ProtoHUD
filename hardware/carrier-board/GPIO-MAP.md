# Carrier — full GPIO reference (CM5 + RP2354B)

Quick-reference pinout for **both brains**, showing each line's **ProtoHUD use
(main)** and its **alternate/peripheral functions (auxiliary)**. This is the
human-readable companion to [`PINMAP.md`](PINMAP.md) (the allocation source of
truth) and [`RP2354-IO.md`](RP2354-IO.md).

How to read it: **"ProtoHUD use"** = what the carrier wires the pin to *now*;
the remaining columns = what else that pin *could* be (the chip's function mux).
Empty "ProtoHUD use" = currently spare.

---

## CM5 (Raspberry Pi RP1) — BCM 0–27

The CM5 drives **only HUB75** (14 lines) + two recommended RP2354B control
lines. Everything else is spare. Alt-function labels follow the project's
`src/sys/gpio_pinmap.h` and the Pi 5/RP1 pinout.

| BCM | ProtoHUD use (main) | I²C | SPI | UART | PWM / other |
|----:|---------------------|-----|-----|------|-------------|
| 0 | *(reserved: HAT ID_SD)* | I2C0 SDA | | | |
| 1 | *(reserved: HAT ID_SC)* | I2C0 SCL | | | |
| 2 | spare | I2C1 SDA | | | |
| 3 | spare | I2C1 SCL | | | |
| 4 | **HUB75 OE** | | | | GPCLK0, 1-Wire |
| 5 | **HUB75 R1** | | | | GPCLK1 |
| 6 | **HUB75 B1** | | | | GPCLK2 |
| 7 | **`RP_RUN`** (RP2354B reset) | | SPI0 CE1 | | |
| 8 | **`RP_BOOTSEL`** (RP2354B boot) | | SPI0 CE0 | | |
| 9 | spare | | SPI0 MISO | | |
| 10 | spare | | SPI0 MOSI | | |
| 11 | spare | | SPI0 SCLK | | |
| 12 | **HUB75 R2** | | | | PWM0 |
| 13 | **HUB75 G1** | | | | PWM1 |
| 14 | spare | | | UART0 TXD | |
| 15 | spare | | | UART0 RXD | |
| 16 | **HUB75 G2** | | SPI1 CE2 | UART0 CTS | |
| 17 | **HUB75 CLK** | | SPI1 CE1 | UART0 RTS | |
| 18 | spare | | SPI1 CE0 | | PCM CLK, PWM0 |
| 19 | spare | | SPI1 MISO | | PCM FS, PWM1 |
| 20 | **HUB75 D** | | SPI1 MOSI | | PCM DIN |
| 21 | **HUB75 STB/LAT** | | SPI1 SCLK | | PCM DOUT |
| 22 | **HUB75 A** | | | | SD/SDIO |
| 23 | **HUB75 B2** | | | | SD/SDIO |
| 24 | **HUB75 E** | | | | SD/SDIO |
| 25 | spare | | | | SD/SDIO |
| 26 | **HUB75 B** | | | | SD/SDIO |
| 27 | **HUB75 C** | | | | SD/SDIO |

**CM5 free now:** BCM 2, 3, 9, 10, 11, 14, 15, 18, 19, 25 (10 lines) — no
required function. BCM 7/8 are *recommended* for RP2354B RUN/BOOTSEL control;
BCM 0/1 are HAT-ID, leave alone.

> The CM5 also has its dedicated, non-GPIO interfaces — 2× CSI (cameras), 2×
> HDMI, USB 2.0 host, the 3V3/5V rails. Those aren't in this bank-0 table.

---

## RP2354B (QFN-80, 48 GPIO) — GP0–47

Every GPIO can also be **SIO** (plain software GPIO) and **PIO0 / PIO1 / PIO2**
(programmable I/O) — not repeated per row. PWM column is the specific slice/
channel. ADC and **HSTX** (high-speed transmit, F0) are RP2350-specific —
**verify exact pins against the RP2350 datasheet Bank-0 function-select table**
(couldn't fetch it live in this build; SPI/UART/I²C/PWM follow the standard
RP-series mux, ADC = GP40–47, HSTX = GP12–19).

| GP | ProtoHUD use (main) | used as | SPI | UART | I²C | PWM | special |
|---:|---------------------|---------|-----|------|-----|-----|---------|
| 0 | **`DBG_TX`** | UART0 TX | SPI0 RX | UART0 TX | I2C0 SDA | 0A | |
| 1 | **`DBG_RX`** | UART0 RX | SPI0 CSn | UART0 RX | I2C0 SCL | 0B | |
| 2 | **`MX_CLK`** (MAX7219) | SPI0 SCK | SPI0 SCK | UART0 CTS | I2C1 SDA | 1A | |
| 3 | **`MX_DIN`** (MAX7219) | SPI0 TX | SPI0 TX | UART0 RTS | I2C1 SCL | 1B | |
| 4 | **`SDA0`** (sensors) | I2C0 SDA | SPI0 RX | UART1 TX | I2C0 SDA | 2A | |
| 5 | **`SCL0`** (sensors) | I2C0 SCL | SPI0 CSn | UART1 RX | I2C0 SCL | 2B | |
| 6 | **`SENS_INT`** | SIO in | SPI0 SCK | UART1 CTS | I2C1 SDA | 3A | |
| 7 | **`MX_CS1`** | SIO | SPI0 TX | UART1 RTS | I2C1 SCL | 3B | |
| 8 | **`MX_CS2`** | SIO | SPI1 RX | UART1 TX | I2C0 SDA | 4A | |
| 9 | **`MX_CS3`** | SIO | SPI1 CSn | UART1 RX | I2C0 SCL | 4B | |
| 10 | **`MX_CS4`** | SIO | SPI1 SCK | UART1 CTS | I2C1 SDA | 5A | |
| 11 | spare | — | SPI1 TX | UART1 RTS | I2C1 SCL | 5B | |
| 12 | spare | — | SPI1 RX | UART0 TX | I2C0 SDA | 6A | HSTX |
| 13 | spare | — | SPI1 CSn | UART0 RX | I2C0 SCL | 6B | HSTX |
| 14 | spare | — | SPI1 SCK | UART0 CTS | I2C1 SDA | 7A | HSTX |
| 15 | spare | — | SPI1 TX | UART0 RTS | I2C1 SCL | 7B | HSTX |
| 16 | **`LED1_DAT`** (WS2812) | PIO | SPI0 RX | UART0 TX | I2C0 SDA | 8A | HSTX |
| 17 | **`LED2_DAT`** | PIO | SPI0 CSn | UART0 RX | I2C0 SCL | 8B | HSTX |
| 18 | **`LED3_DAT`** | PIO | SPI0 SCK | UART0 CTS | I2C1 SDA | 9A | HSTX |
| 19 | **`LED4_DAT`** | PIO | SPI0 TX | UART0 RTS | I2C1 SCL | 9B | HSTX |
| 20 | **`SRV1`** (servo) | PWM | SPI0 RX | UART1 TX | I2C0 SDA | 10A | |
| 21 | **`SRV2`** | PWM | SPI0 CSn | UART1 RX | I2C0 SCL | 10B | |
| 22 | **`SRV3`** | PWM | SPI0 SCK | UART1 CTS | I2C1 SDA | 11A | |
| 23 | **`SRV4`** | PWM | SPI0 TX | UART1 RTS | I2C1 SCL | 11B | |
| 24 | **`SRV5`** | PWM | SPI1 RX | UART1 TX | I2C0 SDA | 0A | |
| 25 | **`SRV6`** | PWM | SPI1 CSn | UART1 RX | I2C0 SCL | 0B | |
| 26 | **`SRV7`** | PWM | SPI1 SCK | UART1 CTS | I2C1 SDA | 1A | |
| 27 | **`SRV8`** | PWM | SPI1 TX | UART1 RTS | I2C1 SCL | 1B | |
| 28 | **`BTN1`** | SIO in | SPI1 RX | UART0 TX | I2C0 SDA | 2A | |
| 29 | **`BTN2`** | SIO in | SPI1 CSn | UART0 RX | I2C0 SCL | 2B | |
| 30 | **`BTN3`** | SIO in | SPI1 SCK | UART0 CTS | I2C1 SDA | 3A | |
| 31 | **`BTN4`** | SIO in | SPI1 TX | UART0 RTS | I2C1 SCL | 3B | |
| 32 | **`BTN5`** | SIO in | SPI0 RX | UART0 TX | I2C0 SDA | 4A | |
| 33 | **`BTN6`** | SIO in | SPI0 CSn | UART0 RX | I2C0 SCL | 4B | |
| 34 | **`BTN7`** | SIO in | SPI0 SCK | UART0 CTS | I2C1 SDA | 5A | |
| 35 | **`BTN8`** | SIO in | SPI0 TX | UART0 RTS | I2C1 SCL | 5B | |
| 36 | **`BTN9`** | SIO in | SPI0 RX | UART1 TX | I2C0 SDA | 6A | |
| 37 | **`BTN10`** | SIO in | SPI0 CSn | UART1 RX | I2C0 SCL | 6B | |
| 38 | spare *(fan zone 1?)* | — | SPI0 SCK | UART1 CTS | I2C1 SDA | 7A | |
| 39 | spare *(fan zone 2?)* | — | SPI0 TX | UART1 RTS | I2C1 SCL | 7B | |
| 40 | **`AIN0`** (analog) | ADC0 | SPI1 RX | UART1 TX | I2C0 SDA | 8A | **ADC0** |
| 41 | **`AIN1`** | ADC1 | SPI1 CSn | UART1 RX | I2C0 SCL | 8B | **ADC1** |
| 42 | **`AIN2`** | ADC2 | SPI1 SCK | UART1 CTS | I2C1 SDA | 9A | **ADC2** |
| 43 | **`AIN3`** | ADC3 | SPI1 TX | UART1 RTS | I2C1 SCL | 9B | **ADC3** |
| 44 | **`AIN4`** | ADC4 | SPI1 RX | UART0 TX | I2C0 SDA | 10A | **ADC4** |
| 45 | **`AIN5`** | ADC5 | SPI1 CSn | UART0 RX | I2C0 SCL | 10B | **ADC5** |
| 46 | **`AIN6`** | ADC6 | SPI1 SCK | UART0 CTS | I2C1 SDA | 11A | **ADC6** |
| 47 | **`AIN7`** | ADC7 | SPI1 TX | UART0 RTS | I2C1 SCL | 11B | **ADC7** |

Non-GPIO pins: **USB_DP/USB_DM** (native USB, → SW1 selector), **XIN/XOUT**
(12 MHz crystal), **SWCLK/SWDIO** (SWD), **RUN** (reset), **QSPI** bank (on-die
2 MB flash; QSPI_SS = BOOTSEL strap), power/ground.

**RP2354B free now:** GP11–GP15 (5 digital spares; GP12–15 are HSTX-capable) and
GP38/GP39 (2 more, suggested for fan PWM). The 8 ADC pins (GP40–47) are mapped to
`AIN0..7` headers but are free to repurpose. → ~7 spare digital + 8 analog.

### Why these assignments are mux-legal
The ProtoHUD picks line up with the function mux: `SDA0`/`SCL0` on GP4/GP5 = a
real **I²C0** pair; `MX_CLK`/`MX_DIN` on GP2/GP3 = **SPI0 SCK/TX**; `DBG_TX/RX`
on GP0/GP1 = **UART0**; the four `MX_CS*` use **SIO** (software chip-selects, so
they need no hardware CSn pin); WS2812 uses **PIO** (any GPIO); all 8 servos sit
on **PWM**-capable pins (slices 10/11/0/1, channels A/B = 8 independent); ADC is
on the only ADC-capable pins (GP40–47). No conflicts.

---

## At-a-glance

| | CM5 | RP2354B |
|--|-----|---------|
| Total GPIO | 28 (BCM 0–27) | 48 (GP0–47) |
| In ProtoHUD use | 14 HUB75 + 2 ctrl | ~31 |
| Free | 10 | ~7 digital + 8 ADC |
| Drives | HUB75 only | sensors, MAX7219, WS2812, servos, buttons, analog |
| Logic | 3.3 V (not 5 V-tol) | 3.3 V (not 5 V-tol) |
