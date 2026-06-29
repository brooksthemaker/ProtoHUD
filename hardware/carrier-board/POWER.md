# Carrier Board — power tree & budget

How power flows and how big each rail must be. Per-device figures marked
**(code)** come from ProtoHUD's own estimator, `rail_currents_mA()` in
`src/main.cpp`; panel/LED draws are datasheet-typical. Everything shares one
ground.

## Domains

Four 5 V-class domains off one input, plus a **local 3.3 V** rail. (The CM5's
own internal 3V3 is no longer the sensor supply — sensors moved to the RP2354B
brain, fed by `+3V3_RP`.)

| Rail | Feeds | Why separate |
|------|-------|--------------|
| **5 V · CM5** (`+5V`) | CM5 + USB hub/peripherals + buffers' B-side | clean, steady; must not brown out |
| **5 V · HUB75 panels** (`+5V_PANEL`) | J2 panel VCC | huge, spiky current — own copper + bulk caps |
| **5 V · WS2812** (`+5V_LED`) | J4 LED VCC | fused; LED inrush/noise off the CM5 rail |
| **5–6 V · servos** (`+V_SERVO`) | J20–J27 servo V+ | stall/inrush surges — own fused rail + bulk caps |
| **3.3 V · local** (`+3V3_RP`) | RP2354B + I²C sensors + buffers' A-side | LDO/buck off 5 V; isolates MCU/sensors from CM5 |

```mermaid
flowchart TB
    subgraph EXT["External unit (belt/back)"]
        IN["Ryobi 40 V battery<br/>~30–42 V"]:::pwr
        PROT["fuse · RP-FET · TVS<br/>(≥ 50 V) · LVC"]:::pwr
        BUCK["40 V → 5 V buck<br/>sized for 5 V peak"]:::pwr
        IN --> PROT --> BUCK
    end
    BUCK ==>|"5 V umbilical (≤24 A!)<br/>fat/short + remote sense"| J1["J1 · 5 V in (helmet)<br/>5 V TVS + big bulk caps"]:::pwr
    J1 --> BUS["5 V distribution (common GND)<br/>star + bulk caps"]:::pwr

    BUS -->|"≥ 5 A"| CM5["CM5 (HUB75 + cams + USB)"]:::cm5
    BUS -->|"LDO/buck"| R33["+3V3_RP (local)<br/>RP2354B + sensors"]:::pwr
    R33 --> RP["RP2354B I/O MCU"]:::rp
    RP --> SENS["I²C sensors: BNO055 12 · MPU9250 4 ·<br/>MPR121 30 · BH1750 1 mA (code)"]:::dir

    BUS -->|"fused, high-current"| HUB["HUB75 panel rail → J2<br/>bulk caps at connector"]:::load
    BUS -->|"fused"| LED["WS2812 rail → J4"]:::load
    BUS -->|"fused (5–6 V)"| SRV["servo rail → J20–J27<br/>bulk caps"]:::load
    CM5 -->|"USB VBUS"| USB["USB hub → RP2354B (CDC) /<br/>RP2350 audio / knob / LoRa / VITURE / cams"]:::usb

    BUS -. tap .-> INA["INA219 ×N (optional)<br/>per-rail telemetry → HUD"]:::opt

    classDef pwr  fill:#fde2c4,stroke:#c9772a,color:#000;
    classDef cm5  fill:#cfe3ff,stroke:#2a5bc9,color:#000,font-weight:bold;
    classDef rp   fill:#cfeaff,stroke:#1f8fc9,color:#000,font-weight:bold;
    classDef load fill:#e8e8e8,stroke:#666,color:#000;
    classDef dir  fill:#d6f5d6,stroke:#2a9d3a,color:#000;
    classDef usb  fill:#ece0ff,stroke:#7a3ac9,color:#000;
    classDef opt  fill:#fff3b0,stroke:#b59000,color:#000;
```

## Budget — 3.3 V rail (`+3V3_RP`, local LDO/buck off 5 V)

| Load | Typical | Source |
|------|--------:|--------|
| RP2354B (cores + IO + USB) | ~30–50 mA | datasheet-typical |
| BNO055 IMU | 12 mA | (code) |
| MPU9250 IMU | 4 mA | (code) |
| MPR121 boop | 30 mA | (code) |
| BH1750 light | 1 mA | (code) |
| 74AHCT245/'125 A-side + pull-ups | ~5 mA | — |
| MCP23017 expander(s), if fitted | ~1 mA each | datasheet |
| **3.3 V subtotal** | **~85–105 mA** | well within a 500 mA LDO |

> Spec the `+3V3_RP` regulator for **≥ 500 mA** for headroom (LED-logic A-side,
> expanders, future sensors). LEDs/servos draw from **5 V / +V_SERVO**, not this
> rail — only chip logic sits on 3V3.

## Budget — 5 V rails

### CM5 + USB
| Load | Typical | Peak | Notes |
|------|--------:|-----:|-------|
| CM5 (cameras, HDMI, render) | 3–4 A | ~5 A | RPi spec: 5 V/5 A PSU |
| USB 3.1 hub (VL817) silicon | ~0.2 A | ~0.3 A | from `+5V`; internal 3.3/1.2 V regs (not on `+3V3_RP`) |
| USB downstream (4× USB-C) | 0.5–1 A | up to 4 A | **VBUS from board `+5V`, per-port PTC ~1 A** — not from CM5's USB current switch |
| **Subtotal** | **~4–5 A** | **~6.5 A+** | size `+5V` for the port budget you actually populate |

> **USB-C port power.** The CM5's own USB 3.0 ports are limited to ~1.2 A
> *combined* — too little for 4 downstream ports. So the hub is **self-powered**:
> each USB-C VBUS is fed from the board `+5V` rail through its own resettable fuse
> (PTC) or a load switch (e.g. TPS2553, which adds true current limit + fault
> flag). Budget `+5V` for the number of ports you expect to draw power
> simultaneously (4 × ~0.9 A ≈ 3.6 A worst case for bus-powered peripherals).

### HUB75 panels (the big one)
Per 64×32 P2.5 panel @ 5 V:

| State | Per panel | 4-panel face (128×64) |
|-------|----------:|----------------------:|
| Typical animated face (~20–40% lit) | ~0.8–1.5 A | ~4–6 A |
| **Full white (worst case)** | ~3–4 A | **~12–16 A** |

> This rail dominates the design. Size copper/connectors/supply for the **full-
> white peak**, or cap brightness in software and budget the typical. A global
> brightness limit makes the difference between a 6 A and a 16 A supply.

### WS2812 accessory LEDs
`rail5 += LEDs × 20 mA` **(code)**, moderate brightness:

| Count | Typical (20 mA) | Full white (~60 mA) |
|------:|----------------:|--------------------:|
| 30 | 0.6 A | 1.8 A |
| 60 | 1.2 A | 3.6 A |

### MAX7219 (RP2354B side, alongside HUB75)
`rail5 += modules × 80 mA` **(code)** at full brightness. An 8-module accent
matrix ≈ 0.64 A — trivial next to HUB75. Driven by the RP2354B, so it can run
*with* HUB75, not instead of it.

### Servos (`+V_SERVO`, 5–6 V)
Hobby-servo current is load-dependent and **spiky**:

| State (per micro/standard servo) | Current |
|---|--:|
| Idle / holding light | ~0.1–0.3 A |
| Moving under load | ~0.5–1.0 A |
| **Stall (worst case)** | ~1.5–2.5 A |

| 8 servos | Typical (moving, ~0.5 A) | Worst (simultaneous stall) |
|---|--:|--:|
| `+V_SERVO` | ~2–4 A | ~12–20 A |

> Servos rarely all stall at once, but size the rail + fuse + bulk caps for a
> healthy fraction of worst-case (e.g. **≥ 5–8 A** for 8 micro servos) and add
> ≥ 1000 µF bulk so a move-transient doesn't sag logic. Big/standard servos:
> budget per the actual datasheet stall current.

## Worked total (4-panel HUB75 + sensors + 30 WS2812 + 8 servos + USB)

| Rail | Typical | Peak |
|------|--------:|-----:|
| CM5 + USB | ~4.5 A | ~6.5 A |
| HUB75 (`+5V_PANEL`) | ~5 A | ~16 A (full white) |
| WS2812 (`+5V_LED`) | ~0.6 A | ~1.8 A |
| Servos (`+V_SERVO`) | ~2–4 A | ~5–8 A (sized) |
| 3.3 V (`+3V3_RP`, ≈0.1 A → ~0.07 A@5V in) | — | — |
| **Total @ 5 V** | **~12–14 A (≈60–70 W)** | **~28–32 A (≈140–160 W)** |

**Recommendation:** size the 5 V supply for **≥ 12–15 A (60–75 W)** with a
software brightness cap, or **≥ 25–30 A** for uncapped full-white panels +
active servos. Give HUB75 and the servo rail their own high-current feeds; don't
push panel/servo surges through the CM5 rail.

## Power delivery — external 40 V→5 V unit, 5 V to the helmet (selected)

Architecture: a **Ryobi 40 V pack + buck regulator live in an external unit**
(belt/back), and a **5 V umbilical feeds the helmet**. The carrier's J1 input is
therefore **5 V**, and all 40 V parts (dock, protection, buck) live off-helmet.

> ⚠️ **The catch — the umbilical now carries the full helmet current at 5 V.**
> Up to ~16–24 A (panels) flows through the cable, and 5 V has almost no margin
> (CM5 wants 4.75–5.25 V; panels dim/colour-shift below ~4.5 V). This *undoes*
> the low-current benefit of the 40 V battery, which only applied on the wire
> between the pack and the buck.

### Umbilical voltage drop (round-trip, copper)
Drop = I × R, with R = 2 × length × wire resistance. For a ~3 ft (0.9 m) run:

| 5 V load | 10 AWG (~6 mΩ) | 12 AWG (~9.5 mΩ) | 14 AWG (~15 mΩ) |
|---------:|---------------:|-----------------:|----------------:|
| 10 A (capped) | 0.06 V (1.2%) ✅ | 0.10 V (1.9%) ✅ | 0.15 V (3%) ⚠️ |
| 20 A | 0.12 V (2.4%) ⚠️ | 0.19 V (3.8%) ⚠️ | 0.30 V (6%) ❌ |
| 24 A (full white) | 0.14 V (2.9%) ⚠️ | 0.23 V (4.6%) ❌ | 0.36 V (7%) ❌ |

Plus I²R heat in the cable (e.g. 20 A through 12 AWG ≈ 3.8 W). Longer runs scale
linearly worse.

### Making the 5 V feed work
- **Fat + short umbilical** — 10–12 AWG silicone, as short as the build allows.
- **Brightness cap** in software keeps the panels near the ~10 A column (the
  green/✅ region) instead of full-white.
- **Remote sense** — if the external buck supports 4-wire sense, run sense leads
  to the carrier so it regulates 5 V *at the helmet*, cancelling cable drop.
- **Large bulk capacitance at J1** to ride out spikes the cable can't deliver.
- Consider **separate feeds** for the panel rail vs CM5 so a panel surge doesn't
  drag the CM5 input down.

### Strongly consider instead: distribute 40 V, step down *in* the helmet
Putting the buck **on/near the carrier** and running **40 V down the umbilical**
drops the cable current to **~3 A** — so a thin, light cable with negligible
drop, and no high-current 5 V umbilical at all. Same battery, same buck, just
relocated. This is the lower-loss, lighter-cable option; the only reasons to keep
the buck external are to keep 40 V off the helmet (safety/regs) or to move the
converter's bulk/heat off your head. **If you can, step down at the helmet.**

### Regulator (in the external unit either way)

The **40 V → 5 V buck** specs are unchanged — high pack voltage means low input
current; the heavy amps live on the 5 V output (which is now the umbilical).

### Regulator selection (the critical part)
- **Wide input range:** must accept ~30–42 V; spec the converter for **≥ 50 V
  input** for margin (transients, full charge).
- **Output power = your 5 V peak:** a 42 V → 5 V buck must supply the full board
  draw. From the budget that's **~50 W typical (cap brightness)** or up to
  **~120 W full-white** (24 A @ 5 V). Size the converter accordingly:
  - Brightness-capped build → a **5 V / 10–15 A (50–75 W)** buck.
  - Uncapped full-white panels → a **5 V / 25–30 A (120–150 W)** buck.
- **Input current is small:** 120 W ÷ 40 V ÷ ~0.9 eff ≈ **~3.3 A** on the
  battery side → input wiring can be **16–18 AWG**; the **5 V output** is where
  the 20 A lives, so put the regulator **close to the panels** and keep the 5 V
  run fat and short.
- **Thermal:** at ~90 % efficiency a 120 W converter dumps **~12 W** as heat —
  heatsink it and give it airflow.

### Battery interface & safety
- **Dock/adapter:** mount via a Ryobi 40 V tool-side dock (3D-printable mounts +
  contacts, or an aftermarket adapter). Connect **B+ / B−**; the large center
  contacts carry current.
- **Low-voltage cutoff:** the pack's internal BMS protects the cells, but
  behavior varies — add a **low-voltage cutoff / battery monitor** (~3.0 V/cell,
  ~30 V pack) so the HUD shuts down gracefully rather than relying on the pack
  folding under load. Surface pack voltage via INA219 / an ADC divider.
- **Fuse the battery feed** right at the dock for the pack's fault current.

### Runtime (≈ pack Wh ÷ load)
| Pack | Energy | Typical (~50 W) | Full-white (~120 W) |
|------|-------:|----------------:|--------------------:|
| 4.0 Ah | ~144 Wh | ~2.5–3 h | ~1.1 h |
| 6.0 Ah | ~216 Wh | ~4 h | ~1.7 h |

> Energy ≈ Ah × ~36 V nominal; usable is a bit less after the buck (~90 %) and
> the low-voltage cutoff.

### Why this beats the alternatives here
| Source | vs. the 40 V plan |
|--------|-------------------|
| USB-C PD 5 V/3 A | far too little (15 W) for panels |
| 1S Li-ion + boost | impractical to boost to 5 V at >10 A |
| **Ryobi 40 V + buck** | ✅ low input current, high capacity, hot-swappable packs |

## Umbilical (backpack ↔ helmet)

System partition: the **backpack** holds the Ryobi battery, the 40 V→5 V
regulator, and the **phone dock**; a tether runs to the **helmet** carrying two
things:

| Conductor | Carries | Notes |
|-----------|---------|-------|
| **5 V power** | up to ~24 A @ 5 V (see drop table above) | fat 10–12 AWG + GND; add a **remote-sense pair** to J1 |
| **USB 2.0** | phone (in dock) ↔ CM5 host | scrcpy/ADB mirror + KDE Connect over USB; one CM5 port |

The phone connects as a **USB device** to the CM5's USB **host** port — ProtoHUD
runs `scrcpy` over ADB to mirror it into a V4L2 node (`src/android/android_mirror.cpp`).
So the helmet end of the USB lands on a CM5 USB port (dedicate one to the
umbilical — see `J11` in [`CONNECTORS.md`](CONNECTORS.md)); the in-helmet USB hub
(RP2350 audio / knob / LoRa / VITURE / cams) stays on a separate port.

### Build notes
- **Keep USB away from the power conductors.** Use the cable's shielded,
  twisted D+/D− pair; route it apart from the high-current 5 V leads to avoid
  the panel/LED switching noise coupling into USB. USB 2.0 is fine to ~5 m, so
  cable length isn't the worry — noise is.
- **Phone charging / VBUS — decide one:**
  - *Passive dock (simplest):* the single USB cable carries CM5 VBUS up to the
    phone; the CM5 charges it slowly (limited port current, and it adds to the
    5 V umbilical load).
  - *Charge-injection dock (faster):* the dock charges the phone from the
    backpack 5 V, data-only to the CM5 — **must isolate VBUS** so the dock
    doesn't back-feed the CM5 port.
- **One connector or two:** a single multi-pin circular (e.g. GX16/GX20 or a
  rugged push-pull) can carry both power and USB, or keep them as two separate
  keyed connectors. If combined, segregate the USB pair from the power pins.

## Protection & sequencing

Split across the two units:

- **External unit (40 V):** rate input protection for **≥ 50 V** — fuse,
  reverse-polarity P-FET (VDS ≥ 60 V), TVS (~45 V standoff), buck input cap
  ≥ 50 V. This is where the battery-voltage parts live.
- **Carrier / helmet (5 V at J1):** 5 V-class reverse-polarity FET + TVS
  (SMBJ5.0A) + **large bulk capacitance** at the input to absorb umbilical drop
  and spikes (REQ R2.1). If using remote sense, bring the sense pair to J1.
- **Per rail:** fuse the HUB75, WS2812, **and servo** rails (R2.3/R2.4/R2.5);
  consider **e-fuses** (TPS259x, REQ N2) for latch-off short protection —
  servos short easily when a horn jams. The `+3V3_RP` LDO/buck has its own
  current limit.
- **Bulk capacitance:** ≥ 1000 µF at the HUB75 connector, 470–1000 µF on CM5 and
  WS2812 rails — LED/panel rows switch hard.
- **Inrush:** big bulk + panels = inrush; consider soft-start / NTC on the panel
  rail so the supply doesn't fold on power-up.
- **Sequencing:** bring up CM5 5 V cleanly; panel/LED rails can come up with it
  (no strict ordering needed), but a brownout on the CM5 rail must not be caused
  by a panel surge — hence the separate feed.

## Telemetry (optional, ties into the HUD)

Per-rail **INA219/INA260** (REQ N2) lets ProtoHUD show *measured* draw next to
the existing `rail_currents_mA` *estimate*, and feeds the planned **battery
indicator** (CAPABILITIES → Possible Additions). I²C, so it rides bus 1.

## Conductor / connector sizing (quick guide)

| Current | Wire (chassis) | Notes |
|--------:|----------------|-------|
| ≤ 3 A | 22 AWG | sensors, single LED runs |
| ~5 A | 20 AWG | CM5 feed |
| ~10 A | 16 AWG | WS2812 + small panel count |
| ~16–20 A | 14–12 AWG | full HUB75 panel rail |

Use polarized/keyed power connectors (XT30/XT60 for the high-current input);
pour wide copper / multiple vias on the panel rail; keep the panel ground return
fat and short back to the star point.
