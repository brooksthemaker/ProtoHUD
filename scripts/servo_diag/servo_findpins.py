#!/usr/bin/env python3
"""Find which header pin each servo is REALLY plugged into (fw >= 1.5.2).

Sweeps a servo signal across one GPIO at a time, with a countdown before each so
there is time to look, and two back-to-back sweeps (~5s of motion) per pin.

The four servo channels are NOT contiguous on the header -- pin 28 is a GND and
pin 30 is GP37 (NeoPixel data) -- so a tidy-looking wiring job can easily put
servos on pins that nothing drives.

Phase 1 checks the four real servo channels; phase 2 checks the neighbours a
servo could plausibly have been plugged into by mistake.
"""
import glob
import sys
import time

import serial

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
sp = serial.Serial(port, 115200, timeout=0.5)
time.sleep(1.5)
sp.reset_input_buffer()

# (gp, header pin, note). Header positions from src/sys/pico_pinmap.h (XL W).
CHANNELS = [
    (34, 26, 'servo channel 0'),
    (35, 27, 'servo channel 1'),
    (36, 29, 'servo channel 2'),
    (38, 31, 'servo channel 3'),
]
NEIGHBOURS = [
    (32, 24, ''),
    (33, 25, ''),
    (37, 30, 'NeoPixel data - LEDs may glitch briefly'),
    (39, 32, ''),
    (44, 35, ''),
    (45, 36, ''),
    (46, 37, ''),
]


def sweep_once(gp, timeout=8.0):
    sp.reset_input_buffer()
    sp.write(f'SWEEPPIN {gp}\n'.encode())
    sp.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        ln = sp.readline().decode(errors='replace').strip()
        if ln.startswith(f'SWEEPPIN {gp} '):
            return ln.split(maxsplit=2)[2]
    return '(no reply)'


def do_pin(gp, hdr, note):
    print(f'\n  ---- GP{gp}  =  HEADER PIN {hdr} ----'
          + (f'   [{note}]' if note else ''))
    for n in (3, 2, 1):
        print(f'       looking in {n}...', flush=True)
        time.sleep(1.0)
    print('       >>> SWEEPING NOW - watch the servos <<<', flush=True)
    r1 = sweep_once(gp)
    r2 = sweep_once(gp)                 # second pass, ~5s of motion total
    print(f'       done ({r1}/{r2})', flush=True)
    time.sleep(1.0)


print('Releasing all servo channels first...')
for ch in range(4):
    sp.write(f'SERVO {ch} off\n'.encode())
    sp.flush()
    time.sleep(0.05)
time.sleep(0.8)
sp.reset_input_buffer()

print('\n' + '=' * 62)
print('PHASE 1 - the four real servo channels')
print('=' * 62)
print('If all four horns move here, the wiring is right and the problem')
print('is elsewhere. Note which ones do NOT move.')
for gp, hdr, note in CHANNELS:
    do_pin(gp, hdr, note)

print('\n' + '=' * 62)
print('PHASE 2 - neighbouring pins a servo may be plugged into by mistake')
print('=' * 62)
print('Any horn that moves HERE is in the wrong hole.')
for gp, hdr, note in NEIGHBOURS:
    do_pin(gp, hdr, note)

print('\nDone. Which header pin moved which servo?')
sp.close()
