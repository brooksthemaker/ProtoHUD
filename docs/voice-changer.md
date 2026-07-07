# Voice changer (coprocessor Pico)

An **optional** real-time voice changer that runs on the button coprocessor's
RP2350 (Raspberry Pi Pico 2) — on **core1**, so it never disturbs the button
debounce / serial protocol on core0. Firmware lives in
[`firmware/button_coproc/pico`](../firmware/button_coproc/pico); enable it by
building the `rpipico2w_voice` PlatformIO env.

## Signal path

```
electret mic ─► MAX9814 preamp/AGC ─► RP2350 ADC ─► DSP (core1) ─► I2S ─► TLV320DAC3100 ─► speaker
```

Why this shape: the TLV320DAC3100 is **output only** (no ADC), and an I2S mic +
I2S DAC on one RP2350 means full-duplex I2S, which needs custom PIO to stay
sample-locked. Feeding an **analog mic through the ADC** and pacing the whole
loop off the **I2S output clock** (one ADC read per output frame) keeps input and
output locked with no drift and no input DMA — reliable to bring up. The effects
mask the 12-bit ADC's noise, so voice quality is more than fine. (An I2S MEMS mic
is a future quality bump once duplex-I2S PIO is in place.)

## Wiring

Pins are set in [`include/config.h`](../firmware/button_coproc/pico/include/config.h)
(“Voice changer” block) and stay clear of the button pins (GP2–GP9 by default).

| Signal | Pico GP | To |
|---|---|---|
| Mic audio | GP26 (ADC0) | MAX9814 **OUT** |
| I2S BCLK | GP16 | TLV320 **BCLK** |
| I2S WS/LRCLK | GP17 (BCLK+1, automatic) | TLV320 **WSEL** |
| I2S data | GP18 | TLV320 **DIN** |
| I2C SDA | GP20 | TLV320 **SDA** |
| I2C SCL | GP21 | TLV320 **SCL** |
| DAC reset | GP22 | TLV320 **RST** (or −1 if tied high) |

- **No MCLK wire:** the TLV320's PLL is clocked from BCLK (configured over I2C).
- **MAX9814:** VDD 3.3 V, GND common, OUT → GP26. Its onboard AGC gives a steady
  level; set its GAIN pin to taste (40/50/60 dB).
- **Power the DAC's speaker amp from 5 V (VBUS)**, common ground, with a
  100–220 µF cap at the amp rail. The TLV320's Class-D speaker output is **mono**;
  for stereo/louder, route its HPL/HPR line outs into your MAX98306.
- **I2C is remapped off GP4/GP5** (the earlephilhower default) because those are
  button pins.

## Build & flash

```bash
cd firmware/button_coproc/pico
pio run -e rpipico2w_voice -t upload     # hold BOOTSEL on first flash
pio device monitor                       # 115200
```

The plain `rpipico2w` env still builds a button-only coprocessor with no audio
libraries.

## Bring-up order (do this first)

1. In `config.h` set `kVoiceEnabled = true` and the default effect to `VFX_PASS`
   (edit `VoiceState::fx`), flash `rpipico2w_voice`.
2. Speak — you should hear a **clean passthrough**. If silent, check: DAC I2C
   found (the init returns false otherwise), mic OUT on GP26, amp power/ground.
3. Switch to `VFX_PITCH` and confirm the pitch moves. If it moves the **wrong
   way**, flip the one sign noted at `step = speed - 1.0f` in `voice.cpp`.

> **Two spots that need on-hardware alignment**, both isolated in `voice.cpp`:
> the **TLV320 register init** (`dac_begin()`) uses the Adafruit library — match
> the calls to your installed version's example sketch if it won't compile — and
> the exact **earlephilhower I2S/ADC** call names. The DSP itself is verified.

## Control protocol

Over the same USB serial link as the button protocol (the Pi, or any terminal).
Lines are newline-terminated ASCII; the firmware echoes the live state back.

| Command | Effect |
|---|---|
| `VOICE 0` / `VOICE 1` | bypass / enable |
| `FX pass\|pitch\|robot\|crush\|echo` | select effect |
| `PITCH <-12..12>` | pitch shift in semitones (negative = deeper) |
| `MIX <0..100>` | wet %, blended with the dry mic |
| `PARAM robot_hz <10..4000>` | ring-mod carrier |
| `PARAM crush_bits <1..16>` | bitcrush depth |
| `PARAM crush_down <1..16>` | sample-rate reduction |
| `PARAM echo_ms <1..500>` | echo delay |
| `PARAM echo_fb <0..95>` | echo feedback % |

Reply line: `VOICE <0/1> FX <name> PITCH <semi> MIX <n>`.

**Standalone control (no Pi):** set `kVoiceToggleBtn` / `kVoiceCycleBtn` in
`config.h` to a button id (index in `kButtonPins`) to toggle voice / cycle the
effect from a physical switch. The press is still reported to the Pi as usual.

## Effects

- **pitch** — granular 2-tap delay-line shifter, ±12 semitones. The headline
  effect (deep/monster or chipmunk). Some grain shimmer is normal.
- **robot** — ring modulation; metallic/robotic, `robot_hz` sets the character.
- **crush** — bitcrush + sample-rate reduction; glitchy/lo-fi.
- **echo** — feedback delay (feedback clamped < 1, so it can't run away).

Sample rate is 16 kHz (voice band, low latency); raise `kSampleRate` in
`config.h` if you want more bandwidth at the cost of CPU headroom.
