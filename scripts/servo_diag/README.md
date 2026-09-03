# Servo diagnostics (coprocessor)

Bench tools for the four RP2350 servo channels. Written while chasing "only the
first servo moves" (2026-07-29/30) and kept because they answer questions the
menu can't.

**All of these need the serial port, so stop the HUD first — it reconnects and
steals the port otherwise:**

```sh
sudo systemctl stop protohud.service
python3 scripts/servo_diag/<script>.py
sudo systemctl start protohud.service
```

They need `pyserial` and firmware **1.5.2+** (except `servo_check.py`, which
works on 1.4.9+).

| script | question it answers |
|---|---|
| `servo_stat.py` | Did each channel REALLY attach? (`att=1`) Or is it a live-looking channel whose writes are silently dropped? |
| `servo_probe.py` | Is a real pulse train present on each pin, and is it landing on the right pin? Measures hi/lo microseconds. |
| `servo_findpins.py` | Which header pin is each servo ACTUALLY plugged into? Sweeps every candidate GPIO with a countdown. |
| `servo_isolate.py` | Does each channel work ALONE, with the others detached and drawing nothing? Separates power from signal. |
| `servo_brownout.py` | Do all four moving at once reset the RP2350? (Detects servo V+ sharing the Pico's rail — no instruments needed.) |
| `servo_check.py` | Quick end-to-end: SERVOCAL + SERVOM on all four, before/after `PINS`. |
| `servo_seq_check.cpp` | Headless unit test of `ServoController`'s Check-All sweep state machine. No hardware. |

Build and run the C++ harness:

```sh
g++ -std=c++17 -Wall -Wextra -Isrc scripts/servo_diag/servo_seq_check.cpp \
    -o /tmp/servo_seq_check && /tmp/servo_seq_check
```

## Firmware verbs these rely on

Added for this hunt; all are permanent.

- `SERVOSTAT` — per channel: `gp/on/att/cur/tgt/spd/us`. **`att` is the one that
  matters**: `Servo::attach()` claims a PIO state machine and returns -1 if none
  is free, after which `write()` silently drops every angle. `PINS` alone cannot
  see this — it reports `g_servo_on[]`, which is set either way.
- `PROBE <gp>` — `pulseIn` on any GPIO: `hi_us`/`lo_us`/`lvl`. A healthy servo
  signal reads `hi_us` 1000-2000, `lo_us` ~18500 (50 Hz frame).
- `SWEEPPIN <gp>` — wiggle a servo on any GPIO, then release it. For finding
  which header pin a servo is really on.

## Pin map (Pico LiPo 2 XL W)

The channels are **not contiguous** — a GND and the NeoPixel line sit inside the
run, which is exactly how a tidy-looking wiring job ends up on dead pins.

| channel | GPIO | header pin |
|---|---|---|
| 0 | GP34 | 26 |
| 1 | GP35 | 27 |
| — | GND | 28 |
| 2 | GP36 | 29 |
| — | GP37 (NeoPixel data) | 30 |
| 3 | GP38 | 31 |

Signal to the GP pin; servo V+ to an **external 5-6 V supply, never the Pico's
3V3**; grounds common.
