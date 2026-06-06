#!/usr/bin/env python3
"""
Register a set of ProtoHUD "Run Command" entries into KDE Connect so the paired
phone can trigger them remotely (KDE Connect app -> device -> Run command).

Two groups of commands:
  * System    -> run ProtoHUD's scripts / power actions directly.
  * In-app    -> write a GpioFunc id to the command FIFO (needs
                 inputs.command_fifo.enabled in config; see docs/kdeconnect-commands.md).

KDE Connect's runcommand plugin stores its commands as a compact-JSON map keyed
by UUID, under `commands=` in each paired device's
  ~/.config/kdeconnect/<device-id>/kdeconnect_runcommand/config
This script merges our entries in by NAME (so re-running is idempotent and your
own commands are preserved). Best-effort: if it can't find the config it prints
the JSON for you to paste into the KDE Connect GUI instead.

Usage:
  scripts/kdeconnect_commands.py                 # install for every paired device
  scripts/kdeconnect_commands.py --fifo /run/protohud/cmd
  scripts/kdeconnect_commands.py --no-fifo       # system commands only
  scripts/kdeconnect_commands.py --print         # just print the JSON, write nothing
After installing, reconnect the phone (or restart kdeconnectd) so it re-reads them.
"""

import argparse
import json
import os
import pathlib
import re
import uuid


def build_commands(root: str, fifo: str, include_fifo: bool):
    cmds = [
        # name, shell command  (System group — direct)
        ("Restart ProtoHUD",  f"{root}/scripts/restart.sh"),
        ("Update & Restart",  f"{root}/scripts/update.sh --restart"),
        ("Rollback Update",   f"{root}/scripts/rollback.sh --restart"),
        ("Reboot Pi",         "sudo reboot"),
        ("Shut Down",         "sudo poweroff"),
    ]
    if include_fifo:
        # In-app group — a GpioFunc id written to the command FIFO.
        for name, func in [
            ("Open/Close Menu",  "menu_open"),
            ("Menu Select",      "menu_select"),
            ("Menu Back",        "menu_back"),
            ("Capture Photo L",  "cam_capture_left"),
            ("Capture Photo R",  "cam_capture_right"),
            ("Autofocus L",      "cam_af_left"),
            ("Autofocus R",      "cam_af_right"),
            ("Swap Cameras",     "cam_swap"),
            ("Ring My Phone",    "phone_ring"),
            ("Boop: Snout",      "boop_snout"),
            ("Boop: Both",       "boop_both"),
        ]:
            cmds.append((name, f"echo {func} > {fifo}"))
    return cmds


def to_json_map(cmds):
    return {("{%s}" % uuid.uuid4()): {"name": n, "command": c} for n, c in cmds}


def merge_into_config(path: pathlib.Path, new_map: dict) -> bool:
    """Merge new_map (by command name) into the device's runcommand config."""
    text = path.read_text() if path.exists() else "[General]\n"
    existing = {}
    m = re.search(r"^commands=(.*)$", text, re.MULTILINE)
    if m:
        try:
            existing = json.loads(m.group(1).strip())
        except Exception:
            existing = {}
    have = {v.get("name") for v in existing.values() if isinstance(v, dict)}
    for k, v in new_map.items():
        if v["name"] not in have:
            existing[k] = v
    blob = json.dumps(existing, separators=(",", ":"))
    line = f"commands={blob}"
    if m:
        text = re.sub(r"^commands=.*$", lambda _: line, text, count=1, flags=re.MULTILINE)
    else:
        if "[General]" not in text:
            text = "[General]\n" + text
        text = text.replace("[General]\n", "[General]\n" + line + "\n", 1)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[1]),
                    help="ProtoHUD repo root (for script paths)")
    ap.add_argument("--fifo", default="/run/protohud/cmd",
                    help="command FIFO path for the in-app commands")
    ap.add_argument("--no-fifo", action="store_true", help="system commands only")
    ap.add_argument("--print", dest="just_print", action="store_true",
                    help="print the JSON map and exit (write nothing)")
    args = ap.parse_args()

    cmds = build_commands(args.root, args.fifo, include_fifo=not args.no_fifo)
    cmd_map = to_json_map(cmds)

    if args.just_print:
        print(json.dumps(cmd_map, indent=2))
        print("\n# Paste the compact form as `commands=<json>` into each device's")
        print("# ~/.config/kdeconnect/<id>/kdeconnect_runcommand/config, or add via the GUI.")
        return

    base = pathlib.Path.home() / ".config" / "kdeconnect"
    devices = [d for d in base.glob("*")
               if d.is_dir() and re.fullmatch(r"[0-9a-zA-Z_]+", d.name)] if base.exists() else []
    if not devices:
        print("No paired KDE Connect devices found under ~/.config/kdeconnect/.")
        print("Pair the phone first, then re-run. Meanwhile, here's the JSON to paste:\n")
        print(json.dumps(cmd_map, indent=2))
        return

    for d in devices:
        cfg = d / "kdeconnect_runcommand" / "config"
        try:
            merge_into_config(cfg, cmd_map)
            print(f"[ok] {cfg}  (+{len(cmd_map)} commands, merged by name)")
        except Exception as e:
            print(f"[warn] {cfg}: {e}")

    print("\nDone. Reconnect the phone or restart kdeconnectd so it re-reads the commands:")
    print("  kdeconnect-cli --refresh    # or: killall kdeconnectd  (it auto-respawns)")


if __name__ == "__main__":
    main()
