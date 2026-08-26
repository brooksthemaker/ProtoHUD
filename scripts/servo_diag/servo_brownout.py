#!/usr/bin/env python3
"""Drive all 4 servos together and watch for the coprocessor browning out.

If servo V+ is taken from the Pico's own 5V/VBUS rail (a common wiring mistake),
four servos moving at once sag it enough to reset the RP2350. A reset is visible
without any eyes on the hardware: the firmware re-emits HELLO on boot, and
millis() restarts. This distinguishes "servos share the Pico's supply" from
"servos have their own supply that is merely too weak".

Stays in the middle third of travel so nothing stalls against a stop.
"""
import glob
import sys
import time

import serial

port = (glob.glob('/dev/serial/by-id/usb-ProtoHUD_Buttons*-if00') or [None])[0]
if not port:
    sys.exit('no ProtoHUD Buttons serial port found')
sp = serial.Serial(port, 115200, timeout=0.2)
time.sleep(1.5)
sp.reset_input_buffer()

hellos = 0
lines = []


def pump():
    """Drain anything pending, counting HELLOs (= firmware reboots)."""
    global hellos
    while sp.in_waiting:
        ln = sp.readline().decode(errors='replace').strip()
        if not ln:
            continue
        lines.append(ln)
        if ln.startswith('HELLO'):
            hellos += 1
            print(f'  *** HELLO (firmware reset #{hellos}): {ln}')


def cmd(line, wait=0.05):
    sp.write((line + '\n').encode())
    sp.flush()
    time.sleep(wait)
    pump()


print('Attaching all four and settling at centre...')
for ch in range(4):
    cmd(f'SERVOM {ch} 90 90', 0.15)
time.sleep(1.5)
pump()

print('\nDriving ALL FOUR together, 60 <-> 120 deg, 6 passes at full speed.')
print('(Peak current is at the start of each move, all four at once.)\n')
for i in range(6):
    target = 120 if i % 2 == 0 else 60
    for ch in range(4):
        cmd(f'SERVOM {ch} {target} 300', 0.01)
    print(f'  pass {i + 1}: all -> {target} deg')
    for _ in range(14):          # ~1.4s, pumping so a reset is caught promptly
        time.sleep(0.1)
        pump()

print('\nSettling back to centre...')
for ch in range(4):
    cmd(f'SERVOM {ch} 90 90', 0.05)
time.sleep(1.5)
pump()

print('\n=== SERVOSTAT after the load test ===')
sp.reset_input_buffer()
cmd('SERVOSTAT', 0.4)
deadline = time.time() + 2.0
while time.time() < deadline:
    ln = sp.readline().decode(errors='replace').strip()
    if not ln:
        break
    print(f'  {ln}')
    if ln.startswith('HELLO'):
        hellos += 1

print('\n=== verdict ===')
if hellos:
    print(f'  {hellos} firmware RESET(S) during the test.')
    print('  => the servos are browning out the RP2350 itself, so servo V+ is')
    print('     sharing the Pico\'s supply. They need their own 5-6V source.')
else:
    print('  No firmware resets: the coprocessor held up under all four moving.')
    print('  => the Pico is NOT being browned out. If horns still do not move,')
    print('     the servo V+ rail is separate and either too weak or not fully')
    print('     wired -- not a coprocessor problem.')

print('\nParking limp.')
for ch in range(4):
    cmd(f'SERVO {ch} off', 0.05)
sp.close()
