#!/usr/bin/env python3
"""Google Calendar sync for the ProtoHUD scheduler daemon.

Implements the OAuth 2.0 *device authorization flow* (for "TV and Limited Input
devices") so the user authorizes on their phone — the glasses only display a
short code + URL, never any typed input. Uses the Python standard library only
(urllib); no third-party dependencies.

Flow:
  1. POST device/code  -> device_code + user_code + verification_url (shown on HUD)
  2. Poll token        -> access_token + refresh_token once the user authorizes
  3. Refresh as needed -> pull upcoming events from Calendar v3, normalize to the
     scheduler event schema, and hand them to the daemon to merge/publish.

State is exposed via snapshot() for scheduler_status.json:
  state: "disconnected" (no client secret) | "pending" (awaiting authorization)
       | "connected" | "error"
"""

import datetime
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

DEVICE_CODE_URL = "https://oauth2.googleapis.com/device/code"
TOKEN_URL       = "https://oauth2.googleapis.com/token"
CAL_EVENTS_URL  = "https://www.googleapis.com/calendar/v3/calendars/primary/events"
SCOPE           = "https://www.googleapis.com/auth/calendar.readonly"
GRANT_DEVICE    = "urn:ietf:params:oauth:grant-type:device_code"

TOKEN_PATH      = os.path.expanduser("~/.config/protohud-scheduler/token.json")
SYNC_INTERVAL_S = 300          # pull cadence once connected
HORIZON_DAYS    = 30           # how far ahead to fetch events
HTTP_TIMEOUT_S  = 30


def _http(req):
    """Return (status, parsed_json). Never raises; error bodies are parsed too."""
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.load(e)
        except Exception:  # noqa: BLE001
            return e.code, {}
    except Exception as e:  # noqa: BLE001 — network/SSL/etc.
        return 0, {"error": str(e)}


def _post(url, data):
    body = urllib.parse.urlencode(data).encode()
    return _http(urllib.request.Request(url, data=body, method="POST"))


def _get(url, token):
    return _http(urllib.request.Request(
        url, headers={"Authorization": "Bearer " + token}))


def _rfc3339(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def _parse_dt(s):
    """RFC3339 datetime string -> epoch seconds (UTC)."""
    try:
        return int(datetime.datetime.fromisoformat(
            s.replace("Z", "+00:00")).timestamp())
    except (ValueError, AttributeError):
        return None


def _date_to_epoch(d, hour):
    """'YYYY-MM-DD' (local) at the given hour -> epoch seconds."""
    try:
        return int(datetime.datetime.strptime(d, "%Y-%m-%d")
                   .replace(hour=hour).timestamp())
    except (ValueError, TypeError):
        return None


class GoogleSync:
    def __init__(self, cfg, on_change):
        self.cfg = cfg
        self.on_change = on_change          # called when state/events change
        self.lock = threading.Lock()
        self.state = "disconnected"
        self.user_code = ""
        self.verify_url = ""
        self.events = []
        self._client = self._load_client_secret()
        self._refresh_token = self._load_refresh_token()
        self._access_token = None
        self._access_expiry = 0

    # ── public ────────────────────────────────────────────────────────────────
    def enabled(self):
        return self._client is not None

    def snapshot(self):
        with self.lock:
            return self.state, self.user_code, self.verify_url, list(self.events)

    def start(self):
        if not self.enabled():
            print("[scheduler] Google: no client_secret.json — sync disabled")
            return
        threading.Thread(target=self._run, daemon=True).start()

    # ── credential loading ──────────────────────────────────────────────────────
    def _load_client_secret(self):
        path = os.path.join(os.path.dirname(__file__), "client_secret.json")
        if not os.path.isfile(path):
            return None
        try:
            with open(path) as f:
                blob = json.load(f)
            node = blob.get("installed") or blob.get("web") or blob
            if node.get("client_id") and node.get("client_secret"):
                return {"client_id": node["client_id"],
                        "client_secret": node["client_secret"]}
        except Exception as e:  # noqa: BLE001
            print(f"[scheduler] Google: bad client_secret.json ({e})")
        return None

    def _load_refresh_token(self):
        try:
            with open(TOKEN_PATH) as f:
                return json.load(f).get("refresh_token")
        except Exception:  # noqa: BLE001
            return None

    def _save_refresh_token(self, tok):
        os.makedirs(os.path.dirname(TOKEN_PATH), exist_ok=True)
        with open(TOKEN_PATH, "w") as f:
            json.dump({"refresh_token": tok}, f)
        try:
            os.chmod(TOKEN_PATH, 0o600)
        except OSError:
            pass

    def _set_state(self, state, user_code="", verify_url=""):
        with self.lock:
            self.state = state
            self.user_code = user_code
            self.verify_url = verify_url
        if self.on_change:
            self.on_change()

    # ── main loop ───────────────────────────────────────────────────────────────
    def _run(self):
        while True:
            try:
                if not self._refresh_token and not self._device_flow():
                    time.sleep(30)
                    continue
                if not self._ensure_access():
                    self._set_state("error")
                    self._refresh_token = None   # force re-auth on next pass
                    time.sleep(60)
                    continue
                events = self._pull()
                if events is None:
                    self._set_state("error")
                    time.sleep(60)
                    continue
                with self.lock:
                    self.events = events
                    self.state = "connected"
                if self.on_change:
                    self.on_change()
            except Exception as e:  # noqa: BLE001 — keep the thread alive
                print(f"[scheduler] Google sync error: {e}")
                self._set_state("error")
            time.sleep(SYNC_INTERVAL_S)

    def _device_flow(self):
        st, resp = _post(DEVICE_CODE_URL,
                         {"client_id": self._client["client_id"], "scope": SCOPE})
        if st != 200 or "device_code" not in resp:
            self._set_state("error")
            return False
        device_code = resp["device_code"]
        interval = resp.get("interval", 5)
        verify = resp.get("verification_url") or resp.get("verification_uri", "")
        self._set_state("pending", resp.get("user_code", ""), verify)
        print(f"[scheduler] Google: visit {verify} and enter {resp.get('user_code')}")

        deadline = time.time() + resp.get("expires_in", 1800)
        while time.time() < deadline:
            time.sleep(interval)
            st, tok = _post(TOKEN_URL, {
                "client_id": self._client["client_id"],
                "client_secret": self._client["client_secret"],
                "device_code": device_code,
                "grant_type": GRANT_DEVICE,
            })
            if "access_token" in tok:
                self._access_token = tok["access_token"]
                self._access_expiry = time.time() + tok.get("expires_in", 3600)
                if tok.get("refresh_token"):
                    self._refresh_token = tok["refresh_token"]
                    self._save_refresh_token(self._refresh_token)
                return True
            err = tok.get("error", "")
            if err == "authorization_pending":
                continue
            if err == "slow_down":
                interval += 5
                continue
            # access_denied / expired_token / other → abort this attempt
            self._set_state("error")
            return False
        self._set_state("error")
        return False

    def _ensure_access(self):
        if self._access_token and time.time() < self._access_expiry - 60:
            return True
        st, tok = _post(TOKEN_URL, {
            "client_id": self._client["client_id"],
            "client_secret": self._client["client_secret"],
            "refresh_token": self._refresh_token,
            "grant_type": "refresh_token",
        })
        if "access_token" in tok:
            self._access_token = tok["access_token"]
            self._access_expiry = time.time() + tok.get("expires_in", 3600)
            return True
        return False

    def _pull(self):
        now = int(time.time())
        params = urllib.parse.urlencode({
            "timeMin": _rfc3339(now),
            "timeMax": _rfc3339(now + HORIZON_DAYS * 86400),
            "singleEvents": "true",
            "orderBy": "startTime",
            "maxResults": "50",
        })
        st, data = _get(CAL_EVENTS_URL + "?" + params, self._access_token)
        if st != 200:
            return None
        out = []
        for item in data.get("items", []):
            ev = self._normalize(item)
            if ev:
                out.append(ev)
        return out

    def _normalize(self, item):
        eid = item.get("id")
        if not eid:
            return None
        start, end = item.get("start", {}), item.get("end", {})
        if "dateTime" in start:
            s = _parse_dt(start["dateTime"])
            e = _parse_dt(end.get("dateTime", "")) or s
            all_day = False
        elif "date" in start:
            hour = int(self.cfg.get("all_day_reminder_hour", 8))
            s = _date_to_epoch(start["date"], hour)
            e = _date_to_epoch(end.get("date", start["date"]), 0)  # end date is exclusive
            all_day = True
        else:
            return None
        if s is None:
            return None
        return {
            "uid": "g:" + eid,
            "title": item.get("summary", "(no title)"),
            "location": item.get("location", ""),
            "start_utc": s,
            "end_utc": e or s,
            "all_day": all_day,
            "source": "google",
        }
