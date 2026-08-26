#!/usr/bin/env python3
"""Verify commanded angle -> actual pulse width, and so diagnose short travel.

The pulse window is what decides how far a servo really moves: the library maps
0-180 deg linearly onto [min_us, max_us]. If a servo sweeps only ~45 deg for a
full 0-180 command, either the window is too narrow for that servo, or the
generated pulses do not match the window. This measures the pulses with PROBE so
the two can be told apart.

Usage: servo_travel.py [channel] [min_us] [max_us]     (default: ch0, 1000-2000)
"""
import glob
import sys
import time

import serial

ch = int(sys.argv[1]) if len(sys.argv) > 1 else 0
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
GP = {0: 34, 1: 35, 2: 36, 3: 38}[ch]

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
sp = serial.Serial(port, 115200, timeout=0.5)
time.sleep(1.5)
sp.reset_input_buffer()


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
            return int(f.get('hi_us', 0))
    return None


print(f'Channel {ch} (GP{GP}), pulse window {lo}-{hi}us')
cmd(f'SERVOCAL {ch} {lo} {hi}', 0.3)

print(f'\n  {"cmd deg":>8} {"expected us":>12} {"measured us":>12}   error')
worst = 0
for deg in (0, 45, 90, 135, 180):
    cmd(f'SERVOM {ch} {deg} 0', 0.05)     # speed 0 = snap, no slew to wait out
    time.sleep(0.9)
    want = lo + (hi - lo) * deg / 180.0
    got = probe(GP)
    if got is None:
        print(f'  {deg:>8} {want:>12.0f} {"(no reply)":>12}')
        continue
    err = got - want
    worst = max(worst, abs(err))
    print(f'  {deg:>8} {want:>12.0f} {got:>12}   {err:+.0f}us')

print(f'\n  worst error: {worst:.0f}us')
if worst <= 60:
    print('  => pulse generation is CORRECT for this window.')
    print(f'     A full 0-180 command really does deliver {lo}us..{hi}us.')
    print('     Short travel therefore means the WINDOW is too narrow for this')
    print('     servo -- widen Pulse Min/Max (menu: Servo Settings > <servo>).')
else:
    print('  => pulses do NOT match the window; the mapping is wrong, not the servo.')

print('\nParking limp.')
cmd(f'SERVO {ch} off', 0.05)
sp.close()
