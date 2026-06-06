# Carrier Board — Block Diagram

System-level block diagram for the ProtoHUD CM5 carrier. Voltage domains and the
**3.3 V → 5 V level-shifting boundary** are called out explicitly — that
boundary is the whole reason the carrier exists (see [`README.md`](README.md)).

The **face display backend is "pick one"**: either **HUB75** panels *or* a
**MAX7219** 8×8-matrix chain (`src/face/max7219_chain.h`). Both are 5 V-logic
and share the same 3.3 V → 5 V buffer; a jumper selects which connector is
driven. Pin references match the firmware (`src/main.cpp` `kHub75`, MAX7219
SPI0 / GPIO transports, I²C bus 1, WS2812 on SPI0 MOSI / BCM 10).

```mermaid
flowchart TB
    J1["J1 · 5 V INPUT<br/>fuse · reverse-polarity FET · TVS"]:::pwr

    subgraph RAILS["Power rails (common GND)"]
        direction LR
        R5C["5 V → CM5 (≥ 5 A)"]:::pwr
        R5F["5 V → face panels<br/>HUB75 / MAX7219 · bulk caps"]:::pwr
        R5L["5 V → WS2812 (fused)"]:::pwr
        R33["3.3 V (from CM5)<br/>sensors ~47 mA"]:::pwr
    end
    J1 --> R5C & R5F & R5L
    R5C --> CM5

    CM5["Raspberry Pi CM5 — 2× DF40 100-pin<br/>GPIO = 3.3 V CMOS, not 5 V-tolerant"]:::cm5
    CM5 -. provides .-> R33

    %% ================= 3.3 V → 5 V level-shift boundary =================
    subgraph SHIFT["3.3 V → 5 V LEVEL SHIFT (required)"]
        direction TB
        FBUF["U1/U2 · 74AHCT245 ×2 @ 5 V<br/>face-display buffer"]:::shift
        LBUF["U3 · 74AHCT1G125 @ 5 V<br/>WS2812 data"]:::shift
    end

    CM5 -- "HUB75 14×  OR  MAX7219 DIN/CLK/CS<br/>(SPI0 BCM10/11/7-8, or bit-banged GPIO) · 3.3 V" --> FBUF
    CM5 -- "SPI0 MOSI (BCM 10) · 3.3 V" --> LBUF

    %% ---- Face backend: pick one (jumper-selected) ----
    subgraph FACE["FACE DISPLAY BACKEND — populate / jumper ONE"]
        direction LR
        JSEL{{"JP1 · backend select"}}:::sel
        HUB["J2 · HUB75 panels<br/>2×8 IDC<br/>R1 G1 B1 R2 G2 B2 A B C D E CLK STB OE"]:::load
        MAX["J3 · MAX7219 chain header<br/>5 V · GND · DIN · CLK · CS×4"]:::load
    end
    FBUF -- "5 V" --> JSEL
    JSEL --> HUB
    JSEL --> MAX
    R5F --> HUB
    R5F --> MAX

    LBUF -- "5 V data" --> LED["J4 · WS2812 accessory LEDs"]:::load
    R5L --> LED

    %% ---- 3.3 V-native: direct connect ----
    subgraph DIRECT["3.3 V native — direct connect"]
        direction TB
        I2C["J5 · I²C bus 1 (SDA BCM2 / SCL BCM3)<br/>4.7k pull-ups → BNO055 0x28 · MPU9250 0x68<br/>· MPR121 0x5A · BH1750 0x23"]:::dir
        BTN["J6 · GPIO buttons / boop<br/>(route clear of HUB75/SPI pins)"]:::dir
        CSI["J7/J8 · 2× CSI cameras (22-pin FFC)"]:::dir
    end
    CM5 --- I2C
    CM5 --- BTN
    CM5 --- CSI
    R33 --> I2C

    %% ---- USB stack ----
    subgraph USB["USB (optional onboard hub — N1)"]
        direction TB
        HUBIC["J9 · USB 2.0 hub"]:::usb
        RP2350["RP2350 helmet audio<br/>(UAC2: 6-mic beamform/NR/DOA)"]:::usb
        KNOB["Smart knob"]:::usb
        LORA["LoRa RAK4631"]:::usb
        VIT["VITURE glasses"]:::usb
        UCAM["USB cameras"]:::usb
        HUBIC --> RP2350 & KNOB & LORA & VIT & UCAM
    end
    CM5 --- HUBIC
    CM5 -- "2× HDMI" --> HDMI["J10 · HDMI out / VITURE"]:::load

    classDef pwr   fill:#fde2c4,stroke:#c9772a,color:#000;
    classDef cm5   fill:#cfe3ff,stroke:#2a5bc9,color:#000,font-weight:bold;
    classDef shift fill:#ffd6d6,stroke:#c92a2a,color:#000,font-weight:bold;
    classDef load  fill:#e8e8e8,stroke:#666,color:#000;
    classDef dir   fill:#d6f5d6,stroke:#2a9d3a,color:#000;
    classDef usb   fill:#ece0ff,stroke:#7a3ac9,color:#000;
    classDef sel   fill:#fff3b0,stroke:#b59000,color:#000;
```

## Legend

| Color | Meaning |
|-------|---------|
| 🟧 Orange | Power rail / protection |
| 🟦 Blue | CM5 compute module (3.3 V GPIO source) |
| 🟥 Red | **3.3 V → 5 V level shifter — required** |
| 🟨 Yellow | Jumper / backend select |
| ⬜ Grey | 5 V-logic load (panels, LEDs, HDMI) |
| 🟩 Green | 3.3 V-native peripheral — direct connect, no shifter |
| 🟪 Purple | USB peripheral (behind optional hub) |

## Connector schedule

| Ref | Connector | Signals | Domain |
|-----|-----------|---------|--------|
| J1 | 5 V input | 5 V, GND (protected) | 5 V in |
| **J2** | **HUB75 face** (2×8 IDC) | R1 G1 B1 R2 G2 B2 A B C D E CLK STB OE + 5 V/GND | 5 V (buffered) |
| **J3** | **MAX7219 face** | 5 V, GND, **DIN, CLK, CS×4** | 5 V (buffered) |
| J4 | WS2812 LEDs | 5 V, GND, DIN (buffered) | 5 V |
| J5 | I²C bus 1 | SDA, SCL, 3.3 V, GND | 3.3 V |
| J6 | GPIO buttons / boop | GPIO×n, 3.3 V, GND | 3.3 V |
| J7/J8 | CSI cameras | 22-pin 0.5 mm FFC | MIPI |
| J9 | USB (hub uplink) | USB 2.0 → RP2350 audio / knob / LoRa / VITURE / cams | USB |
| J10 | HDMI | 2× HDMI out | — |
| JP1 | Backend select | routes face buffer → J2 **or** J3 | — |

### MAX7219 backend notes (`src/face/max7219_chain.h`)

- **Transport A — hardware SPI0 (`spidev0.x`, default):** DIN = MOSI (BCM 10),
  CLK = SCLK (BCM 11), CS = CE0/CE1 (BCM 8 / BCM 7). Two hardware CS lines →
  up to two chains this way.
- **Transport B — bit-banged GPIO** (`Max7219GpioBus`): one shared DIN + CLK on
  any two GPIOs, plus one CS GPIO **per chain** (up to 4 broken out at J3).
  Used when SPI0 is taken by WS2812 / SPI1 by HUB75.
- MAX7219 VCC = 5 V → input-high ≈ 3.5 V, so DIN/CLK/CS **must be buffered to
  5 V** through the same `74AHCT245` (U1/U2). All three are CM5 → driver
  (unidirectional); the chain's DOUT daisies to the next module, not back to
  the CM5, so no down-shift is needed.
- ⚠️ **BCM 10 (MOSI) is shared** by WS2812 *and* MAX7219-over-SPI0 — they can't
  both use SPI0 at once. Use the MAX7219 GPIO transport (or move WS2812 to
  SPI1) if running both; JP1 + the firmware backend select keep it to one face
  path at a time.
