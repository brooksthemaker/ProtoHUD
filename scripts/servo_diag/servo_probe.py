#!/usr/bin/env python3
"""Measure where the servo pulse trains ACTUALLY come out (fw >= 1.5.1).

Attaches all four servos, then probes both the intended pins and the pins the
signal would land on if the PIO's pin field were truncated to 5 bits relative to
a GPIO base of 16 (gp & 31, +16). A servo signal reads hi_us ~1000-2000.
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

INTENDED = {0: 34, 1: 35, 2: 36, 3: 38}
# If PINCTRL took (pin & 31) against a GPIO base of 16, the signal lands here:
ALIAS = {ch: 16 + (gp & 31) for ch, gp in INTENDED.items()}


def cmd(line, wait=0.08):
    sp.write((line + '\n').encode())
    sp.flush()
    time.sleep(wait)


def probe(gp):
    sp.reset_input_buffer()
    sp.write(f'PROBE {gp}\n'.encode())
    sp.flush()
    deadline = time.time() + 3.0
    while time.time() < deadline:
        ln = sp.readline().decode(errors='replace').strip()
        if ln.startswith(f'PROBE {gp} '):
            f = dict(p.split('=', 1) for p in ln.split()[2:] if '=' in p)
            return int(f.get('hi_us', 0)), int(f.get('lo_us', 0)), f.get('lvl')
    return None


print('Attaching all four servos at 90 deg (1500us with a 1000-2000 window)...')
for ch in range(4):
    cmd(f'SERVOM {ch} 90 0', 0.15)      # speed 0 = snap, no slew to wait for
time.sleep(1.5)
sp.reset_input_buffer()


def show(title, pins):
    print(f'\n=== {title} ===')
    print(f'  {"gp":<5} {"hi_us":>7} {"lo_us":>7}  verdict')
    found = {}
    for ch, gp in sorted(pins.items()):
        r = probe(gp)
        if r is None:
            print(f'  GP{gp:<3} {"(no reply)":>16}')
            continue
        hi, lo, lvl = r
        servo_like = 700 <= hi <= 2600 and lo > 10000
        verdict = ('SERVO PULSE' if servo_like
                   else 'silent' if hi == 0 and lo == 0
                   else 'activity, not servo-shaped')
        print(f'  GP{gp:<3} {hi:>7} {lo:>7}  ch{ch}: {verdict}')
        found[ch] = servo_like
    return found

on_intended = show('intended servo pins', INTENDED)
on_alias    = show('base-16 truncation aliases (gp & 31 + 16)', ALIAS)

print('\n=== verdict ===')
good = [c for c, v in on_intended.items() if v]
bad = [c for c, v in on_intended.items() if not v]
print(f'  pulse present on intended pin: {good if good else "none"}')
print(f'  MISSING on intended pin:       {bad if bad else "none"}')
aliased = [c for c, v in on_alias.items() if v]
if aliased:
    print(f'  !! servo-shaped pulses ALSO on alias pins for channels {aliased}')
    print('     -> the PIO is driving the wrong pad (5-bit pin field vs GPIO base)')
elif bad:
    print('  no pulses on the alias pins either -> the signal is not being')
    print('     generated at all for those channels, despite attach reporting OK')
else:
    print('  all four intended pins carry a proper servo pulse train')
    print('  -> signal generation is FINE; the fault is electrical/mechanical')

print('\nParking limp.')
for ch in range(4):
    cmd(f'SERVO {ch} off', 0.05)
sp.close()
