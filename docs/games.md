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

Two sources ship today:

| Source | Notes |
| ------ | ----- |
| **Snake** | Built-in demo, no assets. Validates the whole pipeline. |
| **Doom**  | [doomgeneric](https://github.com/ozkl/doomgeneric) submodule. Needs a WAD. |

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
