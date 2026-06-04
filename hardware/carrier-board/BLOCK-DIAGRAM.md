# Carrier Board — Block Diagram

System-level block diagram for the ProtoHUD CM5 carrier. Voltage domains and the
**3.3 V → 5 V level-shifting boundary** are called out explicitly — that
boundary is the whole reason the carrier exists (see [`README.md`](README.md)).

Pin references match the firmware (`src/main.cpp` `kHub75`, I²C bus 1, WS2812 on
SPI0 MOSI / BCM 10).

```mermaid
flowchart TB
    PWR["5 V INPUT<br/>fuse · reverse-polarity FET · TVS"]:::pwr

    subgraph RAILS["Power rails (common GND)"]
        direction LR
        R5C["5 V → CM5<br/>(≥ 5 A)"]:::pwr
        R5H["5 V → HUB75 panels<br/>(high current, bulk caps)"]:::pwr
        R5L["5 V → WS2812<br/>(fused)"]:::pwr
        R33["3.3 V (from CM5)<br/>sensors ~47 mA"]:::pwr
    end
    PWR --> R5C & R5H & R5L
    R5C --> CM5

    CM5["Raspberry Pi CM5<br/>(2× DF40 100-pin)<br/>GPIO = 3.3 V CMOS, not 5 V-tolerant"]:::cm5
    CM5 -. provides .-> R33

    %% ---- 5 V-logic loads: MUST be level-shifted ----
    subgraph SHIFT["3.3 V → 5 V LEVEL SHIFT (required)"]
        direction TB
        U245["74AHCT245 ×2 @ 5 V<br/>14 HUB75 lines<br/>R1 G1 B1 R2 G2 B2 A B C D E CLK STB OE"]:::shift
        U125["74AHCT1G125 @ 5 V<br/>WS2812 data"]:::shift
    end

    CM5 -- "HUB75 14× (BCM 5,13,6,12,16,23,22,26,27,20,24,4,17,21) 3.3 V" --> U245
    CM5 -- "SPI0 MOSI (BCM 10) 3.3 V" --> U125

    U245 -- "5 V" --> HUB["HUB75 panels<br/>2×8 IDC connector(s)"]:::load
    R5H --> HUB
    U125 -- "5 V data" --> LED["WS2812 accessory LEDs"]:::load
    R5L --> LED

    %% ---- 3.3 V-native: direct connect ----
    subgraph DIRECT["3.3 V native — direct connect"]
        direction TB
        I2C["I²C bus 1 (SDA BCM2 / SCL BCM3)<br/>4.7k pull-ups → BNO055 0x28 ·<br/>MPU9250 0x68 · MPR121 0x5A · BH1750 0x23"]:::dir
        BTN["GPIO buttons / boop<br/>(avoid HUB75-claimed pins)"]:::dir
        CSI["2× CSI cameras (22-pin FFC)"]:::dir
    end
    CM5 --- I2C
    CM5 --- BTN
    CM5 --- CSI
    R33 --> I2C

    %% ---- USB stack ----
    subgraph USB["USB (optional onboard hub — N1)"]
        direction TB
        HUBIC["USB 2.0 hub"]:::usb
        RP2350["RP2350 helmet audio<br/>(UAC2: 6-mic beamform/NR/DOA)"]:::usb
        KNOB["Smart knob"]:::usb
        LORA["LoRa RAK4631"]:::usb
        VIT["VITURE glasses"]:::usb
        UCAM["USB cameras"]:::usb
        HUBIC --> RP2350 & KNOB & LORA & VIT & UCAM
    end
    CM5 --- HUBIC
    CM5 -- "2× HDMI" --> HDMI["HDMI out / VITURE"]:::load

    classDef pwr   fill:#fde2c4,stroke:#c9772a,color:#000;
    classDef cm5   fill:#cfe3ff,stroke:#2a5bc9,color:#000,font-weight:bold;
    classDef shift fill:#ffd6d6,stroke:#c92a2a,color:#000,font-weight:bold;
    classDef load  fill:#e8e8e8,stroke:#666,color:#000;
    classDef dir   fill:#d6f5d6,stroke:#2a9d3a,color:#000;
    classDef usb   fill:#ece0ff,stroke:#7a3ac9,color:#000;
```

## Legend

| Color | Meaning |
|-------|---------|
| 🟧 Orange | Power rail / protection |
| 🟦 Blue | CM5 compute module (3.3 V GPIO source) |
| 🟥 Red | **3.3 V → 5 V level shifter — required** |
| ⬜ Grey | 5 V-logic load (panels, LEDs, HDMI) |
| 🟩 Green | 3.3 V-native peripheral — direct connect, no shifter |
| 🟪 Purple | USB peripheral (behind optional hub) |

The red blocks are the critical path: every signal crossing into a grey HUB75 /
WS2812 load goes through a 5 V buffer. Everything green wires straight to the
CM5 at 3.3 V.
