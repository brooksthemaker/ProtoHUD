# ProtoHUD — Changelog

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
