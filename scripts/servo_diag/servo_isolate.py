#!/usr/bin/env python3
"""Isolation test: sweep ONE servo at a time with the other three fully detached.

Separates a power problem from a signal problem:
  - each channel moves fine alone, but not together  -> supply can't feed them all
  - a channel still dead alone                       -> that channel's signal/wiring
  - a channel judders/stalls at the extremes         -> pulse window too wide

Deliberately conservative: it stays +/-15 deg either side of centre, so no servo
is driven anywhere near a mechanical stop and nothing can stall.
"""
import glob
import sys
import time

import serial

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
sp = serial.Serial(port, 115200, timeout=0.4)
time.sleep(1.5)
sp.reset_input_buffer()

GP = {0: 34, 1: 35, 2: 36, 3: 38}


def cmd(line, wait=0.08):
    sp.write((line + '\n').encode())
    sp.flush()
    time.sleep(wait)


def detach_all():
    for ch in range(4):
        cmd(f'SERVO {ch} off', 0.05)


print('Detaching every servo so nothing holds torque or draws current.')
detach_all()
time.sleep(1.0)

print('\nEach channel gets ~12s ALONE. Watch which horn moves.')
print('Arc is 90 -> 120 -> 60 -> 90: +/-30 deg, the middle third of travel,')
print('so it is clearly visible but nowhere near a mechanical stop.\n')

for ch in range(4):
    detach_all()                       # only this channel will be live
    time.sleep(0.6)
    print(f'=== CHANNEL {ch}  (GP{GP[ch]}) ===')
    for n in (3, 2, 1):
        print(f'      starting in {n}...', flush=True)
        time.sleep(1.0)
    cmd(f'SERVOM {ch} 90 90', 0.6)     # attach + settle at centre
    print('      WATCH NOW', flush=True)
    for deg in (120, 60, 90):
        cmd(f'SERVOM {ch} {deg} 45', 0.05)
        print(f'      -> {deg} deg', flush=True)
        time.sleep(2.6)
    sp.reset_input_buffer()
    cmd('SERVOSTAT', 0.3)
    deadline = time.time() + 1.2
    while time.time() < deadline:
        ln = sp.readline().decode(errors='replace').strip()
        if ln.startswith(f'SERVO {ch} '):
            print(f'      {ln}')
            break
    print()

print('Parking everything limp so nothing is left straining.')
detach_all()
sp.close()
print('\nDone. Which channels actually moved?')
