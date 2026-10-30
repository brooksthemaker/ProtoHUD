# libcamera glue for the IMX563

The Pi 5 / CM5 camera stack (rp1-cfe + PiSP via libcamera) needs three
things per sensor besides the kernel driver. Rebuild libcamera after adding
them (Raspberry Pi's fork: https://github.com/raspberrypi/libcamera).

| What | Where in libcamera | Provided here |
|------|--------------------|---------------|
| Camera helper (gain law, exposure offset, delays) | `src/ipa/rpi/cam_helper/cam_helper_imx563.cpp` + one line in that dir's `meson.build` | `cam_helper_imx563.cpp` |
| Sensor properties (pixel size, test patterns, delays) | `src/libcamera/sensor/camera_sensor_properties.cpp` | `camera_sensor_properties.patch.txt` |
| ISP tuning file | `src/ipa/rpi/pisp/data/imx563.json` (+ `meson.build` list) | **not yet** — see below |

```bash
git clone https://github.com/raspberrypi/libcamera && cd libcamera
cp <ProtoHUD>/drivers/imx563/libcamera/cam_helper_imx563.cpp src/ipa/rpi/cam_helper/
# add 'cam_helper_imx563.cpp', to the rpi_ipa_cam_helper_sources list:
$EDITOR src/ipa/rpi/cam_helper/meson.build
# paste the imx563 entry from camera_sensor_properties.patch.txt:
$EDITOR src/libcamera/sensor/camera_sensor_properties.cpp
# temporary tuning: start from the closest Sony 12MP sensor
cp src/ipa/rpi/pisp/data/imx477.json src/ipa/rpi/pisp/data/imx563.json
# add "imx563.json" to src/ipa/rpi/pisp/data/meson.build
meson setup build -Dpipelines=rpi/pisp -Dipas=rpi/pisp && ninja -C build && sudo ninja -C build install
```

**Tuning**: the borrowed imx477 file gives a usable but colour-wrong image.
Proper tuning is done with Raspberry Pi's Camera Tuning Tool (`utils/raspberrypi/ctt`
in the libcamera tree) using a Macbeth chart — see the *Raspberry Pi Camera
Algorithm and Tuning Guide*. The 122° lens also needs the ALSC (lens shading)
tables re-measured; the IMX477 ones will vignette badly.
