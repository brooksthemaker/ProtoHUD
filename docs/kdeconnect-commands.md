# KDE Connect commands — drive ProtoHUD from your phone

KDE Connect's **Run Command** plugin lets the paired phone trigger actions on the
Pi. ProtoHUD already triggers commands on the *phone* (Communications → Phone →
Run Command); this is the **other direction** — your phone as a remote for the
helmet.

There are two kinds of command:

| Group | How it runs | Needs |
|---|---|---|
| **System** | runs a ProtoHUD script / power action directly | nothing extra |
| **In-app** | writes a `GpioFunc` id to the **command FIFO**, dispatched like a button | `inputs.command_fifo.enabled` |

## 1. The command FIFO (in-app commands)

ProtoHUD can watch a local FIFO and run any `GpioFunc` written to it — the same
functions the GPIO buttons / button coprocessor use. Enable it in `config.json`:

```jsonc
"inputs": {
  "command_fifo": { "enabled": true, "path": "/run/protohud/cmd" }
}
```

Then any tool can drive ProtoHUD:

```bash
echo menu_open        > /run/protohud/cmd
echo cam_capture_left > /run/protohud/cmd
echo phone_ring       > /run/protohud/cmd
```

Valid ids (same as `gpio.pins` functions): `menu_open menu_select menu_back
cam_capture_left cam_capture_right cam_af_left cam_af_right cam_pip_left
cam_pip_right cam_swap boop_snout boop_left boop_right boop_both phone_ring
system_restart system_shutdown`, plus **face jumps** `face_neutral face_happy
face_angry face_sad face_surprised` and `face_return` (snap back to the
previously-set face), and **material jumps** `material_rainbow material_pride
material_progress material_trans material_bisexual material_pansexual
material_lesbian material_nonbinary material_asexual material_genderfluid
material_genderqueer material_aromantic material_intersex`, plus **camera/display
helpers** `cam_capture_stereo rec_toggle cam_zoom_in cam_zoom_out nv_toggle
theater_toggle xr_recenter`, and **face browse/look** `face_next face_prev
material_next effect_next face_bright_up face_bright_down face_restart`. Unknown
lines are ignored (untrusted input is length-bounded). Disabled by default; never
required.

## 2. Register the commands in KDE Connect

`scripts/kdeconnect_commands.py` merges a sensible default set into each paired
device's runcommand config (idempotent — merges by name, keeps your own):

```bash
python3 scripts/kdeconnect_commands.py                 # system + in-app commands
python3 scripts/kdeconnect_commands.py --no-fifo       # system commands only
python3 scripts/kdeconnect_commands.py --fifo /tmp/protohud/cmd
python3 scripts/kdeconnect_commands.py --print         # just print the JSON
```

Then reconnect the phone (or `kdeconnect-cli --refresh`) so it re-reads them.
The commands appear under the device's **Run command** screen in the KDE Connect
app. If the script can't find your config it prints the JSON to paste into the
GUI instead.

It installs:

- **System:** Restart ProtoHUD, Update & Restart, Rollback Update, Reboot Pi,
  Shut Down. (Reboot/Shutdown/`sudo` need passwordless sudo — see
  `scripts/install_sudoers.sh`.)
- **In-app (FIFO):** Open/Close Menu, Menu Select/Back, Capture Photo L/R,
  Autofocus L/R, Swap Cameras, Ring My Phone, Boop reactions, Capture Stereo,
  Record Toggle, Night Vision, Zoom In/Out, Theater Mode, Recenter Display,
  Next Expression, Next Material, Next Effect, Face Brighter/Dimmer, Reboot Face.

## 3. Pi → phone bits (already in the menu)

- **Communications → Phone (KDE Connect) → Export to Phone → Send Last Capture** —
  shares the newest photo/recording with the phone.

## How it fits

```
phone (KDE Connect Run Command)
   ├─ system cmd ─────────────► scripts/*.sh / sudo
   └─ in-app cmd: echo <id> ──► /run/protohud/cmd ─► CmdFifo ─► gpio_dispatch(GpioFunc)
                                                                   └─ menu / camera / boop / system
```

`input::CmdFifo` (`src/input/cmd_fifo.{h,cpp}`) shares the exact `gpio_dispatch`
the GPIO poller and button coprocessor use, so nothing downstream knows the press
came from the phone.
