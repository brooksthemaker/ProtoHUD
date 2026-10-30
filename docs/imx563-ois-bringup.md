# IMX563 12MP OIS module — MIPI bring-up on Pi 5 / CM5

Goal: run the **Waveshare IMX563 12MP OIS Camera Module** (MIPI version, not
the USB one) on the CM5's CSI ports, same as the OWLsight eyes. That means
libcamera, zero-copy NV12, AF, and OIS.

## Is there a driver already?

**No** (checked October 2026). There is no IMX563 driver in mainline Linux,
in the Raspberry Pi kernel, in libcamera, or from Waveshare. Waveshare's
listing says buyers have to design their own carrier board and write their
own driver. The closest public work is Sony IMX662 support in the Pi kernel
(PR #7315). That's a different sensor family, so it doesn't help here.

So this repo carries its own stack under `drivers/imx563/`.

> **MCU note:** a microcontroller can't drive this sensor's video. Full-res
> RAW10 at 30 fps is about 3.7 Gbps over 4 D-PHY lanes, and no RP2040,
> RP2350, ESP32 or STM32 has a CSI-2 receiver that fast. Use the Pi 5/CM5.
> Jetson, i.MX8M Plus and RK3588 would also work with the same kernel driver,
> but their DT and ISP tuning are different. An MCU *can* handle the I²C
> side: VCM, OIS, or capturing the init sequence.

## What the module is

Specs as Waveshare publishes them. Items marked ⚠ conflict or are missing.

| | |
|---|---|
| Sensor | Sony IMX563, 1/2.55", 1.4 µm pixels |
| Resolution | 4032×3024 ⚠ (the product page says 4046×3344) |
| Interface | MIPI CSI-2 D-PHY, **4 lanes**, on a 24-pin board-to-board connector |
| Lens | 2.17 mm, f/2.2, 122° FOV |
| Focus | VCM, **Dongwoon DW9800V** (I²C) |
| OIS | **AKM AK7323**, 2-axis, ±3° (I²C) |
| Not published ⚠ | B2B pinout and part number, supply voltages, I²C addresses, INCK frequency, whether a gyro is inside the module |

## The stack

```
 ProtoHUD (DmaCamera, libcamera NV12)  ── "imx563" entry in sensor_specs()
            │
 libcamera  ├─ CamHelper imx563      drivers/imx563/libcamera/cam_helper_imx563.cpp
 (rpi/pisp) ├─ sensor properties     drivers/imx563/libcamera/camera_sensor_properties.patch.txt
            └─ tuning imx563.json    TODO, start from imx477.json, then run CTT
            │
 kernel     ├─ imx563.ko   V4L2 sensor subdev    drivers/imx563/imx563.c
 (V4L2)     ├─ dw9800v.ko  lens subdev (AF)      drivers/imx563/dw9800v.c
            ├─ ak7323_ois.ko lens subdev (OIS)   drivers/imx563/ak7323_ois.c
            └─ DT overlay                        overlays/imx563-ois.dts
            │
 rp1-cfe CSI-2 receiver (4-lane) ── 22-pin FFC ── adapter PCB ── 24-pin B2B ── module
```

### State of each piece

| Piece | State | What's missing |
|-------|-------|----------------|
| `imx563.c` | Complete V4L2 framework: power sequencing, chip-ID probe, exposure/gain/blanking/flip/test-pattern controls, two modes, runtime PM. Builds cleanly with `W=1`. | **The register values (`TODO(regs)`).** These are the PLL setup, the vendor init block and mode timing. Sony doesn't publish them. Without them the sensor ACKs on I²C but won't stream. |
| `dw9800v.c` | Complete: FOCUS_ABSOLUTE, SAC ring-control, soft lens parking. | Check the SAC mode and timing values against the actuator. First try the in-tree `dongwoon,dw9807-vcm` driver, which uses the same register layout. |
| `ak7323_ois.c` | Skeleton: power, PM, `V4L2_CID_IMAGE_STABILIZATION`, a mode menu (Off/Centering/Still/Movie) and Hall X/Y readback controls. | **The entire register map** (AKM keeps it under NDA). Also unknown: where the gyro data comes from (see below). |
| Overlay | Compiles. Works on CAM0 or CAM1 with 4 lanes, links the VCM through `lens-focus`. | The real I²C addresses. Find them with the probe tool. |
| libcamera | Cam helper and properties entry are written. | Tuning file. The 122° lens needs its own lens-shading (ALSC) calibration. |
| ProtoHUD | `imx563` is listed in the mode table and under Sensor Model → Waveshare. | Nothing else. Once the modes are real, it shows up like any other CSI sensor. |
| Adapter PCB | Requirements are below. | Schematic and layout. |

## Hardware: the 24-pin → Pi adapter

The module only has a B2B connector, so it needs a small adapter PCB. It
takes the Pi-standard **22-pin 0.5 mm FFC** in (the Pi 5/CM5 pinout carries
all four lanes) and has the module's 24-pin B2B receptacle on the other side.

On the adapter:

1. **Rails**: three LDOs enabled from the FFC's `CAM_GPIO` (the base DT drives
   it as `camN_reg`):
   - AVDD 2.8 V (sensor analog)
   - DVDD ≈1.05–1.2 V ⚠ (sensor core; the exact value is in the datasheet)
   - OVDD 1.8 V (sensor I/O and the I²C level)
   - VCM/OIS 2.8 V (actuator coils; budget about 150 mA peak with OIS active)

   Power-up order: OVDD → AVDD → DVDD → INCK → XCLR high. You can do that
   with LDO soft-start or an RC on each EN pin.
2. **INCK**: a 24 MHz 1.8 V oscillator. The Pi camera connector doesn't
   supply a clock.
3. **XCLR** (sensor reset): pull it up through an RC from OVDD, about 10 ms
   after the rails come up. Optionally route it to a spare GPIO through a
   0 Ω resistor.
4. **I²C**: the FFC's SDA/SCL are 3.3 V, so put a PCA9306 (or similar)
   level shifter to 1.8 V in between. The sensor, VCM and OIS all share
   this bus.
5. **EN_FORCE jumper**: a solder jumper that ties the LDO enables high. It
   lets `tools/imx563_probe.py` talk to the module before any kernel driver
   exists.
6. **Layout**: 100 Ω differential pairs for D0–D3 and CLK, matched within
   about 0.1 mm inside each pair and about 1 mm between pairs, with ground
   under them. Keep the board small. Most of the length should be in the
   FFC.
7. Optionally put test points on SDA/SCL/INCK/XCLR, and an I²C EEPROM
   footprint.

> **ProtoHUD carrier board:** `hardware/carrier-board/CONNECTORS.md` lists
> only 2 lanes for J7/J8. The IMX563 needs **all 4** (D2±, D3± as well).
> The 22-pin connector already has those pins, so the change is routing
> only.

Ask Waveshare support for: the B2B connector part number and pinout, the
supply voltages, the IMX563 datasheet or a register init file, and the
AK7323 settings. They have sent init sequences for other modules on request.
Ask before you design the PCB, because the pinout is the only thing above
you can't measure yourself.

## Getting the register values (the hard part)

In order of effort:

1. **Waveshare / Sony through a distributor.** Under NDA you get the
   datasheet and the "register setting" spreadsheet, which has every mode.
   This is the best source.
2. **Capture them from the Waveshare IMX563 OIS *USB* camera.** It's the same
   sensor, VCM and OIS chip, and its UVC bridge programs all three over I²C.
   Probe SDA/SCL on its board with a logic analyzer (Saleae, or any
   sigrok-compatible one; a $10 FX2 clone is fast enough for 400 kHz).
   Capture from power-on through streaming, once for each resolution. Then:
   ```bash
   drivers/imx563/tools/i2c_log_to_regs.py cap.csv --addr 0x1a            > sensor.inc
   drivers/imx563/tools/i2c_log_to_regs.py cap.csv --addr 0x0c --reg-bits 8 > vcm.inc
   drivers/imx563/tools/i2c_log_to_regs.py cap.csv --addr <ois> --reg-bits 8 > ois.inc
   ```
   Paste the block from before the first mode write into
   `imx563_common_regs`. Paste each per-resolution block into its mode
   table. Toggling OIS in the USB camera's settings while capturing gives
   you the AK7323 enable sequence.
   ⚠ The USB bridge probably runs fewer lanes or a lower link rate. Keep its
   0x0300–0x030F (PLL) and 0x0114 (lane count) values aside and recompute
   them for 4 lanes at 600 MHz.
3. **Vendor Android kernel and CamX trees.** Phones with an IMX563 publish
   GPL kernel sources. Qualcomm CamX sensor XML (`imx563_sensor.xml`) holds
   the full init and mode tables in plain text. Search GitHub for
   `imx563` + `camx` or `sensor_init`.

Once you have the values, also fill in or check `IMX563_CHIP_ID`,
`IMX563_EXPOSURE_OFFSET`, the analog gain max, pixel rate and link frequency
(all derived from the PLL), the line/frame lengths in `imx563_modes[]`, and
the native Bayer order.

## OIS: what to expect

The AK7323 runs the stabilisation servo itself at kHz rates. Linux only
configures it and switches modes, which is how `ak7323_ois.c` is
structured. The open question is where it gets gyro data:

- **Gyro inside the module.** This is likely, because the 24-pin connector
  has no spare pins for SPI. If so, full OIS works once the register map is
  known.
- **Gyro expected from the host over SPI.** The flex would need SPI pins,
  so check the pinout. Feeding the helmet's BNO086 to it over I²C from Linux
  isn't viable, because the latency is far too high for OIS.

Either way, **Centering** mode (servo on, lens held at centre) is useful on
a helmet on its own: it stops the floating lens barrel from rattling and
blurring on impacts. ProtoHUD's timewarp (`src/vitrue/timewarp.cpp`) already
corrects head motion at display time, so treat OIS as a bonus, not a
requirement.

From userspace (until a menu item is added):
```bash
v4l2-ctl -d /dev/v4l-subdevN -l                            # find "OIS Mode"
v4l2-ctl -d /dev/v4l-subdevN -c image_stabilization=1 -c ois_mode=2
```

## Bring-up checklist

1. **Adapter powered, EN_FORCE closed**, no driver installed:
   `sudo drivers/imx563/tools/imx563_probe.py buses`, then `scan -b N`, then
   `id -b N`. Write down the addresses and the chip ID.
2. `vcm -b N 0`, then `vcm -b N 1023`. The lens should move and click
   audibly.
3. Get the register tables (previous section) and paste them in.
4. Build and install:
   ```bash
   sudo apt install linux-headers-rpi-2712 dkms
   cd drivers/imx563 && make && sudo make install     # or: dkms add/build/install
   dtc -@ -I dts -O dtb -o imx563-ois.dtbo ../../overlays/imx563-ois.dts
   sudo cp imx563-ois.dtbo /boot/firmware/overlays/
   # config.txt: camera_auto_detect=0, dtoverlay=imx563-ois[,cam0]
   ```
   Open EN_FORCE and reboot. `dmesg | grep -iE "imx563|dw9800|ak7323"`
   should show the probe and the placeholder warning.
5. `media-ctl -p` should show imx563 → csi → pisp, with the lens linked.
6. **Test pattern first.** `v4l2-ctl -d /dev/v4l-subdevX -c test_pattern=2`,
   then capture raw with `rpicam-raw` or `cam`. Colour bars prove the PLL,
   lanes and link frequency without involving the optics.
7. Build libcamera with the helper (see `drivers/imx563/libcamera/README.md`),
   then run `rpicam-hello --list-cameras` and `rpicam-hello`.
8. Tune the image with CTT, then set `owlsight_left/right.sensor_model:
   "imx563"` or leave it on auto-detect.

## File map

```
drivers/imx563/
  imx563.c  dw9800v.c  ak7323_ois.c   kernel modules
  Makefile  dkms.conf
  libcamera/                          cam helper + properties + build notes
  tools/imx563_probe.py               raw-I²C probe, ID, dump, write, VCM move
  tools/i2c_log_to_regs.py            logic-analyzer capture → C reg table
overlays/imx563-ois.dts               CAM0/CAM1, 4-lane, sensor+VCM+OIS
```
Grep for `TODO(regs)` / `TODO(verify)` to find every placeholder.
