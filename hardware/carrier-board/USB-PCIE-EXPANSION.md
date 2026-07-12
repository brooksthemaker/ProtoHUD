# Carrier Board — USB 3.1 hub, USB 2.0 hub & PCIe switch parts list

Parts for expanding the carrier's high-speed I/O: a 4-port USB 3.1 hub, the
4-port USB 2.0 hub (Req **N1** in [`BOM.md`](BOM.md)), and a PCIe packet
switch to fan out the CM5's single lane.

## What the CM5 gives you (sets the speed grades)

| CM5 interface | Spec | Consequence |
|---|---|---|
| 2× USB 3.0 host (via RP1) | **5 Gbps (Gen 1)** | A USB 3.1 **Gen 1** hub is the right fit — a 10 Gbps Gen 2 hub can't run faster than the host and wastes money |
| 2× USB 2.0 host (via RP1) | 480 Mbps | Feeds the N1 hub directly, no USB 3 port consumed |
| 1× PCIe **Gen 2 x1** | ~500 MB/s raw (Gen 3 unofficial/OC) | A **Gen 2 packet switch** matches; all downstream devices share the one uplink lane |

Verify the exact port allocation against the CM5 datasheet / DF40 pinout when
assigning (the block diagram currently feeds the N1 hub from a CM5 USB 2.0
host port — keep that).

## A. USB 3.1 Gen 1 ×4 hub

| # | Part | Example P/N | Qty | Notes |
|---|------|-------------|-----|-------|
| A1 | **USB 3.1 Gen 1 4-port hub controller** | **Microchip USB5744-I/2G** (SQFN-56) | 1 | SS + HS on all 4 downstream. Default straps work with no config ROM; SMBus optional. ~$7 |
| A2 | Crystal, 25 MHz ±50 ppm | ABM8-25.000MHZ + load caps | 1 | |
| A3 | 1.2 V core regulator | e.g. TLV75712 (LDO, ≥300 mA) or small buck | 1 | USB5744 needs VDD33 + VDD12 |
| A4 | Per-port VBUS switch, current-limited | TPS2553DRV ×4 (or dual TPS2561A ×2) | 4 ch | Set ≥900 mA/port (USB 3 spec); give any high-draw port (e.g. VITURE if fed from here) a ≥1.5 A part |
| A5 | AC-coupling caps, SS TX pairs | 100 nF 0402 | 10 | Upstream TX (2) + 4× downstream TX (2 ea) |
| A6 | ESD, SuperSpeed pairs | TPD2EUSB30 ×5 (or TPD6E05U06 ×2) | — | One per SS pair set |
| A7 | ESD, HS pairs + VBUS | USBLC6-2SC6 | 5 | Upstream + 4 downstream |
| A8 | Decoupling | 0.1 µF per supply pin + 10 µF bulk ×2 | ~12 | Per datasheet |

**Alternates:** TI **TUSB8041AI** (VQFN-64, 24 MHz XTAL, 3.3 V + 1.1 V — equally
good, slightly pricier); Infineon **CYUSB3304** (HX3). Avoid VIA VL817 /
Genesys GL3510 for an open design — consumer parts, datasheets are hard to get
in small quantity.

## B. USB 2.0 ×4 hub (Req N1 — confirms/extends the BOM row)

| # | Part | Example P/N | Qty | Notes |
|---|------|-------------|-----|-------|
| B1 | **USB 2.0 4-port hub controller** | **Microchip USB2514B-AEZC** (QFN-36) | 1 | 3.3 V only (internal core reg). ~$3 |
| B2 | Crystal, 24 MHz | ABM8-24.000MHZ + load caps | 1 | |
| B3 | VBUS switch (ganged is fine) | TPS2066 (dual) or TPS2553 per port | 1–4 | Knob / LoRa / RP2354B-CDC are light or self-powered; RP2350 audio ~100 mA |
| B4 | ESD | USBLC6-2SC6 | 5 | Upstream + 4 downstream |
| B5 | Decoupling | 0.1 µF ×~6 + 4.7 µF bulk | — | |

**Alternates:** **USB2517** (7-port, same family) — one chip absorbs every USB 2
peripheral with ports to spare, worth it if the port count creeps; **TUSB4041I**;
FE1.1S only for throwaway prototypes (minimal docs/QC).

## C. PCIe Gen 2 packet switch (1 uplink → 3–4 slots)

| # | Part | Example P/N | Qty | Notes |
|---|------|-------------|-----|-------|
| C1 | **PCIe Gen 2 packet switch** | **Diodes/Pericom PI7C9X2G404SL** | 1 | 4 ports / 4 lanes: 1 upstream x1 + 3 downstream x1. Public datasheet, Mouser/Digi-Key stock, ~$15. Integrated downstream clock buffer |
| C2 | Core regulator | small LDO/buck per datasheet rails (3.3 V I/O + LV core) | 1 | ~1 W total switch power |
| C3 | AC-coupling caps, TX pairs | 100 nF 0402 | 2 per TX pair | Every TX pair leaving a chip you place; CM5's own TX caps are on-module (verify in CM5 datasheet) |
| C4 | Optional HCSL refclk buffer | PI6C557-05LE | 0–1 | Only if downstream clock outputs run short of slot count |
| C5 | M.2 Key-M socket (2280) | TE 2199230-4 or similar | 1+ | NVMe for recording/gallery; add sockets per downstream port used |
| C6 | 3.3 V ≥3 A rail for NVMe | buck from +5 V (e.g. TPS62869 class) | 1 | NVMe peaks 8–10 W; do **not** hang it off logic 3.3 V |
| C7 | PERST# / CLKREQ# routing | — | — | CM5 PERST# → switch; switch (or GPIO) → per-slot PERST#; per-slot CLKREQ# pulled per datasheet |

**Alternates:**
- **ASMedia ASM1184e** (1→4 Gen 2) — the de-facto Pi 5 expander. Bare chips via
  LCSC/AliExpress (~$8), **no public datasheet** — crib from the many open Pi
  carrier layouts, or bring up with a finished M.2 → 4×M.2 board (Pineboards
  Quad, Waveshare) before committing to copper. ASM1182e = 1→2 version.
- **Broadcom/PLX PEX8606** (6-lane) if more/wider ports are ever needed —
  pricier, BGA, more power.

## Suggested topology

```
CM5 USB3 #1 (5 Gbps) ──► USB5744 ──► USB cams / future SS devices / spare
CM5 USB2  #1 ─────────► USB2514B ──► RP2354B CDC · RP2350 audio · knob · LoRa
CM5 USB3 #2 (or a 5744 port w/ 1.5 A switch) ──► VITURE glasses (USB data)
CM5 PCIe Gen2 x1 ─────► PI7C9X2G404 ──► NVMe (M.2) · 2 spare (Coral TPU / 2.5GbE / …)
```

- The 2.0 hub stays on a CM5 USB 2.0 host port (as in the block diagram), so
  both USB 3 ports remain free for bandwidth users.
- **Bandwidth reality:** everything behind the switch shares ~500 MB/s
  (Gen 2 x1); everything behind the 3.1 hub shares 5 Gbps. Fine for NVMe
  recording + a TPU; don't expect four full-rate NVMe drives.

## Layout notes (the part that actually makes it work)

- USB HS pairs 90 Ω diff; USB SS and PCIe pairs 85–90 Ω diff per datasheet;
  intra-pair skew < 5 mil (SS/PCIe), keep stubs off the pairs.
- SS/PCIe want a solid ground reference plane the whole run; via-count minimal
  and symmetric per pair.
- Crystals: short traces, ground guard, load caps per crystal spec.
- All three ICs are QFN/SQFN with thermal pads — plan the stencil and via farm.
