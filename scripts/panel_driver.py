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
    ap.add_argument('--shm', default='/dev/shm/protoface_frame')
    ap.add_argument('--fps', type=float, default=60.0, help='poll rate cap')
    args = ap.parse_args()

    W, H = args.canvas_w, args.canvas_h
    size = 1 + W * H * 3
    print(f"[panel_driver] starting: canvas {W}x{H}, panel {args.panel_w}x{args.panel_h}, "
          f"chain {args.chain}, parallel {args.parallel}, shm {args.shm}", flush=True)

    # Piomatter geometry — identical to Protoface's hub75.py.
    n_addr  = addr_lines(args.panel_h)
    n_lanes = 2 * args.parallel
    width   = args.panel_w * args.chain
    height  = n_lanes << n_addr
    pixelmap = simple_multilane_mapper(width, height, n_addr, n_lanes)
    geometry = piomatter.Geometry(width=width, height=height, n_addr_lines=n_addr,
                                  n_planes=10, n_temporal_planes=4,
                                  map=pixelmap, n_lanes=n_lanes)
    fb = np.zeros((height, width, 3), dtype=np.uint8)
    matrix = piomatter.PioMatter(colorspace=piomatter.Colorspace.RGB888Packed,
                                 pinout=piomatter.Pinout.Active3,
                                 framebuffer=fb, geometry=geometry)
    print("[panel_driver] piomatter ready", flush=True)

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
                # Panels display R->G->B rotated; resend as (G,B,R) to correct.
                fb[:] = frame[:, :, [1, 2, 0]]
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
