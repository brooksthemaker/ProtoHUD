# ProtoHUD MP3 Player

Standalone embedded MP3/FLAC player using a two-chip design:

| Board | Role |
|-------|------|
| **Raspberry Pi Pico 2 W** | Main controller: display, SD, UI, USB file upload, BLE control |
| **ESP32-WROOM-32** | Bluetooth audio bridge: A2DP source/sink, PCM5102A DAC |

---

## Features

- MP3 and FLAC playback from microSD (FAT32)
- **ID3v2 tag display**: title, artist, album read from ID3v2.3/v2.4 frames with full UTF-8 output (ISO-8859-1, UTF-16LE/BE, UTF-8 source encodings supported); ID3v1 fallback
- **Track duration**: read from TLEN tag → Xing/Info VBR header → CBR file-size estimate for MP3; from STREAMINFO for FLAC
- **Cover art**: JPEG decoded and displayed from `cover.jpg` / `folder.jpg` in the album directory (80×80 canvas, JPEGDEC)
- **5-band parametric EQ**: 60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz peaking filters; presets Flat / Bass Boost / Vocal / Treble
- **Settings persistence**: volume, shuffle, repeat, and EQ preset auto-saved to `/settings.bin` on SD after each change (500 ms debounce)
- **Backlight management**: dims to 30% after 30 s idle, turns off after 60 s; encoder input wakes to full brightness
- **USB Mass Storage**: plug in a USB cable → SD card mounts on your computer for drag-and-drop file transfer
- **Wired headphones**: PCM5102A DAC → 3.5 mm jack
- **BT Headphones (source)**: stream music wirelessly to any A2DP Bluetooth headphone or speaker
- **BT Receive (sink)**: receive A2DP audio from your phone → play on wired headphones
- **BLE control**: play/pause, skip, volume, mode change from phone (nRF Connect compatible)
- **ILI9341 320×240 TFT**: Now Playing (album art + metadata), File Browser, Settings, BT Devices screens
- **ANO Rotary Navigation Encoder**: rotate to scroll/volume, click to select, directional buttons to navigate screens
- Shuffle, repeat (OFF / ONE / ALL), queue management
- Toast notifications for volume and EQ changes

---

## Bill of Materials

| Part | Notes |
|------|-------|
| Raspberry Pi Pico 2 W | RP2350 + CYW43439, ~$7 |
| ESP32-WROOM-32 dev board | Any bare ESP32 (not S3/S2), ~$5 |
| ILI9341 2.8" TFT (320×240) | With SPI interface and LEDA/BL pin, ~$8 |
| Adafruit ANO Rotary Navigation Encoder | Stemma QT, product #6310 |
| PCM5102A DAC breakout | e.g. HiLetgo PCM5102A, ~$6 |
| MicroSD breakout (SPI) | Or use the one on the TFT module |
| MicroSD card | FAT32 formatted |
| 3.5 mm stereo jack | TRRS or TRS |
| Misc | Dupont wires, breadboard or custom PCB |

---

## Wiring

### Pico 2 W

```
Pico 2 W GPIO    →  Device
─────────────────────────────────────────────────────────────────
GP0  (UART0 TX)  →  ESP32 GPIO3 (U0RXD)      2 Mbps audio bridge
GP1  (UART0 RX)  →  ESP32 GPIO1 (U0TXD)
GP4  (I2C0 SDA)  →  ANO Encoder SDA (Stemma QT)
GP5  (I2C0 SCL)  →  ANO Encoder SCL (Stemma QT)
GP6  (input)     →  ANO Encoder INT
GP16 (SPI0 MISO) →  ILI9341 MISO  +  SD MISO
GP18 (SPI0 SCK)  →  ILI9341 SCK   +  SD SCK
GP19 (SPI0 MOSI) →  ILI9341 MOSI  +  SD MOSI
GP17             →  ILI9341 CS
GP20             →  ILI9341 DC
GP21             →  ILI9341 RST
GP28 (PWM)       →  ILI9341 LEDA / BL  (backlight)
GP22             →  SD card CS
USB              →  Host computer (USB Mass Storage)
3V3              →  ANO VIN, ILI9341 VCC, SD VCC
GND              →  All grounds
```

> **SPI bus shared**: ILI9341 and SD share MOSI/MISO/SCK. CS pins are separate.
> The firmware uses `SPI.beginTransaction()` around each access for safe arbitration.
> Core 1 (audio decode) only touches UART — never SPI.

> **Backlight (GP28)**: driven by `analogWrite()` at PWM duty 0–255. If your TFT module
> ties LEDA to 3.3 V via a resistor, cut that trace and wire GP28 instead. If you skip
> this connection the backlight stays on permanently at full brightness.

### ESP32-WROOM-32

```
ESP32 GPIO  →  Device
─────────────────────────────────────────────────────────────────
GPIO3  (U0RXD)  →  Pico GP0 (UART TX)
GPIO1  (U0TXD)  →  Pico GP1 (UART RX)
GPIO26          →  PCM5102A BCK
GPIO25          →  PCM5102A LRCK
GPIO22          →  PCM5102A DIN
GPIO27          →  PCM5102A XSMT (mute, active low)
3V3             →  PCM5102A VCC
GND             →  PCM5102A GND
```

### PCM5102A Breakout Wiring

```
PCM5102A pin  →  Connection
─────────────────────────────────────────────────────────────────
VCC           →  3.3 V
GND           →  GND
BCK           →  ESP32 GPIO26
LRCK/WS       →  ESP32 GPIO25
DIN           →  ESP32 GPIO22
XSMT          →  ESP32 GPIO27  (drive HIGH to unmute)
FLT           →  GND  (normal latency filter)
DEMP          →  GND  (de-emphasis off)
FMT           →  GND  (I2S standard format)
SCK           →  GND  (no system clock needed for PCM5102A)
LINEOUT L/R   →  3.5 mm stereo jack tip/ring
AGND          →  Sleeve of 3.5 mm jack
```

### ANO Encoder (Stemma QT)

Connect via a Stemma QT / QWIIC 4-pin cable to the Pico's I2C0 pins:
- **VIN** → 3V3
- **GND** → GND
- **SDA** → GP4
- **SCL** → GP5
- **INT** → GP6 (wire manually from the INT pad on the encoder breakout)

---

## SD Card Layout

The player scans for `.mp3` and `.flac` files recursively from `/`. Recommended structure:

```
/
├── Artist Name/
│   ├── Album Title/
│   │   ├── cover.jpg       ← album art (also: Cover.jpg, folder.jpg, Folder.jpg)
│   │   ├── 01 - Track.mp3
│   │   └── 02 - Track.flac
└── settings.bin             ← auto-created by firmware (do not edit)
```

**Album art**: place a JPEG named `cover.jpg`, `Cover.jpg`, `folder.jpg`, `Folder.jpg`, `album.jpg`, or `Album.jpg` in the same folder as your tracks. The image is decoded and scaled to 80×80 on the Now Playing screen.

**ID3 tags**: the player reads ID3v2.3 and ID3v2.4 tags for title, artist, album, and duration. If no ID3v2 tag is present it falls back to ID3v1 (last 128 bytes). Track title falls back to the filename if no tag is found.

---

## Firmware

Two separate PlatformIO projects, one per chip.

### Building & Flashing

```bash
# Flash ESP32 bridge (do this first — it's simpler)
cd firmware/mp3_player/esp32_bridge
pio run -e esp32bridge -t upload

# Vendor the header-only decoders for Pico
cd firmware/mp3_player/pico/src/audio/vendor
# see vendor/README.md for curl commands

# Flash Pico 2 W
cd firmware/mp3_player/pico
pio run -e rpipico2w -t upload
```

### USB File Transfer

1. Connect Pico USB to computer. Audio pauses automatically.
2. SD card appears as a removable drive ("MP3 Player SD Card").
3. Drag and drop files, create folders, delete unwanted tracks.
4. Eject the drive. Playback resumes automatically.

---

## UI Navigation

| Encoder Action | Effect |
|----------------|--------|
| Rotate CW/CCW  | Scroll list / adjust focused value |
| Centre press   | Select item / play file |
| Centre hold    | Play / pause toggle |
| Up button      | Now Playing screen |
| Down button    | File Browser screen |
| Right button   | Settings screen |
| Left button    | BT Devices screen |

### Settings Screen

| Setting | Options | Saved |
|---------|---------|-------|
| Output Mode | Wired / BT Headphones / BT Sink | — |
| Volume | 0–100 (slider) | Yes |
| Shuffle | On / Off | Yes |
| Repeat | OFF / ONE / ALL | Yes |
| EQ | Flat / Bass Boost / Vocal / Treble | Yes |

All saved settings are written to `/settings.bin` on the SD card and restored on the next boot. A 500 ms debounce prevents rapid encoder turns from thrashing the card.

---

## Equalizer

The 5-band parametric EQ runs on Core 1 in the audio decode pipeline, applied after volume scaling and before PCM output. Filters use standard peaking-EQ biquad coefficients (Audio EQ Cookbook, Bristow-Johnson), computed in floating point on the RP2350B’s Cortex-M33 FPU.

### Band Centres

| Band | Frequency | Q |
|------|-----------|---|
| 1 | 60 Hz | 1.0 |
| 2 | 250 Hz | 1.0 |
| 3 | 1 kHz | 1.0 |
| 4 | 4 kHz | 1.0 |
| 5 | 12 kHz | 1.0 |

### Presets (gain in dB per band)

| Preset | 60 Hz | 250 Hz | 1 kHz | 4 kHz | 12 kHz |
|--------|-------|--------|-------|-------|--------|
| Flat | 0 | 0 | 0 | 0 | 0 |
| Bass Boost | +8 | +5 | −2 | −1 | 0 |
| Vocal | −2 | −1 | +5 | +4 | +1 |
| Treble | 0 | −1 | −1 | +4 | +8 |

Preset changes are applied between audio frames using a lock-free atomic handoff from Core 0 to Core 1 — no audio glitch or mutex stall.

---

## Backlight

GP28 drives the display backlight via PWM (`analogWrite`). Brightness is managed automatically based on encoder inactivity:

| Idle time | Brightness |
|-----------|------------|
| 0–30 s | 100% (full) |
| 30–60 s | 30% (dim) |
| > 60 s | 0% (off) |

Any encoder rotation or button press instantly restores full brightness. If GP28 is not wired to the display’s backlight pin, the screen stays on at full brightness regardless.

---

## BLE Control (nRF Connect / Custom App)

Connect to `MP3Player` in your BLE scanner.  
Service UUID: `12340000-5678-1234-5678-1234567890AB`

| Characteristic | UUID suffix | Direction | Values |
|----------------|-------------|-----------|--------|
| Play/Pause | `...1001...` | Write | `0x01` play, `0x00` pause |
| Skip | `...1002...` | Write | `0x01` next, `0xFF` prev |
| Volume | `...1003...` | Write/Notify | `0`–`100` |
| Mode | `...1004...` | Write | `0` SD, `1` BT Source, `2` BT Sink |
| Track Info | `...1005...` | Notify | `title|artist|album|pos_s|dur_s` |
| Status | `...1006...` | Notify | 2 bytes: `[playing, mode]` |

---

## Bluetooth Audio Notes

- **Source mode** (stream to BT headphones): device advertises as `MP3Player`. Pair your headphones to it. Music streams from SD via the ESP32 A2DP source role.
- **Sink mode** (receive from phone): device advertises as `MP3Player-In`. Connect from your phone’s Bluetooth audio settings (like connecting to a speaker). Audio plays on the wired PCM5102A output.
- **A2DP source and sink cannot run simultaneously** — switch modes in Settings.
- Mode switch takes ~1 second (BT stack handoff on ESP32).

---

## Architecture

```
Pico 2 W (Core 0)              Pico 2 W (Core 1)
──────────────────────        ──────────────────────────────
LVGL UI @ ~30 fps               MP3/FLAC decode (minimp3/dr_flac)
Encoder polling (seesaw)        ↓ read_tags(): ID3v2.3/2.4 + ID3v1
BLE GATT server                 ↓ extract_cover_art(): JPEG → CoverArtBuf
USB MSC (TinyUSB)               ↓ apply_volume(): integer gain
Bridge poll (status RX)         ↓ Eq::process(): 5-band biquad FPU
Settings persistence            ↓ UART → ESP32 bridge
Backlight PWM (GP28)            @ 2 Mbps (1.4 Mbps audio net)

Core 0 → Core 1 signals (lock-free atomics):
  volume_.store()          → picked up per frame in decode loop
  eq_preset_req_.store()   → picked up per frame; coefficients recomputed
  cover_art.generation     → acquire/release fence; Core 0 renders JPEG

                    UART (GP0/GP1, 2 Mbps)
                           ⇕
ESP32-WROOM (Core 0)                 ESP32-WROOM (Core 1)
────────────────────────         ──────────────────────────────
UART frame handler                   I2S drain task (priority 22)
BT A2DP source/sink                  Writes PCM ring buffer → I2S
PCM5102A I2S master                  → PCM5102A → 3.5 mm jack
```

### Settings file format (`/settings.bin`)

10-byte binary record, XOR checksum:

| Offset | Field | Notes |
|--------|-------|-------|
| 0–2 | Magic `PHS` | File identifier |
| 3 | Version `0x01` | Format version |
| 4 | Volume | 0–100 |
| 5 | Shuffle | `0`=off, `1`=on |
| 6 | Repeat | `0`=OFF, `1`=ONE, `2`=ALL |
| 7 | EQ preset | `0`=Flat, `1`=Bass Boost, `2`=Vocal, `3`=Treble |
| 8 | Reserved | `0x00` |
| 9 | Checksum | XOR of bytes 0–8 |

The file is written atomically (remove + reopen) by the `Settings::do_save()` helper after a 500 ms debounce timer. A corrupt or missing file is silently ignored and firmware defaults are used.
