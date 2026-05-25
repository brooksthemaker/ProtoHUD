#!/usr/bin/env python3
"""ProtoHUD scheduler companion daemon.

Owns all scheduler networking so the C++ HUD never does HTTP/OAuth:
  * Serves a phone-friendly web form for manual calendar events.
  * (Future) syncs Google Calendar via OAuth device flow — see load_google_events().

It merges every source and atomically writes two files that the HUD's
SchedulerMonitor polls:
  * events.json          — {"version":1,"generated_utc":N,"events":[...]}
  * scheduler_status.json — {"web_url","gcal_state","event_count",...}

Event JSON shape (epoch UTC; the daemon does all timezone math):
  {"uid","title","location","start_utc","end_utc","all_day",
   "source":"manual"|"google"}

No external dependencies — Python 3 standard library only (Google support will
add libs to requirements.txt later). Runs on the same LAN as the phone; there is
no auth, matching a personal-device threat model. User text is HTML-escaped when
rendered to avoid stored XSS.
"""

import html
import json
import os
import socket
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ── Config resolution ──────────────────────────────────────────────────────────

DEFAULTS = {
    "web_port": 8770,
    "events_path": os.path.expanduser(
        "~/.local/share/protohud-scheduler/events.json"),
    "status_path": os.path.expanduser(
        "~/.local/share/protohud-scheduler/scheduler_status.json"),
    "all_day_reminder_hour": 8,
    "heartbeat_s": 60,
}


def find_config():
    here = os.path.dirname(__file__)
    candidates = [
        os.environ.get("SCHEDULER_CONFIG"),
        os.path.join(here, "config.json"),
        os.path.join(here, "..", "config", "config.json"),   # repo: ../config/config.json
        os.path.join(here, "..", "build", "config.json"),    # in-tree build dir
        os.path.expanduser("~/protohud/config/config.json"),
        os.path.expanduser("~/protohud/build/config.json"),
        "/etc/protohud/config.json",
    ]
    for p in candidates:
        if p and os.path.isfile(p):
            return p
    return None


def load_config():
    cfg = dict(DEFAULTS)
    path = find_config()
    if path:
        try:
            with open(path) as f:
                block = json.load(f).get("scheduler", {})
            for k in DEFAULTS:
                if k in block:
                    cfg[k] = block[k]
            print(f"[scheduler] config from {path}")
        except Exception as e:  # noqa: BLE001 — best-effort; fall back to defaults
            print(f"[scheduler] config read failed ({e}); using defaults")
    else:
        print("[scheduler] no config found; using defaults")
    return cfg


# ── Atomic JSON I/O ─────────────────────────────────────────────────────────────

def atomic_write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)  # POSIX-atomic; HUD sees whole-old or whole-new file


def read_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:  # noqa: BLE001 — missing/corrupt → default
        return default


def _lan_ips():
    """Non-loopback IPv4 addresses from `hostname -I` (best-effort)."""
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True,
                             text=True, timeout=3).stdout
        return [ip for ip in out.split() if "." in ip and not ip.startswith("127.")]
    except Exception:  # noqa: BLE001
        return []


def _is_private(ip):
    return (ip.startswith("192.168.") or ip.startswith("10.")
            or any(ip.startswith(f"172.{b}.") for b in range(16, 32)))


def detect_ip():
    # Prefer the egress interface toward the internet.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # no packets sent; just picks the egress iface
        ip = s.getsockname()[0]
        if ip and not ip.startswith("127."):
            return ip
    except Exception:  # noqa: BLE001
        pass
    finally:
        s.close()
    # Fall back to a LAN address (prefer private ranges) when there's no default route.
    ips = _lan_ips()
    for ip in ips:
        if _is_private(ip):
            return ip
    return ips[0] if ips else "127.0.0.1"


# ── Scheduler store ──────────────────────────────────────────────────────────────

class Store:
    """Owns manual events and publishes the merged events.json + status."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        data_dir = os.path.dirname(cfg["events_path"])
        os.makedirs(data_dir, exist_ok=True)
        self.manual_path = os.path.join(data_dir, "manual_events.json")
        self.manual = read_json(self.manual_path, {"events": []}).get("events", [])

    # --- manual event mutations (call under self.lock via public methods) ---
    def add_manual(self, ev):
        with self.lock:
            self.manual.append(ev)
            self._save_manual()
        self.publish()

    def delete_manual(self, uid):
        with self.lock:
            self.manual = [e for e in self.manual if e.get("uid") != uid]
            self._save_manual()
        self.publish()

    def list_events(self):
        with self.lock:
            return sorted(self._merged(), key=lambda e: e["start_utc"])

    def _save_manual(self):
        atomic_write_json(self.manual_path, {"events": self.manual})

    def _merged(self):
        # Manual + Google (Google returns [] until that pass lands).
        return list(self.manual) + load_google_events(self.cfg)

    # --- publish to the HUD-facing files ---
    def publish(self):
        with self.lock:
            events = sorted(self._merged(), key=lambda e: e["start_utc"])
        atomic_write_json(self.cfg["events_path"],
                          {"version": 1,
                           "generated_utc": int(time.time()),
                           "events": events})
        atomic_write_json(self.cfg["status_path"], {
            "web_url": f"http://{detect_ip()}:{self.cfg['web_port']}",
            "gcal_state": google_state(self.cfg),
            "gcal_user_code": "",
            "gcal_verify_url": "",
            "last_sync_utc": int(time.time()),
            "event_count": len(events),
        })


# ── Google Calendar seam (implemented in a later pass) ───────────────────────────

def google_state(cfg):
    return "disconnected"


def load_google_events(cfg):
    return []


# ── Time helpers ─────────────────────────────────────────────────────────────────

def local_to_epoch(date_str, time_str):
    """'YYYY-MM-DD' + 'HH:MM' (local time) → epoch UTC, or None."""
    try:
        tm = time.strptime(f"{date_str} {time_str}", "%Y-%m-%d %H:%M")
        return int(time.mktime(tm))  # mktime interprets the tuple as local time
    except (ValueError, TypeError):
        return None


def make_event_from_form(form, all_day_hour):
    title = (form.get("title", [""])[0] or "").strip() or "(untitled)"
    location = (form.get("location", [""])[0] or "").strip()
    date = form.get("date", [""])[0]
    all_day = form.get("all_day", [""])[0] in ("on", "1", "true")

    if all_day:
        start = local_to_epoch(date, f"{int(all_day_hour):02d}:00")
        end = local_to_epoch(date, "23:59")
    else:
        start = local_to_epoch(date, form.get("start_time", [""])[0])
        end = local_to_epoch(date, form.get("end_time", [""])[0]) or start
    if start is None:
        return None
    return {
        "uid": uuid.uuid4().hex,
        "title": title,
        "location": location,
        "start_utc": start,
        "end_utc": end or start,
        "all_day": all_day,
        "source": "manual",
    }


# ── Web UI ───────────────────────────────────────────────────────────────────────

PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ProtoHUD Scheduler</title><style>
body{{font-family:system-ui,sans-serif;margin:0;background:#0b0f14;color:#e6f0ee}}
.wrap{{max-width:520px;margin:0 auto;padding:16px}}
h1{{font-size:1.3rem;color:#00dcb4}} h2{{font-size:1rem;color:#9bb;margin-top:24px}}
label{{display:block;margin:10px 0 4px;font-size:.85rem;color:#9bb}}
input[type=text],input[type=date],input[type=time]{{width:100%;padding:12px;
 font-size:1rem;border:1px solid #244;border-radius:8px;background:#101820;color:#e6f0ee}}
.row{{display:flex;gap:10px}} .row>div{{flex:1}}
.chk{{display:flex;align-items:center;gap:8px;margin:12px 0}}
button{{margin-top:16px;width:100%;padding:14px;font-size:1rem;border:0;border-radius:8px;
 background:#00b894;color:#012;font-weight:600}}
ul{{list-style:none;padding:0}} li{{background:#101820;border:1px solid #1d2a30;
 border-radius:8px;padding:10px 12px;margin:8px 0;display:flex;justify-content:space-between;align-items:center}}
.when{{color:#00dcb4;font-size:.8rem}} .del{{width:auto;margin:0;padding:8px 12px;background:#933;color:#fee}}
</style></head><body><div class="wrap">
<h1>ProtoHUD Scheduler</h1>
<form method="POST" action="/add">
<label>Title</label><input type="text" name="title" required>
<label>Location</label><input type="text" name="location">
<label>Date</label><input type="date" name="date" required>
<div class="chk"><input type="checkbox" name="all_day" id="ad"><label for="ad" style="margin:0">All day</label></div>
<div class="row"><div><label>Start</label><input type="time" name="start_time"></div>
<div><label>End</label><input type="time" name="end_time"></div></div>
<button type="submit">Add event</button>
</form>
<h2>Upcoming</h2><ul>{rows}</ul>
</div></body></html>"""


def render_rows(events):
    now = int(time.time())
    rows = []
    for e in events:
        if e["end_utc"] and e["end_utc"] < now:
            continue
        tm = time.localtime(e["start_utc"])
        when = (time.strftime("%a %b %d (all day)", tm) if e["all_day"]
                else time.strftime("%a %b %d %H:%M", tm))
        title = html.escape(e["title"])
        loc = (" · " + html.escape(e["location"])) if e["location"] else ""
        src = "" if e["source"] == "manual" else " [gcal]"
        delete = "" if e["source"] != "manual" else (
            f'<form method="POST" action="/delete">'
            f'<input type="hidden" name="uid" value="{html.escape(e["uid"])}">'
            f'<button class="del" type="submit">x</button></form>')
        rows.append(f'<li><div><div class="when">{when}{src}</div>'
                    f'{title}{loc}</div>{delete}</li>')
    return "".join(rows) or '<li><div>(no upcoming events)</div></li>'


def make_handler(store):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):  # quieter logs
            pass

        def _send(self, code, body, ctype="text/html; charset=utf-8"):
            data = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _redirect(self):
            self.send_response(303)
            self.send_header("Location", "/")
            self.end_headers()

        def do_GET(self):
            if urlparse(self.path).path != "/":
                self._send(404, "not found")
                return
            self._send(200, PAGE.format(rows=render_rows(store.list_events())))

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            form = parse_qs(self.rfile.read(length).decode("utf-8"))
            path = urlparse(self.path).path
            if path == "/add":
                ev = make_event_from_form(form, store.cfg["all_day_reminder_hour"])
                if ev:
                    store.add_manual(ev)
                self._redirect()
            elif path == "/delete":
                uid = form.get("uid", [""])[0]
                if uid:
                    store.delete_manual(uid)
                self._redirect()
            else:
                self._send(404, "not found")

    return Handler


# ── Heartbeat: keep events.json mtime fresh so the HUD sees the daemon as online ──

def heartbeat(store, period_s):
    while True:
        time.sleep(period_s)
        try:
            store.publish()
        except Exception as e:  # noqa: BLE001
            print(f"[scheduler] heartbeat publish failed: {e}")


def main():
    cfg = load_config()
    store = Store(cfg)
    store.publish()  # write initial events.json + status

    threading.Thread(target=heartbeat,
                     args=(store, int(cfg["heartbeat_s"])),
                     daemon=True).start()

    httpd = ThreadingHTTPServer(("0.0.0.0", int(cfg["web_port"])),
                                make_handler(store))
    url = f"http://{detect_ip()}:{cfg['web_port']}"
    print(f"[scheduler] web form at {url}")
    others = _lan_ips()
    if others:
        print("[scheduler] reachable on: " +
              ", ".join(f"http://{ip}:{cfg['web_port']}" for ip in others))
    print(f"[scheduler] events -> {cfg['events_path']}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[scheduler] shutting down")
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
