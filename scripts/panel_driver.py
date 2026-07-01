#!/usr/bin/env python3
"""
Minimal HUB75 frame-pusher for ProtoHUD's native face renderer.

ProtoHUD renders the LED face in C++ and writes the canvas to a shared-memory
segment (ShmPusherOutput). This shim reads that segment and pushes each new
frame to the panels. Two driver backends are selectable with --driver:

  piomatter (default)  Adafruit's RP1 PIO library
                       (adafruit_blinka_raspberry_pi5_piomatter). Fixed pinouts
                       (Adafruit bonnet/HAT or Active-3), no extra system setup.
                       The proven path — behaviour is unchanged from before.

  hzeller              hzeller/rpi-rgb-led-matrix (module: rgbmatrix). Custom pin
                       mappings (regular / adafruit-hat / compute-module / a
                       recompiled custom map), the Pi 5 RP1 backends, and the
                       library's full tuning knobs. Needs root, the `rgbmatrix`
                       and `Pillow` packages, and the onboard sound disabled.
                       See docs/hub75-hzeller.md.

Both backends share the same shared-memory contract and are launched identically
from ProtoHUD (src/main.cpp pf_launch_panel_driver). ProtoHUD launches this when
its panel output mode is "shm".

Shared-memory format (matches src/serial/shm_frame_reader.h / ShmPusherOutput):
    byte 0      uint8 sequence counter (wraps at 256)
    bytes 1..N  W*H RGB, row-major (R G B ...)
"""

import argparse
import mmap
import os
import signal
import time

import numpy as np


# piomatter HUB75 bonnet wirings (validated here; the enum is resolved lazily in
# build_piomatter so the piomatter package is only needed when it's the driver).
PIOMATTER_PINOUTS = ('adafruit_bonnet', 'adafruit_bonnet_bgr', 'active3', 'active3_bgr')

# hzeller hardware mappings (which physical pins carry the HUB75 signals).
# 'compute-module' unlocks up to 6 parallel chains; a fully custom map lives in
# the library's lib/hardware-mapping.c (rebuild required) — pass its name here.
HZELLER_MAPPINGS = ('regular', 'adafruit-hat', 'adafruit-hat-pwm', 'compute-module')

# Channel-order overrides: which SOURCE channel feeds each of the panel's R, G, B
# inputs. Applied in numpy for BOTH backends, so the panel library's own channel
# order stays straight RGB and the swap lives in exactly one place. red/green
# swapped is usually 'grb', red/blue 'bgr'.
ORDERS = {
    'rgb': [0, 1, 2], 'rbg': [0, 2, 1], 'grb': [1, 0, 2],
    'gbr': [1, 2, 0], 'brg': [2, 0, 1], 'bgr': [2, 1, 0],
}


def addr_lines(panel_h: int) -> int:
    return (panel_h // 2).bit_length() - 1


class Backend:
    """A framebuffer + a show() call, produced by build_piomatter/build_hzeller.

    fb   — numpy (height, width, 3) uint8; the main loop writes each frame here.
    chan — source-channel permutation applied to the frame before show().
    show — push the current framebuffer to the physical panels.
    """
    def __init__(self, fb, width, height, chan, show):
        self.fb = fb
        self.width = width
        self.height = height
        self.chan = chan
        self.show = show


def build_piomatter(args) -> Backend:
    import adafruit_blinka_raspberry_pi5_piomatter as piomatter
    from adafruit_blinka_raspberry_pi5_piomatter.pixelmappers import simple_multilane_mapper

    pinouts = {
        'adafruit_bonnet':     piomatter.Pinout.AdafruitMatrixBonnet,
        'adafruit_bonnet_bgr': piomatter.Pinout.AdafruitMatrixBonnetBGR,
        'active3':             piomatter.Pinout.Active3,
        'active3_bgr':         piomatter.Pinout.Active3BGR,
    }

    # The Active-3 board drives parallel chains that share address lines, so it
    # needs the multilane mapper. The single-connector Adafruit bonnet has one
    # daisy-chained output, so it uses a plain geometry whose width/height are
    # the physical canvas dimensions.
    n_addr = addr_lines(args.panel_h)
    if args.pinout.startswith('active3'):
        n_lanes  = 2 * args.parallel
        width    = args.panel_w * args.chain
        height   = n_lanes << n_addr
        pixelmap = simple_multilane_mapper(width, height, n_addr, n_lanes)
        geometry = piomatter.Geometry(width=width, height=height, n_addr_lines=n_addr,
                                      n_planes=10, n_temporal_planes=4,
                                      map=pixelmap, n_lanes=n_lanes)
        chan = [1, 2, 0]      # Active-3 panels display R->G->B rotated; resend (G,B,R)
    else:
        width    = args.panel_w * args.chain
        height   = args.panel_h * args.parallel
        geometry = piomatter.Geometry(width=width, height=height, n_addr_lines=n_addr,
                                      rotation=piomatter.Orientation.Normal)
        chan = [0, 1, 2]      # straight RGB; switch to *_bgr pinout if red/blue swap
    if args.order != 'auto':
        chan = ORDERS[args.order]

    fb = np.zeros((height, width, 3), dtype=np.uint8)
    matrix = piomatter.PioMatter(colorspace=piomatter.Colorspace.RGB888Packed,
                                 pinout=pinouts[args.pinout],
                                 framebuffer=fb, geometry=geometry)
    # matrix holds a reference to fb (framebuffer=fb); the bound method keeps it alive.
    return Backend(fb, width, height, chan, matrix.show)


def build_hzeller(args) -> Backend:
    from rgbmatrix import RGBMatrix, RGBMatrixOptions
    from PIL import Image

    o = RGBMatrixOptions()
    o.rows             = args.panel_h
    o.cols             = args.panel_w
    o.chain_length     = args.chain
    o.parallel         = args.parallel
    o.hardware_mapping = args.hw_mapping
    o.gpio_slowdown    = args.gpio_slowdown
    o.pwm_bits         = args.pwm_bits
    o.brightness       = args.brightness
    o.led_rgb_sequence = 'RGB'          # the R/G/B swap is done in numpy (chan)
    o.drop_privileges  = False          # started under ProtoHUD's user; don't setuid
    if args.pixel_mapper:
        o.pixel_mapper_config   = args.pixel_mapper
    if args.pwm_lsb_ns > 0:
        o.pwm_lsb_nanoseconds   = args.pwm_lsb_ns
    if args.limit_refresh > 0:
        o.limit_refresh_rate_hz = args.limit_refresh
    if args.multiplexing >= 0:
        o.multiplexing          = args.multiplexing
    if args.row_addr_type >= 0:
        o.row_address_type      = args.row_addr_type
    if args.panel_type:
        o.panel_type            = args.panel_type
    # Pi 5 RP1 backend selection (rp1_pio: 0=RIO fast/high-CPU, 1=PIO low-CPU).
    # The attribute name/availability varies by binding version, so set it
    # defensively and don't fail the whole driver if it isn't exposed.
    for attr in ('rp1_pio', 'led_rp1_pio'):
        if hasattr(o, attr):
            setattr(o, attr, args.rp1_pio)
            break

    matrix = RGBMatrix(options=o)
    width, height = matrix.width, matrix.height
    fb = np.zeros((height, width, 3), dtype=np.uint8)

    # Double-buffer: draw into the offscreen canvas, then swap on vsync (tear-free).
    canvas = {'c': matrix.CreateFrameCanvas()}

    def show():
        img = Image.frombytes('RGB', (width, height), fb.tobytes())
        canvas['c'].SetImage(img)
        canvas['c'] = matrix.SwapOnVSync(canvas['c'])

    chan = ORDERS[args.order] if args.order != 'auto' else [0, 1, 2]
    return Backend(fb, width, height, chan, show)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--canvas-w', type=int, default=128)
    ap.add_argument('--canvas-h', type=int, default=32)
    ap.add_argument('--panel-w', type=int, default=64)
    ap.add_argument('--panel-h', type=int, default=32)
    ap.add_argument('--chain', type=int, default=2)
    ap.add_argument('--parallel', type=int, default=1)
    ap.add_argument('--driver', default='piomatter', choices=['piomatter', 'hzeller'],
                    help='panel driver backend (default piomatter)')
    ap.add_argument('--pinout', default='adafruit_bonnet', choices=sorted(PIOMATTER_PINOUTS),
                    help='[piomatter] HUB75 bonnet wiring; *_bgr swaps red/blue')
    ap.add_argument('--order', default='auto', choices=['auto'] + sorted(ORDERS),
                    help='panel color-channel order; auto = per-pinout default')
    # ── hzeller-only options (accepted but ignored by the piomatter backend) ──
    ap.add_argument('--hw-mapping', default='adafruit-hat',
                    help='[hzeller] pin mapping: regular/adafruit-hat/adafruit-hat-pwm/'
                         'compute-module/<custom>')
    ap.add_argument('--gpio-slowdown', type=int, default=2, help='[hzeller] GPIO write delay')
    ap.add_argument('--pwm-bits', type=int, default=11, help='[hzeller] color depth 1..11')
    ap.add_argument('--rp1-pio', type=int, default=1, help='[hzeller] Pi5 backend 0=RIO,1=PIO')
    ap.add_argument('--pixel-mapper', default='', help='[hzeller] e.g. "U-mapper;Rotate:180"')
    ap.add_argument('--brightness', type=int, default=100, help='[hzeller] 0..100')
    ap.add_argument('--pwm-lsb-ns', type=int, default=0, help='[hzeller] 0=library default')
    ap.add_argument('--limit-refresh', type=int, default=0, help='[hzeller] cap Hz; 0=off')
    ap.add_argument('--multiplexing', type=int, default=-1, help='[hzeller] -1=default')
    ap.add_argument('--row-addr-type', type=int, default=-1, help='[hzeller] -1=default')
    ap.add_argument('--panel-type', default='', help='[hzeller] e.g. FM6126A')
    ap.add_argument('--shm', default='/dev/shm/protoface_frame')
    ap.add_argument('--fps', type=float, default=60.0, help='poll rate cap')
    args = ap.parse_args()

    W, H = args.canvas_w, args.canvas_h
    size = 1 + W * H * 3
    print(f"[panel_driver] starting: driver {args.driver}, canvas {W}x{H}, "
          f"panel {args.panel_w}x{args.panel_h}, chain {args.chain}, "
          f"parallel {args.parallel}, order {args.order}, shm {args.shm}", flush=True)

    backend = build_hzeller(args) if args.driver == 'hzeller' else build_piomatter(args)
    fb, width, height, chan = backend.fb, backend.width, backend.height, backend.chan
    print(f"[panel_driver] {args.driver} ready ({width}x{height})", flush=True)

    # The renderer should hand us a canvas matching the physical framebuffer
    # (ProtoHUD derives --panel-*/--chain/--parallel from the same layout that
    # sizes the canvas). If they disagree — e.g. a mixed per-panel-size layout
    # that isn't a uniform chain — copy the overlapping region instead of letting
    # the blit raise and kill the driver (panels would go dark). Warn once.
    if (H, W) != (height, width):
        print(f"[panel_driver] WARNING: canvas {W}x{H} != framebuffer "
              f"{width}x{height}; copying overlap {min(W, width)}x{min(H, height)}",
              flush=True)
    copy_h, copy_w = min(H, height), min(W, width)

    # Wait for ProtoHUD to create the shm segment.
    while not os.path.exists(args.shm):
        time.sleep(0.2)
    print("[panel_driver] shm opened, driving panels", flush=True)
    fd = os.open(args.shm, os.O_RDONLY)
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)

    running = {'go': True}

    def stop(*_):
        running['go'] = False
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    last_seq = -1
    period = 1.0 / max(1.0, args.fps)
    try:
        while running['go']:
            seq = mm[0]
            if seq != last_seq:
                last_seq = seq
                buf = np.frombuffer(mm, dtype=np.uint8, count=W * H * 3, offset=1)
                frame = buf.reshape((H, W, 3))
                # Channel order (chan) is applied here for both backends.
                fb[:copy_h, :copy_w] = frame[:copy_h, :copy_w, chan]
                backend.show()
            time.sleep(period)
    finally:
        fb[:] = 0           # blank on exit
        backend.show()
        time.sleep(0.1)
        mm.close()
        os.close(fd)
        print("panel_driver stopped.")


if __name__ == '__main__':
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        raise
