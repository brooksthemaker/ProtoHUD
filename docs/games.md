# Games (controller-driven, on the glasses + LED face)

ProtoHUD can run a controller-driven game and show it on **both** outputs at
once: fullscreen (or windowed) on the XR glasses, and downscaled onto the
HUB75 face panels. Input comes from the gamepad (or the keyboard on the
desktop build).

## How it works

A game is a `game::GameSource` (`src/game/game_source.h`): it renders an RGBA
frame and consumes a held-button mask (D-pad, A/B/X/Y, L/R, Start/Select). The
render loop ticks the active source once per frame, uploads its frame to a GL
texture for the eyes, and mirrors it to the panels via
`NativeFaceController::set_panel_override()`. A source never touches SDL or GL,
so adding a new game is just implementing the interface.

Sources:

| Source | Included? | Notes |
| ------ | --------- | ----- |
| **Snake** | built in | Demo, no assets. Validates the whole pipeline. |
| **Doom**  | built (submodule) | [doomgeneric](https://github.com/ozkl/doomgeneric). Needs a WAD. |
| **Emulator (libretro)** | **add-on (opt-in)** | dlopen a libretro core (`.so`) + run a ROM. Not in a stock build. |

## Turning it on

- **Menu:** `Games` → `Source` (Snake / Doom) → `Play`. `Windowed` toggles a
  centred window (HUD stays up) vs. fullscreen (HUD hidden).
- **Hardware button:** assign the **`Game: Toggle`** (`game_toggle`) GpioFunc to
  a GPIO/coprocessor button to start/stop the game.

Opening the menu while a game is running stops it; re-`Play` resumes.

### Controls

| Button | Snake | Doom |
| ------ | ----- | ---- |
| D-pad / arrows | steer | move / turn |
| A / `Z` | restart | fire |
| B / `X` | — | use / open |
| X | — | run |
| Y | — | automap |
| L / R | — | strafe left / right |
| Start / `Enter` | restart | menu confirm |
| Select | — | menu (Esc) |

## Doom setup

Doom is built when `ENABLE_DOOM=ON` (the default) **and** the submodule is
present:

```sh
git submodule update --init third_party/doomgeneric
cmake -S . -B build && cmake --build build
```

If the submodule is missing, the build skips Doom automatically (the menu shows
only Snake).

### Provide a WAD

doomgeneric needs an IWAD. ProtoHUD does **not** ship one (the retail Doom WADs
are copyrighted). Use either:

- the **shareware** `doom1.wad` (free to distribute), or
- **[Freedoom](https://freedoom.github.io/)** (`freedoom1.wad` / `freedoom2.wad`, libre).

Point ProtoHUD at it in `config.json`:

```json
{
  "game": {
    "doom_wad": "/home/user/.local/share/protohud/doom.wad"
  }
}
```

The default path is `/home/user/.local/share/protohud/doom.wad`. If the file is
missing, selecting Doom shows an on-screen "IWAD not found" message instead of
crashing — the engine calls `exit()` on a bad WAD, so ProtoHUD checks the file
exists before ever handing control to Doom.

### Notes / limitations

- Doom is all-global C and can't be re-initialised, so the engine starts once
  per session (the first time you Play it). Start a new game from Doom's own
  menu rather than re-selecting the source.
- Sound is disabled (the portable core links the no-op sound stub; no SDL).
- Doom renders at 640×400; it's scaled per output (fullscreen/windowed on the
  glasses, downscaled on the panels).

## Emulator (libretro) — optional add-on

The **Emulator** source is a minimal [libretro](https://docs.libretro.com/)
frontend: it `dlopen()`s a core (the same `.so` files RetroArch uses) and runs a
ROM, mapping the controller onto the standard RetroPad. No core or ROM ships
with ProtoHUD — supply your own.

It is an **opt-in add-on**, *not* part of a stock build (`ENABLE_LIBRETRO`
defaults to **OFF**). When it isn't built, the **Emulator** entry simply doesn't
appear in the Source picker. Install it with the helper script:

```sh
scripts/install-emulator.sh           # reconfigures with -DENABLE_LIBRETRO=ON + rebuilds
```

or by hand:

```sh
cmake -S . -B build -DENABLE_LIBRETRO=ON && cmake --build build
```

It only needs the vendored `third_party/libretro/libretro.h` plus `libdl` and
the GLES/EGL libraries the app already links — no extra packages.

Point ProtoHUD at a core + ROM in `config.json`:

```json
{
  "game": {
    "libretro_core": "/usr/lib/libretro/snes9x_libretro.so",
    "libretro_rom":  "/home/user/roms/game.sfc",
    "libretro_system_dir": "/home/user/.local/share/protohud/system",
    "audio_enabled": true,
    "audio_device":  "default"
  }
}
```

`libretro_system_dir` is handed to the core as both the system and save
directory (some cores need BIOS files or write SRAM there).

### Audio

Core audio plays through a self-contained ALSA sink (a ring buffer drained by a
playback thread, so it never stalls the render loop). It runs **independently**
of ProtoHUD's spatial mic engine, so set `audio_device` to a device that can be
shared:

- `"default"` (the recommended default) routes through dmix / PipeWire and
  coexists with other audio.
- A raw `"hw:CARD=…"` device is exclusive — don't point both the emulator and
  the mic `AudioEngine` at the same `hw:` device, or one will fail to open.

Toggle sound live from **Games → Audio** (shown when the Emulator source is
selected); it mutes/unmutes the running core and is saved to
`game.audio_enabled` on exit. (`"audio_device": ""` disables audio entirely.)

ALSA soft-resamples the core's native rate (e.g. SNES 32040 Hz) to the device,
so no rate config is needed. If the device can't be opened the game still plays,
just silent.

Get cores from your distro (`sudo apt install libretro-*`), from RetroArch's
online updater, or from <https://buildbot.libretro.com/>.

### Software-rendered cores

NES (`fceumm`/`nestopia`), SNES (`snes9x`), Genesis/MD (`genesis_plus_gx`),
GB/GBC/GBA (`gambatte`/`mgba`), PC Engine, arcade (`fbneo`), PS1 software
(`pcsx_rearmed`, software renderer), … These hand back a pixel buffer (RGB565 /
0RGB1555 / XRGB8888) which the frontend converts to RGBA. Rock-solid.

### Hardware-rendered cores (N64 / Dreamcast / PSP) — experimental

These render through OpenGL rather than returning a bitmap. The frontend now
implements the libretro **OpenGL ES** hardware-render path: it hands the core an
FBO to draw into, shares ProtoHUD's GLES context (via `eglGetProcAddress`), and
reads the result back each frame (the readback is needed anyway to mirror to the
panels). So **N64** (`mupen64plus_next` / GLideN64), **Dreamcast** (`flycast`)
and **PSP** (`ppsspp`) can run.

Caveats — treat this as experimental:

- Only **OpenGL ES** cores are accepted; desktop-GL / Vulkan cores are declined.
- A core that needs **GLES 3.x** depends on ProtoHUD's actual EGL context
  supporting it on your Pi/driver — GLideN64 in particular wants GLES 3.1+. If a
  core needs more than the context provides it may fail to render; this is the
  one thing that can't be guaranteed without trying it on your hardware.
- The per-frame `glReadPixels` is a GPU→CPU stall; fine at these resolutions but
  not free.

### Core options (per-core settings)

libretro cores publish tunables — internal resolution, region (NTSC/PAL), BIOS
choice, frameskip, controller type, aspect ratio, etc. ProtoHUD reads them and
surfaces them under **Games → Core Options** (shown when the Emulator source is
selected):

- The list populates when the core loads, i.e. after the first **Play** (before
  that you'll see "start the game to load options").
- **Select** cycles a value forward, **Ctrl+Select** cycles back; the core
  applies the change live.
- Menu changes are **saved to `config.json`** (`game.libretro_options`) on exit,
  so they persist across restarts. You can also pre-seed them by hand — the
  core's own option key → value:

  ```json
  "game": {
    "libretro_options": {
      "mupen64plus-43screensize": "640x480",
      "mupen64plus-rdp-plugin":   "gliden64"
    }
  }
  ```

  (Find exact key/value names in the core's docs or RetroArch's Core Options
  screen.)

> **Doom audio:** the bundled Doom (doomgeneric) is still silent — its portable
> core links a no-op sound stub (no SDL_mixer / software synth), so there are no
> samples to route. The ALSA sink above is specific to the libretro emulator.
