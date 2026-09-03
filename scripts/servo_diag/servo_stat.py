#!/usr/bin/env python3
"""Ask the firmware whether each servo channel REALLY attached (fw >= 1.5.0).

att=1 attached, att=0 attach failed (no free PIO state machine), att=-1 never
tried. A failed attach still shows on=1, which is why the earlier PINS-only
check looked like a pass.
"""
import glob
import sys
import time

import serial

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
sp = serial.Serial(port, 115200, timeout=0.4)
time.sleep(1.5)                      # let the HELLO burst land first
sp.reset_input_buffer()


def cmd(line, wait=0.08):
    sp.write((line + '\n').encode())
    sp.flush()
    time.sleep(wait)


def drain(tag, secs=1.2):
    out, deadline = [], time.time() + secs
    while time.time() < deadline:
        ln = sp.readline().decode(errors='replace').strip()
        if ln:
            out.append(ln)
        elif out:
            break
    for ln in out:
        print(f'  {ln}')
    return out


print('=== SERVOSTAT before any command (nothing attached yet) ===')
cmd('SERVOSTAT', 0.3)
drain('pre')

print('\n=== driving all 4 channels (attach happens on first SERVOM) ===')
for ch in range(4):
    cmd(f'SERVOM {ch} 90 120', 0.15)
    print(f'  sent SERVOM {ch} 90 120')
# Any attach failure prints a SERVOERR line unprompted.
print('\n  --- unprompted output (SERVOERR lines appear here) ---')
drain('err', 1.5)

print('\n=== SERVOSTAT after ===')
cmd('SERVOSTAT', 0.3)
rows = drain('post', 2.0)

print('\n=== verdict ===')
bad = []
for ln in rows:
    if not ln.startswith('SERVO '):
        continue
    f = dict(p.split('=', 1) for p in ln.split()[2:] if '=' in p)
    ch = ln.split()[1]
    att = f.get('att')
    state = {'1': 'ATTACHED', '0': 'ATTACH FAILED', '-1': 'never tried'}.get(att, att)
    print(f"  ch{ch} gp={f.get('gp'):<3} on={f.get('on')} -> {state}")
    if att != '1':
        bad.append(ch)
print('\nall four attached' if not bad
      else f'\nCHANNELS THAT DID NOT ATTACH: {", ".join(bad)}')
sp.close()
