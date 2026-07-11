# VITURE Beast — SBS / 3D bring-up over an HDMI→USB-C(DP) converter

Notes for getting the VITURE Beast into per-eye **side-by-side (SBS) 3D** mode
when the CM5 drives it through an **HDMI → USB-C DisplayPort-Alt-Mode**
converter (rather than a native USB-C DP-alt port).

## TL;DR

The Beast's SBS mode is a **non-standard wide timing that lives only in the
glasses' EDID**. If the converter masks/replaces the sink EDID, the CM5 never
sees that mode, can't output it, and ProtoHUD's SBS switch is skipped — so the
glasses stay 2D no matter what you toggle on-device.

Two things must both be true for SBS:
1. **Video** — the CM5 must actually output the Beast's wide SBS timing
   (3840×1080 or 3840×1200 @60 — *confirm which for your firmware*, see below).
   This requires the Beast's EDID to reach the CM5.
2. **Control** — the USB data channel to the glasses must be present so the
   VITURE SDK can command SBS (and read the IMU). Over a pure HDMI→DP video
   bridge this is absent; keep the glasses' USB connected to the CM5.

If USB is preserved, ProtoHUD's SDK connects, renders per-eye halves, and
switches SBS **automatically** once the output is ≥3840 wide — you shouldn't
need the on-device menu at all.

## How ProtoHUD's XR path works (for reference)

`src/vitrue/xr_display.{h,cpp}`:

- `find_and_connect()` opens the VITURE SDK over **USB**. On success `found =
  true`; only then does each eye get half the width
  (`eye_w_ = disp_w_ / 2`, `xr_display.cpp:95`). No USB → 2D full-width render.
- `choose_monitor()` picks a monitor whose mode width is **≥ 3840**
  (`xr_display.cpp:485`). No 3840-wide mode → windowed/2D fallback.
- `set_sbs_display_mode()` (`xr_display.cpp:373`):
  - **Guards** on `host_w < 3840` and *skips the SBS switch* if the output
    isn't ≥3840 wide, logging *"Force the HDMI output to 3840x1200 to enable
    per-eye SBS."*
  - Enters native mode, then tries the `sbs_height`-derived mode, then falls
    back to `3840×1200@60`; the bypass path accepts only `3840×1200@90`.
  - Logs `set_rc` + `readback` for **every** attempt — the key diagnostic.
- Config: `display.sbs_height` (default **1200**), `display.target_fps`
  (default 90; Beast reliably wants **@60** for SBS), `viture.monitor_index`
  (-1 = auto, prefer a 3840-wide monitor).

## Diagnose first

Run on the **CM5** with the glasses connected.

```bash
# 1. Is the USB control channel present? (VITURE enumerates as 35ca:1102)
lsusb | grep -i 35ca

# 2. What modes / EDID does the CM5 actually see through the converter?
cat /sys/class/drm/card*-HDMI*/modes | grep 3840        # is 3840x1200 / 3840x1080 listed?
for f in /sys/class/drm/card*-HDMI*/edid; do echo "== $f =="; edid-decode "$f"; done
#   -> Does it name VITURE and list a 3840-wide detailed timing,
#      or is it the converter's generic EDID?

# 3. What does ProtoHUD's XR layer report? (the decisive log)
./protohud 2>&1 | grep -i '\[xr\]'
#   Look for: "[xr] display WxH", the "host output ... (<3840) — skipping" guard,
#   and per-mode "set_rc=.. readback=0x.." lines showing which modes the
#   firmware accepts vs rejects (rc=-7 = rejected).
```

If `3840x…` is missing from `modes`, the EDID isn't getting through — the
converter is the culprit.

## Fix: force the Beast's EDID on the CM5 (KMS-correct)

The CM5 uses full KMS (`vc4-kms-v3d`), so the **legacy `hdmi_edid_file` /
`hdmi_force_edid` in `config.txt` are ignored**. Use the DRM override instead.

1. **Capture** the Beast's EDID: plug the glasses into a laptop's *native*
   USB-C DP-alt port and dump it:
   ```bash
   cp /sys/class/drm/card*-*/edid beast.bin
   ```
   (Or use a reference dump — see the community repo below — but your own unit's
   EDID is the most reliable for your firmware.)
2. **Install** it on the CM5:
   ```bash
   sudo mkdir -p /lib/firmware/edid
   sudo cp beast.bin /lib/firmware/edid/beast.bin
   ```
3. **Point KMS at it** in `/boot/firmware/cmdline.txt` (connector name from
   `ls /sys/class/drm/`, e.g. `HDMI-A-1` or `HDMI-A-2`):
   ```
   drm.edid_firmware=HDMI-A-1:edid/beast.bin
   ```
   Reboot. The CM5 now offers the Beast's wide timing regardless of the
   converter's own EDID.

**Ordering caveat:** `set_sbs_display_mode()` runs at XR init. Force the mode at
*boot* (cmdline) so the 3840-wide output is live **before** ProtoHUD launches —
otherwise the guard skips SBS and it won't retry on its own (a re-apply hook is
a planned enhancement, below).

**Converter caveat:** even with the EDID forced, the converter must actually
*pass a custom detailed timing* upstream rather than snapping to CEA modes. A
board with true EDID passthrough is the reliable choice. Bandwidth is not the
issue — the wide mode is ~297 MHz / 7.13 Gbps RGB (4.75 Gbps as YCbCr 4:2:2),
well under a 4K@120 converter's capability.

## ⚠️ Resolution: 1080 vs 1200 (verify for your firmware)

ProtoHUD currently **hardcodes SBS = 3840×1200** (native fallback and the
bypass path). The community Linux repo (below) and its EDID dumps document the
Beast wide mode as **3840×1080@60** and mention 3840×1200 nowhere.

This means firmware may vary. If your Beast's SBS is a **1080**-high frame,
ProtoHUD's 1200-only candidates are all rejected → the *"no SBS/3D display mode
was accepted"* warning → 2D, even with the EDID fixed.

**Check your unit:** the `edid-decode` detailed timing (1080 vs 1200) + the
`[xr] … set_rc/readback` log tell you which height your firmware accepts. If
it's 1080:
- Set `display.sbs_height = 1080` and `display.target_fps = 60`
  (`fps_to_display_mode` then returns the `…_3D_SBS_3840_1080_60HZ` constant for
  the native path).
- The hardcoded `3840×1200` native fallback and the `3840×1200@90` bypass mode
  still need code changes to also try 1080 — **planned enhancement below**.

## Community reference

`https://github.com/blchinezu/viture-beast-linux`
- Useful: ships **Beast EDID dumps** (`edid_viture_beast_overclock.bin` + stock),
  confirms the `drm.edid_firmware=<connector>:edid/…bin` method, documents the
  USB ID `35ca:1102`, and the wide mode as **3840×1080@60** (~297 MHz).
- Not applicable to us: its actual fix is an **AMD amdgpu** debugfs
  DP-link-training hack (locked RBR + YCbCr 4:2:2). The CM5 is vc4/v3d, and the
  DP link to the glasses is trained by the converter, not the Pi — nothing to
  force on our side.

## Planned ProtoHUD enhancements (not yet implemented)

- Make the **SBS resolution + refresh fully config-driven**, trying both 1080
  and 1200 candidates in *both* the native and bypass paths (instead of the
  hardcoded 3840×1200), so ProtoHUD adapts to whatever the firmware accepts.
- Add a **"Re-apply SBS mode"** menu action (and/or run it on *Restart
  Protoface*) to re-run `set_sbs_display_mode()` after the 3840-wide output is
  up — no reboot needed while iterating on the display path.
