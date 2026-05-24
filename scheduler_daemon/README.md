# ProtoHUD Scheduler Daemon

Companion service that feeds the on-glasses scheduler/reminder system. It owns all
networking so the real-time C++ HUD never does HTTP or OAuth.

```
phone browser ──► web form (this daemon) ─┐
                                          ├─► events.json ──► ProtoHUD (file poll) ──► toast reminders
Google Calendar ──► (later) sync ─────────┘    scheduler_status.json
```

## What it does

- Serves a phone-friendly **web form** on `http://<device-ip>:<web_port>` (default
  `8770`) to add/delete calendar events (title, date, start/end or all-day, location).
- Syncs **Google Calendar** (`gcal.py`) via the OAuth 2.0 device flow — see setup
  below. Active only when a `client_secret.json` is present; otherwise the state stays
  `disconnected` and only the web form is used.
- Merges all sources and **atomically** writes:
  - `events.json` — the merged event list the HUD reads.
  - `scheduler_status.json` — web URL, Google auth state (incl. the device-flow
    code/URL while pending), event count (the HUD shows these in the Scheduler menu).
- A heartbeat rewrites those files periodically so the HUD can tell the daemon is alive.

No third-party dependencies — both the web form and Google sync use the Python
standard library (`urllib`). HTTPS to Google needs system CA certificates.

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

## Google sync setup

1. In Google Cloud Console create an OAuth client of type **TV and Limited Input
   devices** and enable the Google Calendar API. Add yourself as a test user (or
   publish the consent screen).
2. Copy `client_secret.example.json` → `client_secret.json` and fill in your client id/
   secret. **Never commit** `client_secret.json` or `token.json` (both gitignored).
3. Start the daemon. It runs the device flow automatically: the Scheduler menu on the
   glasses (and the daemon log) shows a short URL + code — open the URL on your phone
   and enter the code. The refresh token is then stored at
   `~/.config/protohud-scheduler/token.json` and events sync every 5 minutes.

Scope is `calendar.readonly` (events are pulled, never modified). Events are fetched
30 days ahead from the user's primary calendar; recurring events are expanded.
