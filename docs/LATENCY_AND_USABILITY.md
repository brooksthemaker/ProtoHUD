# ProtoHUD — Latency Reduction & Usability Roadmap

A working list of candidate improvements, gathered from a walkthrough of the
camera-to-display pipeline. Two parts:

1. **Latency reduction** — where time is spent between the camera sensor and the
   glasses, and how to cut it (CSI prioritised, USB second).
2. **Usability / deployment** — making ProtoHUD easy to live with on **Raspberry
   Pi OS Lite** (headless: no desktop, no keyboard/monitor by default), with
   emphasis on networking, remote terminals, and a control/chat surface.

Each item lists **effort**, **risk**, and **expected payoff** so you can triage.
Nothing here is implemented yet — it's a menu.

---

## Part 1 — Latency reduction

### Pipeline recap

```
CSI:  sensor exposure → ISP (libcamera) → dmabuf READY → render thread draw()
      → NV12→RGB shader into eye FBO → [post-process] → composite blit → HUD
      overlays → glfwSwapBuffers (vsync wait) → glasses

USB:  V4L2 driver queue → cap.read() → CPU brightness → cv::flip → BGR→RGBA
      → [QR gray] → memcpy → glTexImage2D upload → fullscreen draw → composite
      → HUD overlays → swap → glasses
```

The CSI handoff is already *newest-frame-wins* (`dma_camera.cpp:380, 407`), so the
buffer logic itself is sound — the latency lives in capture rate, vsync, exposure,
and redundant GPU passes.

---

### CSI (OWLsight) — priority

| # | Improvement | File / location | Effort | Risk | Payoff |
|---|---|---|---|---|---|
| C1 | **Raise capture fps to ≥ display refresh.** Default `Config.fps = 60` while display targets 90. Sampling every 16.7 ms is wasted latency. Push to the max the ov64a40 supports at the chosen resolution. | `dma_camera.h:53`, `FrameDurationLimits` at `dma_camera.cpp:322-325` | Low | Low | High — cuts mean frame age |
| C2 | **vsync off (or measure it).** vsync ON by default adds 0–11 ms at 90 Hz and blocks the render thread on `glfwSwapBuffers`. Toggle already exists (`Y` key). | `xr_display.h:140`, `xr_display.cpp:142` | Low | Med (tearing) | Med-High |
| C3 | **Skip eye-FBO + composite when no effects.** The camera is drawn into an eye FBO, then `composite()` blits it again — a 2nd full-screen pass per eye. When post-process / timewarp / single-cam are all off, draw CSI straight into the default framebuffer eye viewport. | render path in `main.cpp`, `xr_display.cpp:192` | Med | Med (HUD ordering) | Med |
| C4 | **Drop redundant clears.** Eye FBO is cleared then fully overwritten by the fullscreen camera quad; `composite()` clears then overwrites both halves. Skip the clear whenever the camera covers the whole viewport (i.e. not theater/letterbox or single-cam). | `main.cpp:~11403`, `xr_display.cpp:195` | Low | Low | Low (free bandwidth) |
| C5 | **Shorter exposure / manual shutter in low light.** AE lengthens integration time → real motion-to-photon latency. The NV / manual-shutter path caps it. | `dma_camera.cpp` controls (`set_shutter_speed_us`, `set_ae_enable`) | Low | Low (noisier image) | Med in low light |

**Camera rotation note (asked separately):** rotating the CSI render **in the
shader** adds *effectively zero* latency — it's just extra UV math on the 4 quad
vertices in the draw that already happens (`nv12.vs`). 90°/270° may add a
sub-millisecond texture-cache blip and needs aspect handling, but not real frame
time. **Do not rotate on the CPU** — that forces a readback of the zero-copy
dmabuf and costs several ms. For 180°/flips, prefer a libcamera `Transform` at
configure time (free at the sensor).

---

### USB — secondary

| # | Improvement | File / location | Effort | Risk | Payoff |
|---|---|---|---|---|---|
| U1 | **Set `CAP_PROP_BUFFERSIZE = 1`.** Not set today, so OpenCV's V4L2 backend keeps a deep queue and `cap.read()` returns the *oldest* frame — up to ~3–4 frame intervals (~100 ms at 30 fps) of staleness. **Biggest single USB win.** | `open_v4l2`, `camera_manager.cpp:59-69` | Low | Low | High |
| U2 | **One capture thread per camera.** All three USB cams are read serially on one thread sharing one `frame`/`rgba` Mat, so a blocking `cap.read()` on usb1 stalls usb2/usb3. Decouple (thread per cam, or `grab()`-all then `retrieve()`). | `camera_manager.cpp:428-437` | Med | Med | Med-High (multi-cam) |
| U3 | **Move brightness / flip / BGR→RGBA into the shader.** Today these are full-frame CPU passes every frame. Brightness = fragment multiply, 180° flip = invert UVs, BGR↔RGBA = swizzle. Removes 2–3 CPU passes from the hot loop. | `camera_manager.cpp:374-389`, shader in `draw_tex_fullscreen` | Med | Med | Med |
| U4 | **`glTexSubImage2D` instead of `glTexImage2D` (+ PBO).** Current upload reallocates the texture every frame after a memcpy. Allocate once per resize; ideally async via a PBO. | upload path, `camera_manager.cpp:415-422` | Low-Med | Low | Med (render-thread stall) |
| U5 | **Gate / throttle / downscale QR grayscale.** `BGR→GRAY` runs whenever QR scan is on; downscale or run every N frames. | `camera_manager.cpp:382-389` | Low | Low | Low-Med |

---

### Suggested order of attack

- **Lowest-risk, behavior-preserving first:** C1 (CSI fps), U1 (`BUFFERSIZE=1`), U4 (`glTexSubImage2D`).
- **Then the structural wins:** C3 (eye-FBO bypass) and C2 (vsync) — bigger payoff, real trade-offs.
- **Multi-cam / CPU-offload:** U2, U3 if you run several USB cameras.

---

## Part 2 — Usability on Raspberry Pi OS Lite (headless)

Lite has no desktop, so everything should be reachable from a **phone or laptop
over the network**, and the device should configure itself on first boot without
a keyboard/monitor. Goals: get online, get a terminal, get a control/chat surface.

### A. Get it online without a screen

| # | Improvement | What it gives | Tooling |
|---|---|---|---|
| A1 | **Wi-Fi hotspot fallback + captive portal.** If no known network is found at boot, the Pi starts its own AP (e.g. `ProtoHUD-Setup`); connecting opens a page to pick a network and enter the password. | Zero-keyboard onboarding | `comitup`, or `RaspAP`, or `wifi-connect` (Balena) |
| A2 | **Headless Wi-Fi preseed.** Document the `wpa_supplicant.conf` / Raspberry Pi Imager "advanced options" flow so a network can be baked into the SD card before first boot. | Backup path for A1 | Pi Imager / `rpi-imager` |
| A3 | **mDNS / Bonjour (`protohud.local`).** Reach the device by name instead of hunting for an IP. | One-step discovery | `avahi-daemon` (usually preinstalled) |
| A4 | **On-HUD network status + IP/QR.** Show SSID, IP, and a QR code linking to the web UI directly in the HUD so the user can scan it with a phone. | Self-documenting | ProtoHUD overlay (QR scanner already present) |

### B. Remote terminal access

| # | Improvement | What it gives | Tooling |
|---|---|---|---|
| B1 | **SSH enabled by default (documented).** Ship instructions / a flag to enable SSH; remind users to change the default password or use keys. | Power-user access | `raspi-config nonint do_ssh`, empty `ssh` file on boot partition |
| B2 | **Browser terminal.** A web-based shell reachable from a phone (no SSH client needed). | Terminal anywhere | `ttyd` or `wetty` behind the control web UI |
| B3 | **In-HUD terminal/log pane.** A scrollable log/console overlay so users can see what ProtoHUD is doing without another device. | On-glasses diagnostics | New ImGui/NanoVG panel |

### C. Control & chat surface

| # | Improvement | What it gives | Tooling |
|---|---|---|---|
| C1 | **Companion web control panel.** A small server on the Pi (reachable at `protohud.local`) exposing the same settings as the in-HUD menu: camera source, NV mode, brightness, post-process, rotation, restart. | Phone-as-remote | lightweight C++/embedded HTTP, or a sidecar Python/Flask service |
| C2 | **Chat / messaging surface.** Two readings, both useful: (a) **remote text-to-HUD** — push a message from the web panel that appears as a HUD toast/banner (toasts already exist, `draw_toasts`); (b) **two-way chat** between wearer and a phone operator over the same web/websocket channel. | Communication while wearing glasses | reuse toast system + a websocket endpoint in C1's server |
| C3 | **Optional AI assistant hook.** If online, route a chat box to an API so the wearer can ask questions and get answers as HUD toasts. Make it strictly opt-in and offline-tolerant. | Hands-busy help | C1's server + provider SDK |

### D. "Just works" deployment plumbing

| # | Improvement | What it gives | Tooling |
|---|---|---|---|
| D1 | **systemd service, auto-start on boot, auto-restart on crash.** ProtoHUD comes up on power-on with no login. | Appliance behavior | `protohud.service` (`Restart=on-failure`) |
| D2 | **Read-only / overlay root filesystem.** Survives yanked power (glasses get unplugged) without SD-card corruption. | Reliability | `raspi-config` overlayfs, or `overlayroot` |
| D3 | **First-boot setup wizard.** Detects cameras, lets the user pick eye assignment, sets resolution/fps, tests the glasses — all from the web UI or HUD. | Friendly onboarding | extend C1 web panel |
| D4 | **One-line installer + prebuilt image.** A `curl | bash` installer and/or a flashable `.img` with everything baked in (deps, service, hotspot fallback). | Distribution | install script + `pi-gen` custom image |
| D5 | **OTA / git-pull updates.** "Update" button in the web panel that pulls and rebuilds, with rollback. | Maintainability | C1 panel + git + systemd |
| D6 | **Config in one human-editable file with safe defaults + validation.** Bad values fall back instead of crashing; web panel writes it. | Forgiving config | existing JSON config + schema/validation |

### Recommended usability MVP (highest impact, least work)

1. **D1** systemd auto-start — makes it an appliance.
2. **A1 + A3** hotspot fallback + `protohud.local` — gets users online with no screen.
3. **C1 (minimal)** web control panel — settings + restart from a phone.
4. **B1/B2** SSH and/or `ttyd` — a terminal when something goes wrong.
5. **A4** on-HUD IP/QR — ties it together so the wearer can find the panel.

Chat (C2) and the AI hook (C3) build naturally on top of the C1 web/websocket
server once it exists, and reuse the HUD toast system already in the codebase.

---

*Generated as a planning document — no source changes were made. Pick items and
ask to implement; the low-risk latency set (C1 / U1 / U4) is the suggested
starting point.*
