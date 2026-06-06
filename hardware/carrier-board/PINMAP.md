# Carrier Board — master pin / net map

The single source of truth for CM5 GPIO allocation. Schematic nets, the buffer
connections, and the firmware config should all agree with this table. Values
come from the code: HUB75 = `src/main.cpp` `kHub75`; I²C/SPI/WS2812 = the device
configs; free-pin ledger = what HUB75 leaves behind.

> CM5 exposes GPIO on its two DF40 connectors, not a physical 40-pin header —
> the carrier *creates* any header. This table is **BCM-logical** (what the
> firmware addresses); assign physical header/connector pins during layout.

## Allocation — BCM 0–27 (HUB75 build)

| BCM | Default fn | ProtoHUD use | On carrier | Notes |
|----:|-----------|--------------|-----------|-------|
| 0 | ID_SD | — | leave / ID EEPROM | avoid (HAT ID) |
| 1 | ID_SC | — | leave / ID EEPROM | avoid (HAT ID) |
| 2 | I²C1 SDA | **I²C bus 1 data** | direct + 4.7k pull-up | sensors + I/O expanders |
| 3 | I²C1 SCL | **I²C bus 1 clock** | direct + 4.7k pull-up | sensors + I/O expanders |
| 4 | GPCLK0 | **HUB75 OE** | →74AHCT245→ J2 | face buffer |
| 5 | — | **HUB75 R1** | →74AHCT245→ J2 | face buffer |
| 6 | — | **HUB75 B1** | →74AHCT245→ J2 | face buffer |
| 7 | SPI0 CE1 | SPI0 chip-select 1 | J3/J4 (face/LED lane) | free w/ HUB75 |
| 8 | SPI0 CE0 | SPI0 chip-select 0 | J3/J4 | free w/ HUB75 |
| 9 | SPI0 MISO | SPI0 MISO | J3 (expander readback) | free w/ HUB75 |
| 10 | SPI0 MOSI | **WS2812 / MAX7219 / SPI exp.** | →74AHCT125/245→ J4/J3 | ⚠️ shared — one owner at a time |
| 11 | SPI0 SCLK | SPI0 clock | →buffer→ J3/J4 | free w/ HUB75 |
| 12 | — | **HUB75 R2** | →74AHCT245→ J2 | face buffer |
| 13 | — | **HUB75 G1** | →74AHCT245→ J2 | face buffer |
| 14 | UART TXD | spare (MAX7219 bit-bang / button) | header | free w/ HUB75 |
| 15 | UART RXD | spare (MAX7219 bit-bang / button) | header | free w/ HUB75 |
| 16 | SPI1 CE2 | **HUB75 G2** | →74AHCT245→ J2 | (blocks SPI1) |
| 17 | SPI1 CE1 | **HUB75 CLK** | →74AHCT245→ J2 | (blocks SPI1) |
| 18 | PCM CLK | spare (I²S free — audio on USB) | header | free w/ HUB75 |
| 19 | PCM FS | spare (I²S free — audio on USB) | header | free w/ HUB75 |
| 20 | SPI1 MOSI | **HUB75 D** | →74AHCT245→ J2 | **blocks SPI1** |
| 21 | SPI1 SCLK | **HUB75 STB/LAT** | →74AHCT245→ J2 | **blocks SPI1** |
| 22 | — | **HUB75 A** | →74AHCT245→ J2 | face buffer |
| 23 | — | **HUB75 B2** | →74AHCT245→ J2 | face buffer |
| 24 | — | **HUB75 E** | →74AHCT245→ J2 | face buffer |
| 25 | — | **I/O-expander INT / coproc IRQ** | header → BCM 25 | free w/ HUB75 |
| 26 | — | **HUB75 B** | →74AHCT245→ J2 | face buffer |
| 27 | — | **HUB75 C** | →74AHCT245→ J2 | face buffer |

## Bus summary

| Bus | Pins (BCM) | Status w/ HUB75 | Carrier role |
|-----|-----------|-----------------|--------------|
| I²C bus 1 | 2, 3 | ✅ free pins, shared bus | sensors + MCP23017 expanders (+ STEMMA QT header) |
| SPI0 | 7, 8, 9, 10, 11 | ✅ fully free | WS2812 **or** MAX7219 **or** SPI expander/shift regs |
| SPI1 | 16, 17, 18, 19, 20, 21 | ❌ unusable | MOSI/SCLK (20/21) are HUB75 D/STB |
| UART | 14, 15 | ✅ free | spare GPIO / MAX7219 bit-bang |
| PCM/I²S | 18, 19 | ✅ free (audio on USB) | spare GPIO / INT lines |

## Free-pin ledger (HUB75 active)

**Free: `{7, 8, 9, 10, 11, 14, 15, 18, 19, 25}`** — 10 lines. Everything else is
HUB75 (14), I²C (2), or HAT-ID (2). Demand on those 10:

| Want | Takes from free set |
|------|---------------------|
| WS2812 custom panel | 10 (MOSI) |
| MAX7219 on hardware SPI0 | 10, 11, 8/7 |
| MAX7219 bit-bang (with WS2812) | 14, 15 (DIN/CLK) + 7/8/25 (CS) |
| I/O-expander INT / coproc IRQ | 25 |
| Cooling fans (2 zones) | 18, 19 (one GPIO per zone → MOSFET) |
| Buttons via I/O expander | **0 Pi pins** (rides I²C) ✅ |

This is exactly why the recommended button/LED path is the **I²C expander**
([`IO-EXPANSION.md`](IO-EXPANSION.md)) or the **USB coprocessor**
([`../../docs/coprocessor-input.md`](../../docs/coprocessor-input.md)) — both
sidestep the scarce free pins.

## Contention rules (enforce in schematic + config)

1. **BCM 10 has one owner at a time** — WS2812 *or* MAX7219-SPI0 *or* an SPI
   expander. Use jumper/0Ω select; if two need the SPI lane, move one to
   bit-bang or the I²C expander.
2. **SPI1 is off-limits** in a HUB75 build (20/21 = D/STB).
3. **Don't reuse HUB75 pins** for buttons/aux — route those from the free set
   or an expander; the firmware GPIO picker already hides hardware-claimed pins.
4. **I²C address hygiene** — see the address map in [`IO-EXPANSION.md`](IO-EXPANSION.md)
   (existing: 0x23, 0x28, 0x5A, 0x68).

## MAX7219 / `rgb_matrix` build delta

The `rgb_matrix` backend defaults its right chain to `spidev1.0` — **invalid
with HUB75** (SPI1 blocked). Override to a single `spidev0` chain or bit-bang.
A MAX7219-only build (no HUB75) frees all 14 HUB75 pins and re-enables SPI1.
