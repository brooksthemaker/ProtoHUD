#!/usr/bin/env python3
"""
Minimal HUB75 frame-pusher for ProtoHUD's native face renderer.

ProtoHUD renders the LED face in C++ and writes the canvas to a shared-memory
segment (ShmPusherOutput). This shim only reads that segment and pushes each new
frame to the panels via Adafruit Piomatter — the proven driver path — so no
Python rendering daemon is needed. ProtoHUD launches this when its panel output
mode is "shm".

Shared-memory format (matches src/serial/shm_frame_reader.h / ShmPusherOutput):
    byte 0      uint8 sequence counter (wraps at 256)
    bytes 1..N  W*H RGB, row-major (R G B ...)

Usage: panel_driver.py [--canvas-w 128] [--canvas-h 32]
                       [--panel-w 64] [--panel-h 32] [--chain 2] [--parallel 1]
                       [--pinout adafruit_bonnet] [--order auto]
                       [--shm /dev/shm/protoface_frame]
"""

import argparse
import mmap
import os
import signal
import time

import numpy as np

import adafruit_blinka_raspberry_pi5_piomatter as piomatter
from adafruit_blinka_raspberry_pi5_piomatter.pixelmappers import simple_multilane_mapper


# HUB75 bonnet wiring → piomatter pinout. "adafruit_bonnet" is the single-
# connector Adafruit RGB Matrix Bonnet/HAT; "active3" is the triple-connector
# Active-3 board. *_bgr swaps the panel's red/blue channel order.
PINOUTS = {
    'adafruit_bonnet':     piomatter.Pinout.AdafruitMatrixBonnet,
    'adafruit_bonnet_bgr': piomatter.Pinout.AdafruitMatrixBonnetBGR,
    'active3':             piomatter.Pinout.Active3,
    'active3_bgr':         piomatter.Pinout.Active3BGR,
}

# Channel-order overrides: which SOURCE channel feeds each of the panel's
# R, G, B inputs. 'auto' keeps the per-pinout default below (Active-3 takes
# a (G,B,R) rotate, the Adafruit bonnet straight RGB). Panels/bonnets with
# odd wiring pick whichever order makes the colors look right — red/green
# swapped is usually 'grb', red/blue swapped 'bgr'.
ORDERS = {
    'rgb': [0, 1, 2],
    'rbg': [0, 2, 1],
    'grb': [1, 0, 2],
    'gbr': [1, 2, 0],
    'brg': [2, 0, 1],
    'bgr': [2, 1, 0],
}


def addr_lines(panel_h: int) -> int:
    return (panel_h // 2).bit_length() - 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--canvas-w', type=int, default=128)
    ap.add_argument('--canvas-h', type=int, default=32)
    ap.add_argument('--panel-w', type=int, default=64)
    ap.add_argument('--panel-h', type=int, default=32)
    ap.add_argument('--chain', type=int, default=2)
    ap.add_argument('--parallel', type=int, default=1)
    ap.add_argument('--pinout', default='adafruit_bonnet', choices=sorted(PINOUTS),
                    help='HUB75 bonnet wiring: adafruit_bonnet (single-connector '
                         'Adafruit bonnet/HAT, default) or active3 (triple-connector); '
                         '*_bgr swaps red/blue')
    ap.add_argument('--order', default='auto', choices=['auto'] + sorted(ORDERS),
                    help='panel color-channel order; auto = per-pinout default, '
                         'or force rgb/rbg/grb/gbr/brg/bgr')
    ap.add_argument('--shm', default='/dev/shm/protoface_frame')
    ap.add_argument('--fps', type=float, default=60.0, help='poll rate cap')
    args = ap.parse_args()

    W, H = args.canvas_w, args.canvas_h
    size = 1 + W * H * 3
    print(f"[panel_driver] starting: canvas {W}x{H}, panel {args.panel_w}x{args.panel_h}, "
          f"chain {args.chain}, parallel {args.parallel}, pinout {args.pinout}, "
          f"order {args.order}, shm {args.shm}", flush=True)

    # Piomatter geometry depends on the bonnet. The Active-3 board drives parallel
    # chains that share address lines, so it needs the multilane mapper. The
    # single-connector Adafruit bonnet has one output with the panels daisy-
    # chained (serpentine for multi-row), so it uses a plain geometry whose
    # width/height are the physical canvas dimensions.
    n_addr  = addr_lines(args.panel_h)
    if args.pinout.startswith('active3'):
        n_lanes  = 2 * args.parallel
        width    = args.panel_w * args.chain
        height   = n_lanes << n_addr
        pixelmap = simple_multilane_mapper(width, height, n_addr, n_lanes)
        geometry = piomatter.Geometry(width=width, height=height, n_addr_lines=n_addr,
                                      n_planes=10, n_temporal_planes=4,
                                      map=pixelmap, n_lanes=n_lanes)
        chan = [1, 2, 0]    # Active-3 panels display R->G->B rotated; resend (G,B,R)
    else:
        width    = args.panel_w * args.chain
        height   = args.panel_h * args.parallel
        geometry = piomatter.Geometry(width=width, height=height, n_addr_lines=n_addr,
                                      rotation=piomatter.Orientation.Normal)
        chan = [0, 1, 2]    # straight RGB; switch to *_bgr pinout if red/blue swap
    if args.order != 'auto':
        chan = ORDERS[args.order]   # explicit channel order beats the pinout default
    fb = np.zeros((height, width, 3), dtype=np.uint8)
    matrix = piomatter.PioMatter(colorspace=piomatter.Colorspace.RGB888Packed,
                                 pinout=PINOUTS[args.pinout],
                                 framebuffer=fb, geometry=geometry)
    print("[panel_driver] piomatter ready", flush=True)

    # The renderer should hand us a canvas that exactly matches the physical
    # framebuffer (ProtoHUD derives --panel-*/--chain/--parallel from the same
    # layout that sizes the canvas). If they ever disagree — e.g. a mixed
    # per-panel-size layout that can't be a uniform chain — copy the overlapping
    # region instead of letting the blit raise and kill the driver (which leaves
    # the panels dark). Warn once so the mismatch is visible in the log.
    if (H, W) != (height, width):
        print(f"[panel_driver] WARNING: canvas {W}x{H} != framebuffer "
              f"{width}x{height}; copying overlap {min(W, width)}x"
              f"{min(H, height)}", flush=True)
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
                # Channel order is per-bonnet (see chan above): Active-3 needs a
                # (G,B,R) rotate; the Adafruit bonnet takes straight RGB.
                fb[:copy_h, :copy_w] = frame[:copy_h, :copy_w, chan]
                matrix.show()
            time.sleep(period)
    finally:
        fb[:] = 0           # blank on exit
        matrix.show()
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
