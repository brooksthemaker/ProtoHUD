#!/usr/bin/env python3
"""Deep-merge config.example.json defaults into the user's config.json.

After an update, the shipped config.example.json may have gained keys for new
features. This adds those new keys/defaults to the user's config WITHOUT ever
overwriting values the user already set. Existing values win; lists (custom
effects, fan zones, etc.) are kept exactly as the user has them.

ProtoHUD also migrates missing keys in-memory at load time (per-field defaults),
so this is belt-and-suspenders — but it makes config.json complete on disk
immediately after an update, and reports what changed.

Usage:
    merge_config.py <user_config.json> <example_config.json>

Exit code is always 0 for "nothing to do" / recoverable conditions so it never
aborts an update; only a usage error returns non-zero.
"""
import json
import os
import sys


def deep_merge(user, example):
    """Return a copy of `user` with any keys missing from it filled in from
    `example`. Recurses into nested objects. For anything that isn't a dict on
    both sides (scalars, lists), the user's value is kept verbatim."""
    if isinstance(user, dict) and isinstance(example, dict):
        out = dict(user)
        for key, ex_val in example.items():
            if key in out:
                out[key] = deep_merge(out[key], ex_val)
            else:
                out[key] = ex_val
        return out
    return user


def added_paths(user, example, prefix=""):
    """List the dotted key paths that would be newly added (for logging)."""
    added = []
    if isinstance(user, dict) and isinstance(example, dict):
        for key, ex_val in example.items():
            path = f"{prefix}{key}"
            if key not in user:
                added.append(path)
            else:
                added.extend(added_paths(user[key], ex_val, path + "."))
    return added


def main():
    if len(sys.argv) != 3:
        print("usage: merge_config.py <user.json> <example.json>", file=sys.stderr)
        return 2
    user_path, ex_path = sys.argv[1], sys.argv[2]

    try:
        with open(ex_path) as f:
            example = json.load(f)
    except Exception as e:  # noqa: BLE001 — best-effort, never abort the update
        print(f"[merge_config] no/invalid example ({e}); nothing to merge")
        return 0

    if not os.path.exists(user_path):
        # No user config yet — seed it straight from the example defaults.
        with open(user_path, "w") as f:
            json.dump(example, f, indent=2)
        print("[merge_config] no existing config — seeded from example defaults")
        return 0

    try:
        with open(user_path) as f:
            user = json.load(f)
    except Exception as e:  # noqa: BLE001
        # Don't touch a config we can't parse — the user's edits matter more
        # than the merge. ProtoHUD's loader handles defaults at runtime.
        print(f"[merge_config] config.json didn't parse ({e}); left untouched")
        return 0

    new_keys = added_paths(user, example)
    if not new_keys:
        print("[merge_config] config already has all current keys — no change")
        return 0

    merged = deep_merge(user, example)
    with open(user_path, "w") as f:
        json.dump(merged, f, indent=2)
    print(f"[merge_config] added {len(new_keys)} new default key(s), "
          f"user values preserved: {', '.join(new_keys[:12])}"
          + (" …" if len(new_keys) > 12 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
