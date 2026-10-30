#!/usr/bin/env python3
"""IMX563 module bring-up probe — talk to the sensor/VCM/OIS over raw I2C.

Step 1 of bring-up, before any kernel driver: prove the adapter board powers
the module, find the real I2C addresses, read the chip ID, and wiggle the
lens. Uses /dev/i2c-N directly (no dependencies).

Prerequisites:
  * module powered — on the ProtoHUD adapter close the EN_FORCE jumper, or
    load the overlay with the imx563 driver *not* installed (otherwise the
    driver owns the bus and runtime-PM keeps the module powered off);
  * 24 MHz INCK running (adapter oscillator) — without it the sensor ACKs
    nothing on some Sony parts.

Examples:
  imx563_probe.py buses                 # which /dev/i2c-N are the CSI buses
  imx563_probe.py scan  -b 4            # list responding addresses
  imx563_probe.py id    -b 4            # read chip ID at likely Sony addresses
  imx563_probe.py dump  -b 4 -a 0x1a 0x0000 0x40
  imx563_probe.py write -b 4 -a 0x1a 0x0100 0x01
  imx563_probe.py vcm   -b 4 512        # move the focus lens (DW9800V)
"""

import argparse
import ctypes
import fcntl
import glob
import os
import sys
import time

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001

# Addresses worth trying. Sony mobile sensors are usually 0x1a or 0x10
# (SID pin); DW98xx VCMs sit at 0x0c; AKM OIS parts vary.
SENSOR_ADDRS = (0x1a, 0x10, 0x1c, 0x34, 0x36)
KNOWN = {
    0x0c: "DW9800V VCM?",
    0x10: "Sony sensor (SID=0)?",
    0x1a: "Sony sensor (SID=1)?",
    0x50: "EEPROM/OTP (lens shading / AF calibration)?",
}
CHIP_ID_REG = 0x0016


class I2cMsg(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_uint8))]


class I2cRdwr(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(I2cMsg)), ("nmsgs", ctypes.c_uint32)]


class Bus:
    def __init__(self, num):
        self.fd = os.open(f"/dev/i2c-{num}", os.O_RDWR)

    def close(self):
        os.close(self.fd)

    def _xfer(self, msgs):
        arr = (I2cMsg * len(msgs))(*msgs)
        fcntl.ioctl(self.fd, I2C_RDWR, I2cRdwr(arr, len(msgs)))

    @staticmethod
    def _msg(addr, flags, data):
        buf = (ctypes.c_uint8 * len(data))(*data)
        return I2cMsg(addr, flags, len(data), buf), buf

    def write(self, addr, data):
        m, _ = self._msg(addr, 0, bytes(data))
        self._xfer([m])

    def read_reg(self, addr, reg, n=1, reg_bits=16):
        """Write the register address, repeated-start, read n bytes."""
        ra = reg.to_bytes(reg_bits // 8, "big")
        w, _ = self._msg(addr, 0, ra)
        r, rbuf = self._msg(addr, I2C_M_RD, bytes(n))
        self._xfer([w, r])
        return bytes(rbuf)

    def write_reg(self, addr, reg, values, reg_bits=16):
        self.write(addr, reg.to_bytes(reg_bits // 8, "big") + bytes(values))

    def ping(self, addr):
        try:
            r, _ = self._msg(addr, I2C_M_RD, bytes(1))
            self._xfer([r])
            return True
        except OSError:
            return False


def cmd_buses(_args):
    for dev in sorted(glob.glob("/sys/bus/i2c/devices/i2c-*")):
        try:
            name = open(os.path.join(dev, "name")).read().strip()
        except OSError:
            name = "?"
        print(f"{os.path.basename(dev):8s} {name}")
    print("\nThe CSI buses are usually the ones named after the camera ports "
          "(i2c_csi_dsi*/rp1 i2c on Pi 5).")


def cmd_scan(args):
    bus = Bus(args.bus)
    found = [a for a in range(0x03, 0x78) if bus.ping(a)]
    bus.close()
    if not found:
        print("nothing answered — check power (EN_FORCE), INCK, and the bus number")
        return 1
    for a in found:
        print(f"0x{a:02x}  {KNOWN.get(a, '')}")
    return 0


def cmd_id(args):
    bus = Bus(args.bus)
    addrs = [args.addr] if args.addr is not None else SENSOR_ADDRS
    hit = False
    for a in addrs:
        try:
            v = int.from_bytes(bus.read_reg(a, CHIP_ID_REG, 2), "big")
        except OSError:
            continue
        hit = True
        tag = "  <-- IMX563" if v == 0x0563 else ""
        print(f"0x{a:02x}: reg 0x{CHIP_ID_REG:04x} = 0x{v:04x}{tag}")
    bus.close()
    if not hit:
        print("no sensor responded at", ", ".join(f"0x{a:02x}" for a in addrs))
        return 1
    print("\nPut the address in overlays/imx563-ois.dts (or sensor-addr=) and "
          "the value in IMX563_CHIP_ID if it isn't 0x0563.")
    return 0


def cmd_dump(args):
    bus = Bus(args.bus)
    rb = args.reg_bits
    for off in range(0, args.length, 16):
        n = min(16, args.length - off)
        data = bus.read_reg(args.addr, args.start + off, n, rb)
        print(f"{args.start + off:0{rb // 4}x}: " + " ".join(f"{b:02x}" for b in data))
    bus.close()
    return 0


def cmd_write(args):
    bus = Bus(args.bus)
    bus.write_reg(args.addr, args.reg, args.values, args.reg_bits)
    bus.close()
    return 0


def cmd_vcm(args):
    """DW9800V: wake, enable ring control, then step to the target."""
    bus = Bus(args.bus)
    a = args.addr if args.addr is not None else 0x0c
    pos = max(0, min(1023, args.position))
    bus.write_reg(a, 0x02, [0x00], 8)        # control: power up
    time.sleep(0.001)
    bus.write_reg(a, 0x02, [0x02], 8)        # ring (SAC) enable
    bus.write_reg(a, 0x03, [pos >> 8, pos & 0xff], 8)
    print(f"VCM @0x{a:02x} -> {pos}. Watch the preview (or listen for a click).")
    bus.close()
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    num = lambda s: int(s, 0)

    sub.add_parser("buses").set_defaults(fn=cmd_buses)

    for name, fn in (("scan", cmd_scan), ("id", cmd_id)):
        s = sub.add_parser(name)
        s.add_argument("-b", "--bus", type=int, required=True)
        s.add_argument("-a", "--addr", type=num)
        s.set_defaults(fn=fn)

    s = sub.add_parser("dump")
    s.add_argument("-b", "--bus", type=int, required=True)
    s.add_argument("-a", "--addr", type=num, required=True)
    s.add_argument("--reg-bits", type=int, choices=(8, 16), default=16)
    s.add_argument("start", type=num)
    s.add_argument("length", type=num)
    s.set_defaults(fn=cmd_dump)

    s = sub.add_parser("write")
    s.add_argument("-b", "--bus", type=int, required=True)
    s.add_argument("-a", "--addr", type=num, required=True)
    s.add_argument("--reg-bits", type=int, choices=(8, 16), default=16)
    s.add_argument("reg", type=num)
    s.add_argument("values", type=num, nargs="+")
    s.set_defaults(fn=cmd_write)

    s = sub.add_parser("vcm")
    s.add_argument("-b", "--bus", type=int, required=True)
    s.add_argument("-a", "--addr", type=num)
    s.add_argument("position", type=int, help="0..1023")
    s.set_defaults(fn=cmd_vcm)

    args = p.parse_args()
    try:
        return args.fn(args)
    except PermissionError:
        print("permission denied — run with sudo or add yourself to the i2c group")
        return 1
    except OSError as e:
        print(f"I2C error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
