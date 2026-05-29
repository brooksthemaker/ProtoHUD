# ProtoHUD Companion App API (v1)

The companion app (a Flutter app — see the **ProtoHUD-companion-** repo) controls
the helmet over a small REST + JSON HTTP server that runs *inside* the `protohud`
process (`src/net/remote_control_server.{h,cpp}`). The server has to live in-process
because `protohud` exclusively owns the serial ports that drive the face LEDs.

The same socket is reachable two ways, both selected by the companion app's
connection screen:

| Transport | How the phone reaches the Pi | Default host:port |
|-----------|------------------------------|-------------------|
| **Pi broadcast Wi-Fi (AP mode)** | Phone joins the `ProtoHUD` network the Pi broadcasts. Set up with `scripts/setup_companion_ap.sh`. | `192.168.50.1:8780` |
| **USB tether (USB-ethernet gadget)** | Phone (or laptop) is wired to the Pi's USB-C; the gadget exposes a point-to-point link. Set up with `scripts/setup_companion_usb.sh`. | `192.168.42.1:8780` |

Both routes terminate at the same `0.0.0.0:8780` bind, so the API is identical
regardless of how you connect. A normal LAN/Wi-Fi connection works too if the Pi
is on a shared network.

## Enabling the server

In `config/config.json`:

```jsonc
"network_control": {
  "enabled": true,
  "host":    "0.0.0.0",   // bind all interfaces (AP + USB + LAN)
  "port":    8780,
  "token":   ""           // "" = no auth; set a string to require X-ProtoHUD-Token
}
```

## Auth

If `token` is non-empty, **every** request must carry a matching
`X-ProtoHUD-Token: <token>` header (a `401` is returned otherwise). For the
isolated AP / USB-tether links the app uses, leaving `token` empty is fine. CORS
is wide-open (`Access-Control-Allow-Origin: *`) so a browser PWA on the same link
works too.

## Endpoints

All paths are prefixed `/api/v1`. Request and response bodies are JSON. Every
response includes `"ok": <bool>`; failures add `"error": "<code>"`.

### `GET /status`

Health + current face look. Poll this (~1 Hz) to keep the app's UI in sync.

```json
{
  "ok": true,
  "name": "ProtoHUD",
  "api": "1",
  "face": {
    "connected": true,
    "hud_control": true,
    "r": 0, "g": 220, "b": 180,
    "brightness": 200,
    "effect_id": 0,
    "gif_id": 0,
    "palette_id": 0,
    "playing_gif": false,
    "face_index": 0
  }
}
```

### Face control

| Method & path | Body | Effect (`IFaceController`) |
|---------------|------|----------------------------|
| `POST /face/color`      | `{ "r":0-255, "g":0-255, "b":0-255, "layer"?:0-255 }` | `set_color` |
| `POST /face/effect`     | `{ "effect_id":0-255, "p1"?:0-255, "p2"?:0-255 }`      | `set_effect` |
| `POST /face/gif`        | `{ "gif_id":0-255 }`                                   | `play_gif` |
| `POST /face/brightness` | `{ "value":0-255 }`                                    | `set_brightness` |
| `POST /face/palette`    | `{ "palette_id":0-255 }`                               | `set_palette` |
| `POST /face/expression` | `{ "index":0-255 }` **or** `{ "name":"happy" }`        | `set_face` / `set_face_by_name` |
| `POST /face/release`    | _(none)_                                               | `release_control` (return face to autonomous mode) |

All numeric fields are clamped to `0-255`; missing fields default sensibly
(color components → 0, brightness → 200). Returns `503 {"error":"no_face"}` if no
face backend is attached.

### `POST /pad`

The remote control pad. Fires the **same** action as the in-paw wireless
controller / SmartKnob, so behaviour is identical across transports.

```json
{ "button": "nav_up" }
```

`button` ∈
`select` · `back` · `menu` · `nav_up` · `nav_down` · `nav_left` · `nav_right` ·
`af` (autofocus both OWLsight cameras) · `capture` (stereo photo).

Unknown buttons return `400 {"error":"unknown_button"}`; a missing `button` field
returns `400 {"error":"missing_button"}`.

> **Note:** PiP-toggle buttons are intentionally not in v1 — in the helmet they are
> momentary GPIO states sampled by the render loop each frame, not callback
> actions, so they need a different mechanism. Planned for a later revision.

## Versioning

`api` in `/status` is bumped only on incompatible changes. The app should check it
on connect and warn if it doesn't recognise the major version.
