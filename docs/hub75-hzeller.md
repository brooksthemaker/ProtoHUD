# HUB75 driver backends: piomatter vs hzeller

ProtoHUD renders the LED face in C++, writes the canvas to shared memory
(`ShmPusherOutput` → `/dev/shm/protoface_frame`), and a small Python shim
(`scripts/panel_driver.py`) pushes each frame to the HUB75 panels. That shim now
supports **two interchangeable driver backends**, selected by
`protoface.hub75.driver` in `config.json`:

| | `piomatter` (default) | `hzeller` |
|---|---|---|
| Library | `adafruit_blinka_raspberry_pi5_piomatter` | `rpi-rgb-led-matrix` (module `rgbmatrix`) |
| Pin assignment | **Fixed** (Adafruit bonnet/HAT or Active-3) | **Configurable** (`regular` / `adafruit-hat` / `compute-module` / custom) |
| Parallel chains | per the bonnet | up to **6** (`compute-module` mapping) |
| Pi 5 / RP1 | native | RIO or PIO backend (`rp1_pio`) |
| System setup | none | root + disable onboard sound (+ optional core isolation) |
| Extra packages | (already used) | `rgbmatrix`, `Pillow` |

Nothing about the piomatter path changes — leaving `driver` unset (or
`"piomatter"`) behaves exactly as before. Choose `hzeller` when you need pin
freedom for a custom PCB, more parallel chains, or the library's finer tuning.

## Enabling hzeller

1. **Install the library + PIL** on the Pi:
   ```bash
   sudo apt install -y python3-pil
   pip install git+https://github.com/hzeller/rpi-rgb-led-matrix
   # module imports as `rgbmatrix`; verify: python3 -c "import rgbmatrix"
   ```
   For a fully custom pin layout, edit `lib/hardware-mapping.c` in the library
   source, rebuild, and pass that mapping's name as `hw_mapping`.

2. **Disable the onboard sound** (it fights the panel timing). ProtoHUD uses
   USB/HDMI/VITURE audio, so this is safe:
   ```bash
   # /boot/firmware/config.txt
   dtparam=audio=off
   # /etc/modprobe.d/blacklist-rgb-matrix.conf
   blacklist snd_bcm2835
   ```

3. **(Recommended) Isolate a CPU core** for a rock-steady refresh, on
   `/boot/firmware/cmdline.txt`:
   ```
   isolcpus=3 nohz_full=3 rcu_nocbs=3
   ```
   Note the audio engine pins its thread to core 3 (`src/audio/audio_engine.cpp`);
   pick a different free core for one of them if you run both.

4. **Root:** the hzeller backend needs `/dev/mem`-level access to init. Either
   run ProtoHUD via `sudo`, or grant the binary the capability. The shim starts
   with `drop_privileges = False` so it keeps the access it was launched with.

5. **Configure** `config.json`:
   ```json
   "protoface": {
     "hub75": {
       "driver": "hzeller",
       "hw_mapping": "adafruit-hat",   // or "compute-module" / "regular" / custom
       "gpio_slowdown": 2,             // tune per board
       "pwm_bits": 11,
       "rp1_pio": 1,                    // Pi 5: 0=RIO (fast), 1=PIO (low CPU)
       "pixel_mapper": ""               // e.g. "U-mapper;Rotate:180"
     }
   }
   ```

## How the two paths share code

`panel_driver.py` builds a small `Backend` (a framebuffer + a `show()` call) from
either `build_piomatter()` or `build_hzeller()`, then runs one shared loop:
read the shm sequence byte, reshape the RGB frame, apply the color-channel order
(`--order`, done in numpy for **both** backends so the panel library's own
channel order stays straight RGB), copy into the framebuffer, and `show()`.

The hzeller backend maps ProtoHUD's geometry onto `RGBMatrixOptions`
(`rows`=panel_h, `cols`=panel_w, `chain_length`=chain, `parallel`=parallel) and
pushes frames via a double-buffered `SetImage` + `SwapOnVSync` (tear-free). The
color order is applied in numpy, so `led_rgb_sequence` stays `RGB`.

## Wiring / config plumbing

- Config keys live under `protoface.hub75` (`driver`, `hw_mapping`,
  `gpio_slowdown`, `pwm_bits`, `rp1_pio`, `pixel_mapper`) and load into
  `PfHub75Layout` (`src/menu/build_menu.h`).
- `pf_launch_panel_driver()` (`src/main.cpp`) forwards them to
  `panel_driver.py` as `--driver / --hw-mapping / --gpio-slowdown / --pwm-bits /
  --rp1-pio / --pixel-mapper`. The piomatter backend accepts but ignores the
  hzeller-only flags, so the launch contract is identical for both.

## Notes & caveats

- **Pi 5/RP1 support in hzeller is newer** than its mature Pi 0–4 path; the
  `rp1_pio` attribute name can vary by binding version, so the shim sets it
  defensively (and skips it silently if the installed binding predates it).
- **Active-3** is a piomatter-only concept; on hzeller use `hw_mapping` +
  `parallel`/`chain` (or a pixel mapper) to describe multi-panel layouts.
- **License:** hzeller's library is GPLv2+. ProtoHUD invokes it out-of-process
  (a separate Python script), so there's no linking entanglement, but keep it in
  mind for redistribution.
- On a **CM4** the hzeller path is fully mature and is the best route for
  arbitrary pin layouts.
