#!/usr/bin/env python3
"""Direct-serial check of all 4 coprocessor servos (protohud must be stopped).

Proves the whole command path on real hardware: SERVOCAL sets the pulse window,
SERVOM slews, and PINS reports each channel as claimed by a servo. Movement
itself needs eyes on the ears -- this confirms the firmware accepts and acts on
every channel.
"""
import glob
import sys
import time

import serial

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
print(f'[port] {port}')

sp = serial.Serial(port, 115200, timeout=0.4)
# The HELLO burst on connect will swallow an early query -- let it land first.
time.sleep(1.5)
sp.reset_input_buffer()


def cmd(line, wait=0.05):
    sp.write((line + '\n').encode())
    sp.flush()
    time.sleep(wait)


def pins():
    sp.reset_input_buffer()
    cmd('PINS', 0.6)
    out = []
    deadline = time.time() + 1.5
    while time.time() < deadline:
        ln = sp.readline().decode(errors='replace').strip()
        if not ln:
            break
        out.append(ln)
    return out


def servo_pin_roles(dump):
    """Pull the servo-role lines out of a PINS dump: 'PIN <gp> <val> <role>'."""
    roles = {}
    for ln in dump:
        parts = ln.split()
        if len(parts) >= 4 and parts[0] == 'PIN' and 'servo' in parts[3]:
            roles[int(parts[1])] = parts[3]
    return roles


print('\n=== baseline PINS (before any servo command) ===')
base = servo_pin_roles(pins())
for gp in sorted(base):
    print(f'  GP{gp:<3} {base[gp]}')
if not base:
    print('  (no servo roles reported -- unexpected)')

print('\n=== calibrating all 4 channels to a full 500-2500us window ===')
for ch in range(4):
    cmd(f'SERVOCAL {ch} 500 2500')
    print(f'  SERVOCAL {ch} 500 2500')

print('\n=== sweeping each channel: 60deg -> 120deg -> 90deg at 120deg/s ===')
print('    (watch the ears -- each channel moves on its own, in order)')
for ch in range(4):
    print(f'  -- channel {ch} (GP{[34, 35, 36, 38][ch]})')
    for deg in (60, 120, 90):
        cmd(f'SERVOM {ch} {deg} 120', 0.05)
        print(f'     SERVOM {ch} {deg} 120')
        time.sleep(1.0)          # let the slew finish before the next step

print('\n=== PINS after driving every channel ===')
after = servo_pin_roles(pins())
for gp in sorted(after):
    print(f'  GP{gp:<3} {after[gp]}')

print('\n=== result ===')
expect = {34: 'servo0', 35: 'servo1', 36: 'servo2', 38: 'servo3'}
ok = True
for gp, want in expect.items():
    got = after.get(gp, '<missing>')
    # An idle channel reports "servoN(idle)"; a driven one drops the suffix.
    live = got == want
    print(f'  GP{gp:<3} expect {want:<8} got {got:<15} '
          f'{"DRIVEN" if live else "NOT DRIVEN"}')
    ok = ok and live
print('\nALL FOUR CHANNELS DRIVEN' if ok else '\nSOME CHANNELS DID NOT ATTACH')
sp.close()
