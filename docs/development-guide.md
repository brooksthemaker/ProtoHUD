# ProtoHUD Development Guide

How the code is put together, what each part does, and how to change it without
breaking the rest. Every section explains itself twice: **In one line** for the
quick mental model, then **How it works** for the detail, then **Talks to** for
the connections and **To modify** for the practical part.

New here? Read [§1 The big picture](#1-the-big-picture) and
[§3 The five patterns](#3-the-five-patterns-everything-uses), then jump to the
subsystem you're changing and the matching recipe in
[§6 Recipes](#6-recipes--how-do-i-).

---

## 1. The big picture

ProtoHUD is **two computers and one C++ process**:

```
              ┌──────────────────── CM5 (Raspberry Pi) ────────────────────┐
 cameras ───▶ │  ProtoHUD (src/, one process)                              │ ──▶ VITURE XR glasses
 IMU (I2C1) ─▶│    render loop → HUD + menu → XR side-by-side output       │ ──▶ HUB75 face panels
 phone/LoRa ─▶│    face renderer → panel outputs                           │      (via panel_driver.py)
              └───────────────▲────────────────────────────────────────────┘
                              │  one USB-CDC serial cable ("the trunk")
              ┌───────────────▼──────── RP2350 coprocessor ────────────────┐
 switches ──▶ │  firmware/button_coproc/pico (Arduino / earlephilhower)    │ ──▶ MAX7219 matrices
 boop pads ─▶ │    core0: buttons, protocol, SPI bridge, peripherals       │ ──▶ fans (PWM)
 mic ───────▶ │    core1: voice changer DSP                                │ ──▶ voice speaker
 DS18B20 ───▶ └────────────────────────────────────────────────────────────┘
```

**Design rule (the "GPIO policy"):** the CM5's 40-pin header carries only the
HUB75 bonnet and the head-tracking IMU. Every other GPIO peripheral hangs off
the RP2350 over the trunk. If you're adding hardware, it almost certainly goes
on the coprocessor — see the [peripheral hub recipe](#add-a-coprocessor-protocol-verb).

**Repo map:**

| Path | What lives there |
|---|---|
| `src/` | the ProtoHUD application (C++17, one binary) |
| `firmware/button_coproc/` | RP2350 coprocessor firmware (PlatformIO) |
| `firmware/teensy_audio/` | legacy Teensy mic-array bridge (fallback) |
| `lora_bridge/` | RAK4631 LoRa radio firmware |
| `Protoface/` | the original Python face daemon (legacy `mode: "daemon"`) |
| `scripts/` | build / run / update / flash / panel driver |
| `config/config.example.json` | every config key, documented inline via `_note` keys |
| `docs/` | focused guides (coprocessor, voice changer, KDE Connect, BNO086, this file) |
| `hardware/`, `overlays/` | carrier-board docs, device-tree overlays |

---

## 2. Build, run, flash

**Host (on the Pi):**

```bash
scripts/install_deps.sh      # once — apt build dependencies
scripts/build.sh             # cmake + ninja → build/protohud
scripts/run.sh               # run with the local config
scripts/update.sh --restart  # git pull + rebuild + restart (rollback.sh undoes it)
```

**Coprocessor firmware:**

```bash
scripts/install_coproc_tools.sh          # once — PlatformIO + picotool + udev rule
scripts/flash_coproc.sh --build          # build rpipico2w_voice env + flash over USB
                                         # (no BOOTSEL button — 1200-baud reset)
```

The firmware reports `fw=<version>` in its HELLO line; bump `kFwVersion` in
`firmware/button_coproc/pico/include/config.h` with every firmware change so
flashes are verifiable.

**There is no CI.** Nothing verifies a change until you compile it on the Pi
(`scripts/build.sh`) and, for firmware, `pio run` — do both before opening a PR.

---

## 3. The five patterns everything uses

Learn these once and most of the codebase reads itself.

### 3.1 `AppState` + one mutex

**In one line:** almost all shared state lives in one struct (`src/app_state.h`),
guarded by one mutex.

**How it works:** `AppState state` is created in `main()` and passed by
reference everywhere. Threads take `std::lock_guard<std::mutex> lk(state.mtx)`
for the shortest possible time — snapshot what you need, unlock, then work.
High-rate producers (audio levels, IMU samples) bypass the mutex with
`std::atomic` members instead, because the render thread holds `state.mtx` for
whole ticks.

**To modify:** add fields to the right sub-struct (`FaceState`, `SystemHealth`,
`KnobState`…). Never call anything slow (disk, serial) while holding the lock —
`NativeFaceController::serialize_state_locked` vs `write_state_file` shows the
split pattern to copy.

### 3.2 config.json: load → mutate → save-at-exit

**In one line:** one JSON file is parsed at startup into locals/structs, menus
mutate the live values, and `main()` writes the JSON back at shutdown.

**How it works:** `main.cpp` reads `config.json` (nlohmann::json `cfg`) near the
top and has a long save block near the bottom (search `cfg["protoface"]` in the
save region). Menu actions either mutate the parsed structs (persisted by that
save block) or write into `cfg` directly for immediate persistence.

**To modify:** a new option touches **three** places — the parse (with a
default so old configs keep working), the save block, and
`config/config.example.json` with a `_yourkey_note` explaining it. Grep an
existing key (e.g. `pride_sharp`) to see all three.

### 3.3 The `GpioFunc` input bus

**In one line:** every button-like input — GPIO pins, coprocessor buttons, the
command FIFO, KDE Connect phone commands — becomes a `GpioFunc` enum value
dispatched through one switch.

**How it works:** `src/input/gpio_function.h` defines the enum plus
name/id maps. Sources call `gpio_dispatch(func)` which *queues* the action
(`post_input`) so it runs on the render thread; `gpio_apply` in `main.cpp` is
the single switch that acts on it. Nothing downstream knows where a press came
from.

**To modify:** see [Add a triggerable action](#add-a-triggerable-action-gpiofunc).

### 3.4 Swappable backends behind small interfaces

**In one line:** anything with hardware alternatives sits behind an interface
so backends hot-swap.

**How it works:** two seams matter most:
- `IFaceController` (`src/serial/face_controller.h`) — who animates the face:
  `TeensyController` (legacy MCU), `ProtoFaceController` (Python daemon), or
  `NativeFaceController` (in-process C++, the default). `FaceProxy` lets menus
  hold one pointer while the backend changes underneath.
- `PanelOutput` (`src/face/panel_output.h`) — where rendered face pixels go:
  `ShmPusherOutput` (→ `/dev/shm` → `panel_driver.py` → HUB75),
  `Max7219PanelOutput`, `NeoPixelMatrixOutput`, and `TeePanelOutput` which fans
  one canvas to several backends at once (how HUB75 + MAX7219 run together).

**To modify:** new hardware = implement the small interface, add a case to the
factory (`pf_build_panel_output` in `main.cpp` for outputs), done. Don't teach
the renderer about hardware.

### 3.5 The coprocessor trunk protocol

**In one line:** the CM5 and RP2350 speak newline-delimited ASCII verbs over
one USB serial link; both ends ignore unknown lines.

**How it works:** documented in `docs/coprocessor-input.md`. Up: `HELLO`, `BTN`,
`BOOP`, `TEMP`, `PING`, `I2C`. Down: `PONG`, `CFG`, `LED`, `PINCFG`, `SPI`,
`I2CSCAN`, `FAN`, plus the voice-changer verbs. Host side lives in
`src/input/coproc_inputs.cpp` (reader thread, length-bounded lines, treats all
bytes as untrusted); firmware side in
`firmware/button_coproc/pico/src/main.cpp` `handle_line()`. Each message is one
`write()` call, so concurrent senders never interleave mid-line.

**To modify:** see [Add a coprocessor protocol verb](#add-a-coprocessor-protocol-verb).

---

## 4. Subsystem tour — the CM5 application

### `src/main.cpp` — the orchestrator

**In one line:** constructs everything, wires everything, runs the render loop.

**How it works:** it's big (~6k lines) but linear: parse config → construct
subsystems (cameras, face, sensors, radios, inputs) → build the menu context →
GLFW/GLES render loop (camera textures → HUD → menu → SBS output) → save
config on exit. Cross-subsystem glue lives here as lambdas (`gpio_apply`,
`fire_boop`, `swap_backend`, `pf_max7219_apply`…), captured by reference —
safe because `main`'s stack outlives every thread.

**Talks to:** literally everything; if two subsystems need each other, the
wire is a lambda defined here.

**To modify:** search for a sibling of what you're adding and copy its shape.
Order matters: some objects are constructed early but wired late (e.g.
coproc-output fans start only after the coprocessor link exists) — put your
wiring where the dependencies already exist, and re-arm anything a "reload"
lambda recreates (see `coproc_wire_peripherals`).

### Face pipeline — `src/face/`

**In one line:** renders the LED face (expressions, materials, effects) to an
RGB canvas each tick and hands it to the configured panel output(s).

**How it works:**
- `native_face_controller.{h,cpp}` — the render thread. Owns per-panel
  `FaceState` (expression/blink/wiggle timing), `FaceLoader` (PNG art),
  `BaseMaterial` (colour), `ParticleSystem` (effects), `GifPlayer`. Composites
  everything (`renderer.cpp`), applies `glitch.cpp` post-effects, then calls
  `output_->show(canvas)`. Auto-saves its look to `protoface_state.json`
  (serialize under lock, write after unlock).
- `materials.{h,cpp}` — the material spec grammar
  (`gradient:<dir>:<mode>:<speed>:<hex-…>`, `solid:`, `zone:`, PNG names).
  Presets live in `NativeFaceController::preset_material`; user-facing
  preferences (pride sharp bands, rotation) are applied by
  `material_for_index`.
- Panel outputs (see §3.4). `max7219_chain.cpp` speaks the MAX7219 register
  protocol over spidev, bit-banged GPIO, or the coprocessor (`Transport::Coproc`
  ships the exact bytes as `SPI <cs> <hex>` lines).
- `max_section_controller/content` — the *independent* content (symbols, 5×7
  text, patterns) for MAX "section" panels, triggered via menu/FIFO/buttons.
- `scripts/panel_driver.py` — a separate Python process that reads the
  `/dev/shm/protoface_frame` canvas and drives HUB75 through Adafruit
  piomatter. ProtoHUD launches and supervises it.

**Talks to:** menu (via `IFaceController` + `MenuBuildContext` callbacks),
audio (`set_audio_drive` → mouth), sensors (`set_motion` → wiggle/heading
effects), boop (`trigger_boop`).

**To modify:** new material/effect → `materials.cpp` or `particles.cpp` plus a
preset id and a menu entry. New LED hardware → new `PanelOutput`. The face art
pipeline (PNG folders, versions, editor) is `face_loader.cpp` +
`menu/face_editor.cpp`.

### HUD — `src/hud/`

**In one line:** draws everything you see in the glasses that isn't the menu:
compass, health panel, toasts, PiPs, LoRa arms, clock.

**How it works:** `hud_renderer.cpp` is immediate-mode drawing (NanoVG +
ImGui draw lists) called every frame from the render loop with a snapshot of
`AppState`. `toast_renderer.cpp` owns notification toasts;
`background_library.cpp` the landing-page backgrounds.

**To modify:** find the `draw_*` function nearest to what you want (e.g.
`draw_compass_ring`, `draw_lora_indicator`) and extend it; take state via the
snapshot, never lock inside a draw call.

### Menu — `src/menu/`

**In one line:** a data-driven tree of `MenuItem`s built once at startup; the
items hold lambdas that read/write live state.

**How it works:** `menu_system.{h,cpp}` renders/navigates the tree and hosts
overlays (file picker, on-screen keyboard, face editor). The tree is built by
`build_menu.cpp`, which calls one `build_<tab>_menu(ctx)` per top-level tab
(`build_vision/hud/face_display/gpio-related parts of system/files/
communications/system/quick.cpp`). `MenuBuildContext` (`build_menu.h`) is the
bag of pointers/callbacks main.cpp wires so builders can touch live objects.
`item_factories.h` has the vocabulary: `leaf`, `leaf_sel`, `toggle`, `slider`,
`submenu`, `color_picker`, `with_desc`, `with_panel` (context-panel drawer),
`make_dynamic_rows` (fixed rows, live `visible_fn`).

Everything displayed can be **live**: `label_fn`, `visible_fn`, `warn_fn`, and
`context_panel_draw` are re-evaluated every frame — that's how the pin
visualizer re-shapes when you switch boards without rebuilding the menu.

**To modify:** see [Add a menu item](#add-a-menu-item-with-a-live-context-panel).
Gotcha: the tree is built once, so anything that must react later needs a
`*_fn`, not a value computed at build time. Heavy actions (backend rebuilds)
belong behind an explicit "Apply" leaf, not a slider callback.

### Input — `src/input/`

**In one line:** all the ways a human triggers something, funnelled into the
`GpioFunc` bus (§3.3).

**How it works:** `gpio_inputs.cpp` polls local GPIO via libgpiod v2;
`coproc_inputs.cpp` is the trunk reader (buttons + boop/temp/I2C replies +
fan/pin-map senders); `cmd_fifo.cpp` watches `/run/protohud/cmd` for id lines
(plus a raw-handler hook for parametric commands like `max_text:HELLO`);
`gamepad_input.cpp` (SDL2) and `wireless_controller.cpp` (ESP-NOW bridge) map
to the same dispatch. `gpio_buttons.cpp` holds the editable pin-slot model the
GPIO menu edits.

**To modify:** new *meaning* = new `GpioFunc` (recipe below). New *source* =
construct it in main.cpp with `gpio_dispatch`, mirror `CoprocInputs`' lifecycle.

### Sensors — `src/sensor/`

**In one line:** each physical sensor gets a small class that polls on its own
thread and publishes into `AppState`.

**How it works:** IMUs (`bno08x` preferred — vendored CEVA SH-2 lib in
`vendor/sh2`; `bno055`, `mpu9250` fallbacks) feed heading/head-tracking;
`mpr121_boop_sensor` (local pads) and the coprocessor `BOOP` path both end at
`fire_boop`; `light_sensor` feeds auto-brightness. Sensor *selection* is config
(`imu_source: auto|…`).

**To modify:** copy an existing sensor's shape (config struct → class with
`start/stop` + thread → publish under `state.mtx` → menu/HUD readout). If the
sensor is I2C/1-Wire/analog and non-latency-critical, prefer putting it on the
coprocessor instead (peripheral hub) and just parsing a new trunk verb.

### Serial devices — `src/serial/` + `src/protocols.h`

**In one line:** framed-binary USB devices (Teensy face, SmartKnob, RAK4631
LoRa) share one `SerialPort` reader with a `0xAA 0x55 … CRC8` frame protocol.

**How it works:** `serial_port.cpp` owns the port + reader thread + frame
state machine; `protocols.h` defines opcodes/payloads. `lora_radio.cpp` decodes
node positions/messages into `AppState` (`lora_nodes`, `lora_messages`);
`smartknob.cpp` turns encoder detents into menu navigation;
`teensy_controller.cpp`/`protoface_controller.cpp` are the non-native face
backends. (The ASCII trunk in §3.5 is deliberately separate and simpler.)

**To modify:** new opcode = add to `protocols.h`, handle in the device class's
`on_frame`, keep payloads little-endian packed. New USB device = new class on
its own `SerialPort`, matched by stable `/dev/serial/by-id/` path — never a
bare `ttyACMn`.

### Cameras & XR — `src/camera/`

**In one line:** gets camera frames to GL textures with as few copies as
possible, and knows about the VITURE glasses.

**How it works:** `camera_manager.cpp` orchestrates; `dma_camera.cpp` is the
zero-copy libcamera/CSI path (DMA-buf → EGLImage), `v4l2_camera.cpp` handles
USB cams (MJPEG), `viture_camera.cpp` the glasses' extras. The render loop
composites per-eye backgrounds, PiPs, and the SBS output; head-tracking
timewarp uses the glasses' IMU pose.

**To modify:** camera controls thread through `camera_manager` → menu
(`build_vision.cpp`) → config `cameras.*.controls`. Be careful in the DMA
path — buffer lifetime bugs show up as GPU faults, not crashes.

### Audio — `src/audio/`

**In one line:** ALSA duplex engine; output routing plus a voice analyzer that
drives the face's mouth.

**How it works:** `audio_engine.cpp` owns the ALSA loop and output selection
(glasses/headphones/HDMI); `voice_analyzer.cpp` classifies visemes and levels,
which main forwards via `face_proxy.set_audio_drive`. (The *voice changer* is
NOT here — it's on the coprocessor, deliberately independent of the CM5.)

### System & net — `src/sys/`, `src/net/`, `src/integrations/`

**In one line:** fans, pin-map data for the visualizers, system/scheduler
monitors; Wi-Fi/BT/weather/ping monitors; the KDE Connect DBus bridge.

**Notables:** `sys/fan_controller.cpp` (temp→duty curve; `output: "gpio"`
bit-bangs CM5 pins, `output: "coproc"` ships duties over the trunk),
`sys/gpio_pinmap.h` + `sys/pico_pinmap.h` (pure-data pin tables + board
variants driving both visualizers), `integrations/kdeconnect_bridge.cpp`
(DBus; notifications in, run-commands + ring out — see
`docs/kdeconnect-commands.md`).

---

## 5. Subsystem tour — the coprocessor firmware

`firmware/button_coproc/pico/` — PlatformIO, earlephilhower arduino-pico core.
Two build envs: `rpipico2w` (buttons only) and `rpipico2w_voice`
(+`VOICE_CHANGER` +`MAX_BRIDGE` +`PERIPHERAL_HUB`). Every feature is an
`#ifdef` so the plain build stays dependency-free.

| File | In one line | Detail |
|---|---|---|
| `include/config.h` | every pin + timing constant | button map defaults (PINCFG can override at runtime), voice pins (I2S GP16-18, I2C0 GP20/21, reset GP22, mic GP26), MAX SPI1 (GP10/11/13), hub pins (1-Wire GP19, fans GP14/15), `kFwVersion` |
| `src/main.cpp` | core0: buttons + protocol | runtime pin table (debounce → `BTN <id> SHORT/LONG`), `handle_line()` dispatch for every downlink verb, `I2CSCAN` (validates the pin pair *before* touching the bus), MAX `SPI` relay, HELLO/PING |
| `src/voice.{h,cpp}` | core1: the voice changer | ADC mic → pitch/robot/crush/echo → I2S → TLV320. Paced by the blocking I2S write; control via `VOICE/FX/PITCH/MIX/PARAM`. Bring-up notes flagged inline — start with the passthrough effect |
| `src/peripherals.{h,cpp}` | core0: the peripheral hub | MPR121 boop poll (shared I2C0), bit-banged 1-Wire + Maxim ROM search for DS18B20s (one probe read per loop pass so buttons never stall), fan PWM. Emits `BOOP`/`TEMP`, consumes `FAN` |

**Firmware rules:** core0 work must be cooperative (no blocking > ~10 ms —
budget against the 15 ms debounce); core1 is the audio realtime domain; treat
every host byte as untrusted (bounded lines, validated ranges); anything
user-rewirable belongs in the runtime pin map, anything peripheral-specific in
`config.h`.

---

## 6. Recipes — "How do I…"

### Add a triggerable action (GpioFunc)

1. `src/input/gpio_function.h`: add the enum value before `Count`, plus its
   `gpio_func_name` (menu label) and `gpio_func_id` (config string) cases —
   `gpio_func_from_id` derives automatically.
2. `src/main.cpp` `gpio_apply`: add the `case` that performs it. Post-only
   context — you're on the render thread; take `state.mtx` briefly if needed.
3. Add the id string to the function list in `config.example.json`'s
   `gpio._about`.
   Now it's assignable to every switch (both boards), the FIFO, and KDE Connect.

### Add a menu item (with a live context panel)

1. Pick the tab builder (`src/menu/build_<tab>.cpp`) and the parent submenu.
2. Build with the factories: `toggle`/`slider`/`leaf_sel` holding lambdas that
   read/write your live object (usually via a pointer wired through
   `MenuBuildContext` from main.cpp — add a field there if needed).
3. `with_desc(...)` for the help text; `with_panel(item, "Title", drawer)` for
   a context panel — the drawer is `(ImDrawList*, ImVec2 origin, ImVec2 size)`
   and runs every frame (see the MAX7219 wiring diagram or `hub_preview` for
   worked examples).
4. Anything that must update after build time goes in `label_fn`/`visible_fn`/
   `warn_fn`, not the static label.

### Add a config option

Parse with a default → write in the save block → document in
`config.example.json` with a `_note`. Three places, always. Validate the JSON
(`python3 -c "import json;json.load(open('config/config.example.json'))"`).

### Add a coprocessor protocol verb

1. **Firmware:** handle it in `handle_line()` (or your feature's
   `*_handle_command`), or emit it from your service. One line per message;
   validate every field; unknown lines must stay ignored.
2. **Host:** send in the relevant class (one `write()` per line), or parse in
   `CoprocInputs::on_line` (tokenise with `next_tok`, treat as untrusted,
   `connected_.store(true)` on any valid line).
3. Document it in the protocol tables in `docs/coprocessor-input.md`, and bump
   `kFwVersion`.

### Add a new sensor

CM5-local: copy `mpr121_boop_sensor` or `sensor::TempSensors` (config struct →
threaded poller → publish to `AppState` → menu rows). On the coprocessor
(preferred for new GPIO hardware): extend `peripherals.cpp` with a cooperative
service + a new uplink verb, then parse it in `CoprocInputs` (previous recipe).

### Add a face material preset

Add the spec string in `NativeFaceController::preset_material` (next free id),
mirror it in the menu table in `build_face_display.cpp` (`pf_mats` /
`pf_pride`), and check `material_for_index` if the preset should respect the
pride sharp/rotation preferences.

### Add a panel output backend

Implement `PanelOutput` (`open/show/close`, plus `covered_regions` +
`supports_face_editor` if the pixel editor should target it), add a branch in
`pf_build_panel_output` (main.cpp) keyed on `protoface.backend` or a tee, and
give it a config block.

---

## 7. Conventions & gotchas

- **Threading:** the render thread owns GL and the menu; input sources *queue*
  via `post_input`. `state.mtx` guards `AppState`; hold it briefly, never
  around I/O. High-rate producers use atomics.
- **Reload lambdas recreate objects.** If you attach a handler to something a
  `*_reload` lambda rebuilds (GPIO poller, coprocessor link), re-attach inside
  the reload too — `coproc_wire_peripherals` is the pattern.
- **Untrusted inputs everywhere:** serial links, the FIFO, and config values
  get bounded lengths, validated ranges, and silent-ignore on garbage. A flaky
  cable must degrade to "offline", never crash a reader thread.
- **Comments explain *why*, config explains itself.** Every non-obvious block
  has a short rationale comment; every config key has a `_note` sibling in the
  example file. Match the density around your change.
- **Pin budgets are documented, not remembered.** CM5: see
  `hardware/carrier-board/PINMAP.md` and the GPIO visualizer. RP2350: the
  fixed assignments live in firmware `config.h` and are echoed in the pin
  editor's description — update both when you claim a pin.
- **Menus are cheap, rebuilds are not.** Sliders/pickers should mutate state
  only; expensive effects (renderer/backend rebuilds) go behind an explicit
  "Apply" item (`pf_max7219_apply` and the HUB75 layout editor are the model).
- **Stable device paths.** Multiple CDC devices race for `ttyACMn` — always
  match `/dev/serial/by-id/…`.
- **Verify before merging:** `scripts/build.sh` on the Pi, `pio run` for
  firmware, and a click-through of any menu you touched. There is no CI to
  catch you.
