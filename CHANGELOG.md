# ProtoHUD — Changelog

## Week of Jul 6 – Jul 9, 2026

This week: the reactions/sensors branch merged (**PR #249**), and the Face
Display menu got a ground-up redesign around per-expression styling and a
trigger-recipe system (**PR #250**, open), plus a pick-and-place → LED-map
converter for the custom APA102 panels, faster compass/attitude response, and
a floating-terminal keyboard fix.

---

### 🟢 Merged — PR #249 ("reactions, sensors, attitude indicator, led_panels")

- **Reactions engine** (IMU-stillness Awake→Drowsy→Asleep with a snap-awake
  motion spike), **attitude indicator** with speed-adaptive smoothing, the
  Set-Level fix, frost/water physics rewrites, an efficiency pass, faster boot,
  the floating terminal/mirror window, F-row deep-menu shortcuts, coprocessor
  test pins with TTP223 boops, and the **`led_panels`** face backend (custom
  APA102/SK9822 panels driven straight from CM5 SPI — up to ~2000 LEDs/panel).

### 🟡 In progress — PR #250 ("Face Display redesign + per-expression styles, custom expressions, trigger recipes")

**Faces and Expressions — one list, edited in place**
- The Face Display menu is now four groups: **Base Settings**, **Faces and
  Expressions**, **Accessory LEDs**, **Gifs and Text**. The ProtoTracer/Teensy
  source picker is hidden by default (`protoface.show_prototracer` restores it;
  wiring intact).
- **Expressions** is a single list of built-in slots *and* custom slots (5
  seeded + **Add Expression…**, up to 24). A custom expression borrows a
  built-in's PNG for art and adds its own name + style. Everything edits in
  place as normal menu levels — no pop-up editor — so the Face Preview panel
  stays visible while styling.

**Per-expression styling (universal)**
- Every expression — built-in or custom — can carry its own **Material**,
  **Effect**, and **Glitch**, overriding the inherited **Default Style** (the
  former top-level Material Color / Effects / Glitch) only while it's showing.
  A three-layer resolver (default → expression style → override slot) re-runs
  on every face switch, so restore is automatic.
- Material: inherit / presets / Pride / Custom Color. Effect: inherit / None /
  Single / Premade / your saved Custom presets. Glitch: inherit / Off / preset.
- Persisted as `protoface.expression_styles`.

**Trigger recipes**
- Replaces single-event triggers with *event × count within a window, WHILE
  conditions hold* — uniform for every expression, keyed by stem or
  `custom_<slot>` in `protoface.expression_triggers`.
- Events: boop zones, APDS swipes, head shake, gets-bright / gets-dark
  (edge-detected, hysteresis). Conditions: head tilt L/R past an angle, light
  above/below a threshold, moving/still.
- **Nose boop ×5 → angry** and **left cheek while tilted left → curious** are
  each one recipe. Most-specific recipe wins; counting boops keep the default
  reaction and only the firing boop is claimed; hold time per expression
  (0 = latch). `ExpressionDirector` reworked around Rule snapshots + per-recipe
  accumulators + a conditions feed.

**On-screen keyboard**
- A symbols page (`?123`/`ABC`), a real in-string text caret (Up focuses the
  field; Left/Right move the caret), and a per-field character counter that
  reddens near the limit.

**Accessory LEDs**
- New **Blush** zone (cheek boops flash it), **Chase** and **Sparkle**
  patterns, per-zone brightness on top of the chain master, and **Follow Face**
  (a zone tracks the face canvas's mean lit colour, ~5 Hz, driver-agnostic).

**Custom LED panels — map generator**
- `scripts/pnp_to_ledmap.py`: a pick-and-place CSV → `led_panels` `map_file`
  converter. Designator order = chain order; KiCad/Altium/JLC header tolerance;
  `--pitch-px`/`--px-per-mm`/`--fit` scaling; auto X-mirror for bottom-layer
  assemblies; `--reverse` for designators numbered against the wiring; a
  chain-order preview SVG. Validated against a real 571-LED top/bottom pair.

**Fixes**
- **Compass** now uses the same speed-adaptive filter as the attitude
  indicator (was a fixed 350 ms lag) — near-raw during a turn, calm at rest;
  the attitude filter reacts sooner and its slider range was widened.
- **Floating terminal**: enabling Keyboard Focus closes the deep menu, and key
  capture is suspended while any menu is open — no more being trapped in the
  terminal with an open menu that ignores the keyboard (F10 still releases).

**Docs**
- New [`docs/face-expressions.md`](docs/face-expressions.md) (Expressions /
  styling / triggers); `docs/led-face-panels.md` gains the pick-and-place
  converter section; `CAPABILITIES.md` §9/§11 refreshed.

---

## Week of Jun 30 – Jul 5, 2026

This week: the RP2350 coprocessor grew from a button reader into a full
peripheral hub (**PR #242**, open), temperature probes arrived (**PR #243**,
open), and face materials + KDE Connect setup got quality-of-life work
(**PRs #240–#241**, merged).

---

### 🟡 In progress — PR #242 ("Coprocessor: voice changer, pin map, flashing, MAX7219, GPIO menu")

**Voice changer (RP2350 core1)**
- Real-time mic → effect → speaker on the coprocessor's second core: analog mic
  (MAX9814) → ADC → DSP → I2S → TLV320DAC3100, paced off the I2S clock so input
  and output stay sample-locked.
- Effects: granular pitch shift (±12 semitones), robot (ring-mod), bitcrush,
  echo, dry/wet mix. Driven over serial (`VOICE`/`FX`/`PITCH`/`MIX`/`PARAM`) or
  a local button; built only with `-DVOICE_CHANGER` so the plain button
  firmware is unchanged.

**Coprocessor as a configurable GPIO expander**
- Runtime pin map (`PINCFG`): which GPIO is a button, its pull/polarity and
  backlight LED are HUD config pushed on connect — no reflash to move a switch.
- New top-level **GPIO** menu (after Face Display) with **On-Board GPIO**
  (the Pi 40-pin visualizer + button map, moved out of System) and **RP2350
  GPIO Expander** (enable/status + a Pico pin visualizer/editor: colour-coded
  roles, free-pin picker, per-button function/pull/polarity/LED, live apply).
- Board variants: RP2350 (Pico 2), Pimoroni Pico Plus 2, Pico LiPo 2 XL W
  (RP2350B, GP0-47 with ADC GP40-47 + reserved pins flagged), or a raw GP0-47
  view. Pin labels carry the fixed I2C mux role (`I2C0/1 SDA/SCL`).
- `I2CSCAN` bus test: probe the coprocessor's I²C lines from the menu and see
  which addresses ACK (e.g. the TLV320 at 0x18); invalid pin pairs are rejected
  before the bus is touched.
- Flash the coprocessor from the CM5 over USB — no BOOTSEL button:
  `scripts/flash_coproc.sh` (1200-baud touch + picotool, `--build` self-builds
  via PlatformIO); `scripts/install_coproc_tools.sh` sets up the toolchain.
  Firmware reports `fw=<version>` in HELLO so updates are verifiable.

**MAX7219 panels through the coprocessor**
- The coprocessor doubles as a USB→SPI bridge (`SPI <cs> <hex>`), so MAX7219
  chains run **alongside** the HUB75 face with zero CM5 GPIO (piomatter's PIO
  owns those). `Max7219Chain` gained a `coproc` transport; a tee output drives
  HUB75 + MAX from one renderer (`mode: main` or `section`).
- In-HUD **MAX7219 Layout** editor: rows, panels-per-row (ragged grids),
  chain order, brightness — with a live **wiring diagram** (modules numbered
  DIN→DOUT, chain arrows, coproc pin legend), also overlaid in the face editor.
- Section content: triggerable symbols / 5×7 text / patterns on the panels,
  via `max_next`/`max_prev`/`max_clear` buttons, a Content Library menu, or
  FIFO commands (`max_symbol:heart`, `max_text:HELLO`, `max_pattern:bars`).

### 🟡 In progress — PR #243 ("Temperature sensors")
- DS18B20 1-Wire probes (many share one GPIO) read ~1 Hz into the HUD state;
  **System → Temperature** shows live readouts that turn amber/red at
  per-probe warn/crit thresholds. `fans.temp_path` can point at a probe file
  so the fan curve follows a helmet temperature instead of the SoC.

### Merged — PR #241 ("Face materials: mirror, rotation, pride bands")
- Gradient materials mirror at the face's centre (symmetric halves instead of
  one edge-to-edge ramp); scenic presets mirrored by default.
- Rotation for gradients (0–360°) on the Custom Gradient **and** the pride
  flags; pride flags gained a Sharp Bands toggle (hard-edged real-flag stripes
  vs smooth blends).

### Merged — PR #240 ("KDE Connect commands import JSON")
- `scripts/kdeconnect_commands.json` ships ready to import in the KDE Connect
  desktop app (29 commands); the generator gained `--json` and deterministic
  UUIDs so re-imports update in place.

---

## Week of May 26 – Jun 4, 2026

Most of this week's new feature work lives on **PR #206** (currently open / in
review). Earlier items merged via PRs #190–#205. Dates are commit/merge dates.

---

### 🟡 In progress — PR #206 ("Fix HUB75 panels staying dark after the panel-layout rework")
Started as a dark-panels fix and grew into the week's main feature branch.

**HUB75 multi-panel rendering**
- Fixed panels staying dark after the layout rework; added per-panel Flip H/V.
- Render the face as one canvas, crop each panel's slice (no more squishing),
  and flip the physical slices at output.
- Whole-face blink so both eyes close; **eye-region blink** so the eye closes
  without wiping the mouth/nose (alpha-composite + a new "Eye Region" editor
  tool that round-trips the boxes through the face's `config.json`).
- GIFs play per-panel (duplicated, not stretched) and read forwards on every
  panel.
- Fixed the panel-driver **restart race** (panels blanked but didn't come back)
  by waiting for the old `panel_driver.py` to release the PIO/DMA before
  relaunching.

**Communications (new top-level menu)**
- New "Communications" heading grouping LoRa + Phone + Notification Log.
- Notification log: scrollable/filterable rows, sender checklist, grouped by
  sender, full-text side panel, disk persistence, browse dismissed history,
  delete / quick-delete-with-confirm, **save/pin messages**, and bulk-clear.
- Toast bodies wrap to fit the block; larger chat/DM toasts; ring phone
  (in-HUD + quick-menu + GPIO function) with retry-on-reconnect; message-apps
  and ignore-list editors.
- **Scanned QR codes**: each saved to its own folder with the link, the
  grayscale decode image, and the colour camera image; a de-duplicated running
  list (no double-ups across reboots) and a browser under Communications.

**Faces, materials & effects**
- Per-expression **face versions** (named + auto-backup) with thumbnails.
- Multi-colour gradient materials + presets (removed Face Color); hex / R,G,B
  colour entry in the picker.
- Procedural animated-eye reactions triggered by rapid boops; particle effects
  suppressed during GIF playback; Protoface submenu moved to the top.

**GPIO**
- Configurable GPIO switch map (replaces the fixed 3-button controller) with
  live reload (apply edits without relaunch).
- Visual pin-picker showing only available pins, hardware/slot claim marking
  with conflict override, and a restyled pin/legend layout.

**Cameras & editor fixes**
- "Reinitialize CSI Cameras" recovery + CSI boot auto-retry + quick-menu
  diagnostics.
- Face editor: edge-triggered mouse so the two-step tools (Line / Rect /
  Eye Region) work correctly instead of self-cancelling.

**Helpers**: `restart.sh`, `update.sh`, executable script bits, plus
portability and RGB-parse / multicam-seam fixes.

---

### 🟢 Merged

**Setup & docs** — PRs #204, #205 (May 31)
- BNO055 example config block (wiring + axis notes).
- Setup docs + install scripts for BNO055, KDE Connect, multi-cam, HUB75.

**Camera latency & layout** — PR #203 (May 30–31)
- USB `BUFFERSIZE=1`, per-camera capture threads, CSI fps tuning.
- Per-eye CSI display rotation (0/90/180/270).
- Multi-cam layout: CSI top + two USB bottom.

**Phone / KDE Connect integration** — PR #197 (May 29–30)
- Phone notifications bridged into the HUD notification queue over KDE Connect
  (DBus).
- Phone Inbox: import face PNGs/GIFs from a watched directory.
- Battery indicator in the HUD chrome (fixed the DBus battery sub-object path).

**Menus & HUB75 layout groundwork** — PRs #196, #198, #199, #200 (May 28–29)
- System menu regrouped into 9 categories + Pi Settings submenu; long lists
  now scroll.
- HUB75 panel layout: presets, per-panel nudge, named save slots, per-face
  layout stamping, editor support.
- Face editor: preview-to-panels (V), live-effects view (T), input lockdown,
  full-screen mode.

**Info panel / fixes** — PRs #193, #195 (May 28)
- Rotating ring when top-docked + themed clock faces.
- Fixed GPIO Visualizer / Layered Builder crash on null cfg sections.

**Protoface backends & face editor** — merged with the above (authored May 26–27)
- **MAX7219** panel backend with bit-banged GPIO transport (HUB75 + MAX7219
  coexistence) and an **RGB-matrix** backend (WS2812 drop-in for MAX7219).
- Full-screen face editor for MAX7219 / RGB-matrix backends, with a tool suite:
  Pencil/Eraser, Bucket fill, Eyedrop, Line + Rect (two-click anchor + live
  preview), brush sizes, direct tool keys, brush-sized cursor.
- Editor chain-zone awareness: outline + label each chain bbox, pick
  eye/nose/mouth layouts with bounding boxes that follow, mouth bottom-anchored
  with nose-centre mirror and dual mouth boxes.
- Chain layout moved into Face Options with a schematic preview + adaptive,
  evenly-spaced anchors.
- Native backends: shrink canvas to face width, one face panel, disable wiggle
  / particle tint, source native frames in the panel preview.

**Sensors & reactions** — PR #192-era (May 26–27)
- **BNO055 IMU** driver + a source picker (replaces the Viture-first priority).
- **Light sensor (BH1750)** with a dark→bright squint reaction.
- Protoface boop-reaction faces + a coalesced "BothCheeks" zone.

**USB cameras** — PR #192 (May 26)
- Regrouped the USB camera deep menu + added live-feed context panels.

**Minimap & info panel** — PR #190 (May 26)
- Minimap countdown rings + a cycling info panel.

---

_Notes:_
- "This week" is bounded to **May 26 – Jun 4, 2026**.
- Everything under **PR #206** is compiling, pushed work but **not yet merged** —
  it is one large feature branch still in review.
