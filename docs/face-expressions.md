# Faces, Expressions & Triggers

The **Face Display** deep menu is organised into four top-level groups:

| Group | What lives there |
|---|---|
| **Base Settings** | Hardware/backend selection, Panel Preview, Brightness, Save/Release Face Config |
| **Faces and Expressions** | The Expressions list, Default Style, Animations, Reactions, Boop, Voice, Light Sensor |
| **Accessory LEDs** | Cheek-hub / cheek-fin / blush LED zones |
| **Gifs and Text** | GIF playback slots, the scrolling-text banner |

This page covers **Faces and Expressions** — the Expressions list, per-expression
styling, custom expressions, and the trigger-recipe system.

---

## The Expressions list

`Face Display > Faces and Expressions > Expressions` is a single list holding
**every** expression:

- **Built-in slots** — Neutral, Happy, Angry, Sad, Surprised, Squint, Sleepy,
  Asleep, Blink. Their art is a PNG per slot in the active face folder.
- **Custom slots** — five empty slots to start, plus **Add Expression…** at the
  bottom (up to 24). A custom expression borrows a built-in's PNG for its art
  and adds its own name, style, and triggers.
- **Mouth Shapes / Boop Reactions / layout pickers** — the viseme and boop-face
  overlay slots and the chain/HUB75 layout tools, as before.

Everything is edited **in place** as normal menu levels (there is no pop-up
editor), so the **Face Preview** side panel stays visible while you work —
changes show live on the preview and the panels.

Each expression row contains:

| Row | Purpose |
|---|---|
| **Play** / **Edit…** / **Replace…** / **Versions** / **Clear** / **Import…** | Its art (Edit… needs an editor-capable backend — MAX7219 / RGB matrix) |
| **Material** | Its own material colour (overrides the Default Style) |
| **Effect** | Its own particle effect |
| **Glitch** | Its own glitch |
| **Triggers** | The recipes that activate it |

Custom slots add **Name…**, **Base Face**, **Activate** / **Deactivate**, and
**Clear Slot** around the same style/trigger rows.

---

## Per-expression styling

Every expression — built-in or custom — can carry its own **material**,
**particle effect**, and **glitch**. Anything left on *Default (inherit)* uses
the **Default Style** (`Faces and Expressions > Default Style`, which is the
former top-level Material Color / Effects / Glitch menus). A style set on an
expression overrides the default only while that expression is showing, and
restores it on switch-away.

### Material

`<expression> > Material`:

- **Default (inherit)** — use the Default Style's material.
- **Solid / gradient presets** — Teal, Red, Sunset, Ocean, Fire, … (the same
  palette as the Default Style).
- **Pride/** — the pride-flag gradients.
- **Custom Color** — an arbitrary RGB/hex colour via the standard colour picker.

### Effect

`<expression> > Effect`:

- **Default (inherit)** / **None**.
- **Single Effects** — one primitive (`sparkle`, `embers`, `rain`, `water`, …).
- **Premade Effects** — curated combos (`fire`, `celebration`, `galaxy`,
  `thunderstorm`, …), with the recipe shown in the row description.
- **Custom Effects** — your saved layered presets (authored in
  `Default Style > Effects > Custom`). Saved after boot? Reopen the menu to
  list new ones.

### Glitch

`<expression> > Glitch`: **Default (inherit)**, **Off** (force it clean even
when the default glitches), or a glitch **preset**.

---

## Custom expressions

Open an empty slot (or **Add Expression…**) and fill in:

1. **Name…** — via the on-screen keyboard (now with a symbols page and a real
   text cursor — see below).
2. **Base Face** — which built-in slot's PNG supplies the art. The style is
   what makes it look different from that base.
3. **Material / Effect / Glitch** — its style, exactly as above.
4. **Triggers** — what activates it (below).

**Activate** / **Deactivate** test it from the menu. **Clear Slot** empties it
(and drops its triggers).

---

## Trigger recipes

Every expression's **Triggers** menu decides what makes it activate. A trigger
is a **recipe**:

> **event × count within a window, WHILE conditions hold**

Two worked examples:

- **Nose boop ×5 → Angry**: on Angry's Triggers, set Trigger 1 → Event
  *Boop: Snout*, Count *5*, Window *3 s*.
- **Left cheek while head tilted left → Curious**: on a custom "Curious", set
  Event *Boop: Left Cheek*, Count *1*, and *While: Head Tilt → Tilted Left*.

Each expression has a **Hold Time** and up to **three** recipe slots.

### Hold Time

How long a triggered activation shows before the previous face returns.
**0 = latch** — it stays until another expression is chosen (or Deactivate).

### Recipe fields

| Field | Options |
|---|---|
| **Event** | *Off* · Boop: Snout / Left Cheek / Right Cheek / Both Cheeks · Swipe Up / Down / Left / Right (APDS-9960) · Head Shake · Gets Bright · Gets Dark |
| **Count** | 1–10 — fire after this many events inside the window |
| **Window** | 0.5–10 s — rolling window the count must land inside |
| **While: Head Tilt** | Any / Tilted Left / Tilted Right (head roll past **Tilt Angle**, 5–45°) |
| **While: Light** | Any / Bright / Dark (ambient lux above/below **Light Threshold**) |
| **While: Motion** | Any / Moving / Still (wearer walking/active vs holding still) |
| **Clear Recipe** | Reset this slot to Off |

The recipe row summarises itself in the list, e.g.
`Trigger 1: Boop Snout x5 +tiltL`.

### How firing works

- **Conditions are checked when the final counting event lands.** "Left cheek
  while tilted left" only fires if your head is tilted at the moment of the
  cheek boop.
- **The most specific matching recipe wins** — higher count first, then more
  conditions. So a bare "cheek → happy" and "cheek ×5 → angry" can coexist:
  single boops give happy, the fifth in the window gives angry.
- **Counting boops keep the normal boop reaction.** On the way to "×5 → angry",
  boops 1–4 still play the default boop face/ripple as feedback; only the boop
  that actually fires a recipe is claimed by it.
- **Light edges** (Gets Bright / Gets Dark) are edge-detected with ±10 %
  hysteresis around the threshold, so a hovering reading doesn't machine-gun.

### Events vs. conditions — the difference

- An **event** is a momentary thing that gets *counted* (a boop, a swipe, a
  shake, a light crossing).
- A **condition** (`While: …`) is a *state* that must be true *at fire time*.
  Motion "Still", for instance, is only ever a condition — there is no "became
  still" event. Head Shake is only ever an event.

---

## Where it's stored

All of this saves through the normal **Save Face Config** flow into
`config.json`:

- `protoface.expression_styles` — `{ "<expr>": { material, effect, glitch } }`,
  keyed by built-in stem (`"happy"`).
- `protoface.custom_expressions` — the custom-slot list (name, base_expression,
  style).
- `protoface.expression_triggers` — `{ "<key>": { hold_s, recipes[…] } }`,
  keyed by built-in stem or `"custom_<slot>"`.

Absent keys mean pure legacy behaviour: no per-expression styles and no
recipes, exactly as before this feature.

---

## The on-screen keyboard (used for naming)

Text entry (custom-expression names, etc.) got three upgrades:

- **Symbols page** — a `?123` / `ABC` key on the bottom row flips between
  letters and symbols; a USB keyboard can type any printable character too.
- **Real cursor** — press **Up** from the top key row to focus the text field,
  then **Left/Right** move the insertion point; characters insert (and delete)
  at the cursor instead of always at the end. The knob still walks the keys.
- **Character counter** — a `used/max` readout above the field, which turns red
  when you're within five of the limit.

---

## Related

- **Default Style** — `Faces and Expressions > Default Style`: the Material /
  Effects / Glitch that every expression inherits. (The legacy
  Expression-Coupled Effects toggle still works when an expression has no
  effect style of its own.)
- **Boop / Light Sensor / Voice / Reactions** — also under Faces and
  Expressions; they feed the same events/conditions the recipes read.
- **Accessory LEDs** — see the four-group split above; blush/cheek zones react
  to boops.
- **Custom LED face panels** — `docs/led-face-panels.md` (APA102 panels + the
  `scripts/pnp_to_ledmap.py` pick-and-place → LED-map converter).
