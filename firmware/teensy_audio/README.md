# teensy_audio — ProtoHUD 6-channel Mic Array (Teensy USB bridge — fallback)

> **Preferred path**: wire the ICS-43434 mics directly to CM5 I2S GPIO and use
> the `overlays/cm5-6mic.dts` device tree overlay. The CM5 native driver has
> lower latency and no USB overhead.
>
> Use this Teensy firmware only if the CM5 I2S driver does not support three
> simultaneous DIN lines (driver/hardware limitation).

Turns a **Teensy 4.1** into a 6-channel USB Audio device that streams the six
ICS-43434 MEMS microphone signals to the CM5 over a single USB-C cable.
Change `config.json` → `audio.capture_device` to `"hw:CARD=TeensyAudio,DEV=0"`
when using this path.

## Wiring

| Mic role   | I2S bus    | L/R pin | Teensy data pin | USB ch |
|------------|------------|---------|-----------------|--------|
| FRONT\_L   | I2S1 quad  | GND     | pin 8           | 0      |
| FRONT\_R   | I2S1 quad  | VDD     | pin 8           | 1      |
| SIDE\_L    | I2S1 quad  | GND     | pin 6           | 2      |
| SIDE\_R    | I2S1 quad  | VDD     | pin 6           | 3      |
| REAR\_L    | I2S2       | GND     | pin 5           | 4      |
| REAR\_R    | I2S2       | VDD     | pin 5           | 5      |

Shared clocks:
- I2S1: SCK = pin 21, WS = pin 20
- I2S2: SCK = pin 33, WS = pin 34

All mics share VDD (3.3 V) and GND from the Teensy 3.3 V rail.

## Required: 6-channel USB Audio patch

By default, Teensyduino's USB Audio descriptor only exposes **2 channels**.
To get 6-channel capture on the CM5:

1. Locate `usb_desc.h` in your Teensyduino hardware files:
   ```
   ~/.arduino15/packages/teensy/hardware/avr/<version>/cores/teensy4/usb_desc.h
   ```
   (on Windows: `%LOCALAPPDATA%\Arduino15\...`)

2. Find the `AUDIO_INTERFACE_DESC_SIZE` or `USB_AUDIO_IN_CHANNELS` definition
   and change it from `2` to `6`.

3. Alternatively, use the pre-patched `usb_desc.h` available in the
   [Teensy 6-ch audio fork](https://github.com/PaulStoffregen/cores) — look
   for community patches in the Teensy forum audio section.

4. After patching, select **USB Type → Audio** (or "Serial + Audio") and build.

### Verify on CM5

```bash
aplay -l | grep Teensy
# Should show: hw:CARD=TeensyAudio,DEV=0

arecord -D hw:CARD=TeensyAudio,DEV=0 -c 6 -r 48000 -f S16_LE -d 3 test.wav
ffprobe test.wav   # should report 6 channels
```

## Arduino IDE build settings

| Setting  | Value              |
|----------|--------------------|
| Board    | Teensy 4.1         |
| USB Type | Audio (or Serial+Audio for debug) |
| CPU      | 600 MHz            |
| Optimize | Faster             |

## Channel map (matches `src/audio/mic_array.h`)

```
ch 0  FRONT_L   azimuth 330°  (front-left, 30° to the left of straight ahead)
ch 1  FRONT_R   azimuth  30°  (front-right)
ch 2  SIDE_L    azimuth 270°  (pure left)
ch 3  SIDE_R    azimuth  90°  (pure right)
ch 4  REAR_L    azimuth 210°  (rear-left)
ch 5  REAR_R    azimuth 150°  (rear-right)
```
