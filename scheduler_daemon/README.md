# ProtoHUD Scheduler Daemon

Companion service that feeds the on-glasses scheduler/reminder system. It owns all
networking so the real-time C++ HUD never does HTTP or OAuth.

```
phone browser ──► web form (this daemon) ─┐
                                          ├─► events.json ──► ProtoHUD (file poll) ──► toast reminders
Google Calendar ──► (later) sync ─────────┘    scheduler_status.json
```

## What it does today

- Serves a phone-friendly **web form** on `http://<device-ip>:<web_port>` (default
  `8770`) to add/delete calendar events (title, date, start/end or all-day, location).
- Merges all sources and **atomically** writes:
  - `events.json` — the merged event list the HUD reads.
  - `scheduler_status.json` — web URL, Google auth state, event count (the HUD shows
    these in the Scheduler menu).
- A heartbeat rewrites those files periodically so the HUD can tell the daemon is alive.

Google Calendar sync is stubbed (`load_google_events()` / `google_state()` return
empty/"disconnected") and lands in a later pass; `requests` will be added then.

## Run it

```bash
cd scheduler_daemon
python3 run.py
```

No dependencies (Python 3 standard library only). Config is read from the shared
ProtoHUD `config.json` `"scheduler"` block if found (searched: `$SCHEDULER_CONFIG`,
`./config.json`, `../config/config.json`, `/home/user/ProtoHUD/config/config.json`,
`/etc/protohud/config.json`); otherwise built-in defaults are used.

ProtoHUD can auto-launch it on boot when `scheduler.autostart` is `true` in config
(mirrors the Protoface daemon). Otherwise start it manually or via systemd.

## Files & data

- Output (HUD-facing): `events.json`, `scheduler_status.json` — paths from config,
  default `~/.local/share/protohud-scheduler/`.
- Private store: `manual_events.json` in the same data dir.

## Security

No authentication — intended for a personal device on a trusted LAN, same model as the
rest of ProtoHUD. User-entered text is HTML-escaped before display. Do not expose the
port to the public internet.

## Google sync setup (for the later pass)

1. In Google Cloud Console create an OAuth client of type **TV and Limited Input
   devices**.
2. Copy `client_secret.example.json` → `client_secret.json` and fill in your client id/
   secret. **Never commit** `client_secret.json` or `token.json` (both gitignored).
3. The daemon will run the device flow: the glasses show a short URL + code, you
   authorize on your phone, and the refresh token is stored at
   `~/.config/protohud-scheduler/token.json`.
