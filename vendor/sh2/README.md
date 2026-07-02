# CEVA SH-2 library (vendored)

Source: https://github.com/ceva-dsp/sh2 (core `*.c` / `*.h`, fetched at the repo
default branch). License: **Apache-2.0** (per-file headers preserved).

Used by `src/sensor/bno08x.cpp` to talk to a **BNO086 / BNO08x** sensor over
SHTP. This driver provides the transport HAL (I2C + INT/RST via the raw
`linux/gpio.h` line-request ABI); the library handles the SH-2 protocol and
sensor-report decoding.

Built as a small static lib (`add_library(sh2 STATIC …)`) in the top-level
`CMakeLists.txt`. To update, re-fetch the same file set from upstream.
