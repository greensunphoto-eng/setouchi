#!/usr/bin/env python3
"""Claude Code hook / statusline entry point for Claude DS.

Claude Code runs this script for every configured hook event and passes the
event JSON on stdin. The script forwards it to the local bridge
(claude_ds_bridge.py) and, for PermissionRequest, waits for the answer the
user gives on the Nintendo DS.

Rules this script follows so it can never get in Claude Code's way:
  * standard library only (fast start, nothing to install)
  * if the bridge is not running, it exits 0 silently
  * for PermissionRequest it prints a decision only when the DS answered;
    otherwise it prints nothing and the normal permission dialog appears

Usage (installed automatically by `claude_ds_bridge.py --install-hooks`):
  python claude_ds_hook.py               # hook mode (event JSON on stdin)
  python claude_ds_hook.py --statusline  # statusline mode (statusline JSON on stdin)
"""

import json
import os
import sys
import urllib.request

BRIDGE_URL = os.environ.get("CLAUDE_DS_BRIDGE", "http://127.0.0.1:8787").rstrip("/")

# Claude Code's default command-hook timeout is 600 s; the installer sets the
# same value explicitly. Stay a little below it so we always answer in time.
PERMISSION_WAIT_S = float(os.environ.get("CLAUDE_DS_PERMISSION_WAIT", "590"))

# Never route localhost traffic through an HTTP proxy from the environment.
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _post(path, payload, timeout):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BRIDGE_URL + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with _OPENER.open(req, timeout=timeout) as resp:
        body = resp.read()
    return json.loads(body.decode("utf-8")) if body else {}


def _read_stdin_json():
    try:
        raw = sys.stdin.buffer.read().decode("utf-8", "replace")
    except Exception:
        return {}
    if not raw.strip():
        return {}
    try:
        value = json.loads(raw)
    except ValueError:
        return {}
    return value if isinstance(value, dict) else {}


def _pct(value):
    try:
        return "%d%%" % round(float(value))
    except (TypeError, ValueError):
        return None


def statusline_text(payload, bridge_reply):
    """Builds a compact, ASCII-only status line for the Claude Code UI."""
    parts = []
    if bridge_reply is None:
        parts.append("DS:off")
    else:
        ds = "on" if bridge_reply.get("device") else "wait"
        if bridge_reply.get("remote") and bridge_reply.get("device"):
            ds += "+R"
        parts.append("DS:" + ds)

    model = payload.get("model") or {}
    name = model.get("display_name") if isinstance(model, dict) else None
    if name:
        parts.append(str(name))

    ctx = payload.get("context_window") or {}
    if isinstance(ctx, dict) and _pct(ctx.get("used_percentage")):
        parts.append("ctx " + _pct(ctx.get("used_percentage")))

    limits = payload.get("rate_limits") or {}
    if isinstance(limits, dict):
        for key, label in (("five_hour", "5h"), ("seven_day", "7d")):
            window = limits.get(key) or {}
            if isinstance(window, dict) and _pct(window.get("used_percentage")):
                parts.append(label + " " + _pct(window.get("used_percentage")))

    text = " | ".join(parts)
    return text.encode("ascii", "replace").decode("ascii")


def run_statusline():
    payload = _read_stdin_json()
    try:
        reply = _post("/statusline", payload, timeout=0.3)
    except Exception:
        reply = None
    sys.stdout.write(statusline_text(payload, reply) + "\n")
    return 0


def permission_output(decision):
    """Maps a DS decision to the PermissionRequest hook stdout JSON (or None)."""
    if decision == "allow":
        return {
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {"behavior": "allow"},
            }
        }
    if decision == "deny":
        return {
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {
                    "behavior": "deny",
                    "message": "The user denied this from the Nintendo DS remote.",
                },
            }
        }
    if decision == "stop":
        return {
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {
                    "behavior": "deny",
                    "message": "The user pressed STOP on the Nintendo DS remote.",
                    "interrupt": True,
                },
            }
        }
    return None


def run_hook():
    payload = _read_stdin_json()
    event = payload.get("hook_event_name")

    if event == "PermissionRequest":
        try:
            reply = _post("/permission", payload, timeout=PERMISSION_WAIT_S)
        except Exception:
            return 0  # bridge not running -> normal dialog
        out = permission_output(reply.get("decision"))
        if out is not None:
            sys.stdout.write(json.dumps(out) + "\n")
        return 0

    try:
        _post("/hook", payload, timeout=1.5)
    except Exception:
        pass
    return 0


def main(argv):
    try:
        if "--statusline" in argv:
            return run_statusline()
        return run_hook()
    except Exception:
        # A status display must never break Claude Code.
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
