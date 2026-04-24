# CM5 Device Tree Overlays

## cm5-6mic — 6-channel ICS-43434 mic array (direct I2S)

Connects six **ICS-43434** MEMS microphones directly to the CM5's I2S GPIO,
bypassing the Teensy entirely. The CM5 kernel captures all six channels as a
native ALSA device; the ProtoHUD spatial processor runs in userspace.

### Wiring

```
CM5 GPIO     Signal    Description
────────     ──────    ───────────────────────────────────────────
GPIO 18      PCM_CLK   I2S bit clock (BCK) — driven by CM5, shared by all mics
GPIO 19      PCM_FS    I2S frame sync (LRCK) — driven by CM5, shared by all mics
GPIO 20      PCM_DIN0  FRONT pair data  (FRONT_L + FRONT_R)
GPIO 21      PCM_DIN1  SIDE pair data   (SIDE_L  + SIDE_R)
GPIO 25      PCM_DIN2  REAR pair data   (REAR_L  + REAR_R)
3V3          VDD       ICS-43434 supply (all 6 mics)
GND          GND       Common ground
```

#### L/R channel select per mic
Each stereo pair has two ICS-43434 mics on the same DATA line:

| Mic role  | L/R pin | Occupies slot |
|-----------|---------|---------------|
| FRONT\_L  | GND     | LRCK = LOW    |
| FRONT\_R  | VDD     | LRCK = HIGH   |
| SIDE\_L   | GND     | LRCK = LOW    |
| SIDE\_R   | VDD     | LRCK = HIGH   |
| REAR\_L   | GND     | LRCK = LOW    |
| REAR\_R   | VDD     | LRCK = HIGH   |

### Build & Install

```bash
# On CM5 (or cross-compile with dtc):
dtc -@ -I dts -O dtb -o cm5-6mic.dtbo cm5-6mic.dts
sudo cp cm5-6mic.dtbo /boot/firmware/overlays/

# Enable in /boot/firmware/config.txt:
echo "dtoverlay=cm5-6mic" | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

### Verify

```bash
# List capture devices
arecord -l
# Expected: card X: sndrpii2s0, device 0: ...

# 3-second test capture (6ch, 48kHz, 16-bit)
arecord -D hw:CARD=sndrpii2s0,DEV=0 \
        -c 6 -r 48000 -f S16_LE -d 3 /tmp/test.wav

# Inspect channels
ffprobe /tmp/test.wav 2>&1 | grep Audio
# Expected: Audio: pcm_s16le, 48000 Hz, 5.1(side), s16, 4608 kb/s
```

### ALSA config.json entry

```json
"audio": {
  "capture_device": "hw:CARD=sndrpii2s0,DEV=0",
  ...
}
```

### Channel map (matches src/audio/mic_array.h)

```
ALSA ch 0  →  FRONT_L  (azimuth 330°)
ALSA ch 1  →  FRONT_R  (azimuth  30°)
ALSA ch 2  →  SIDE_L   (azimuth 270°)
ALSA ch 3  →  SIDE_R   (azimuth  90°)
ALSA ch 4  →  REAR_L   (azimuth 210°)
ALSA ch 5  →  REAR_R   (azimuth 150°)
```

### Troubleshooting

| Symptom | Fix |
|---------|-----|
| `arecord -l` shows no sndrpii2s0 | Check `dmesg | grep i2s`, verify GPIO ALT function with `pinctrl get 18-21,25` |
| Only 2 channels captured | Kernel I2S driver may not support multiple DIN lines; try `dtparam=i2s=on` first |
| Clicking / xruns | Increase `period_size` and `n_periods` in config.json; pin CPU governor to `performance` |
| All channels identical | Check L/R select wiring (one mic per pair must have L/R at GND, the other at VDD) |

### Fallback: Teensy USB Audio bridge

If BCM2712's PCM block does not support 3 simultaneous DIN lines (driver
limitation), use the Teensy 4.1 as a USB Audio bridge instead:

```
ICS-43434 mics  →  Teensy I2S  →  USB  →  CM5 ALSA
```

Change `capture_device` to `"hw:CARD=TeensyAudio,DEV=0"` and flash the sketch
from `firmware/teensy_audio/teensy_audio.ino`.
