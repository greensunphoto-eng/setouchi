#!/usr/bin/env python3
"""Claude DS bridge: Claude Code status on a Nintendo DS (+ remote approvals).

This is the PC side of Claude DS. It sits between three things:

  Claude Code --(hooks, claude_ds_hook.py)--> HTTP 127.0.0.1:8787 --> bridge
  bridge --(USB serial)--> ESP32 "CLAUDE-DS" Wi-Fi --> Nintendo DS app
  bridge <--(TCP, router mode)--> Nintendo DS app        (alternative)

Quick start:
  pip install pyserial psutil
  python claude_ds_bridge.py --install-hooks   # once: add the Claude Code hooks
  python claude_ds_bridge.py                   # run (auto-detects the ESP32)
  python claude_ds_bridge.py --demo            # test the DS without Claude Code

The DS <-> bridge line protocol is documented in ../PROTOCOL.md.
"""

import argparse
import json
import os
import re
import secrets
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
from collections import OrderedDict, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:  # Only needed for the ESP32 (USB serial) mode.
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - depends on the user's machine
    serial = None
    list_ports = None

try:  # Optional: CPU / memory numbers on the DS.
    import psutil
except ImportError:  # pragma: no cover - depends on the user's machine
    psutil = None

VERSION = "1.0.0"
PROTO = 1

DEFAULT_HTTP_PORT = 8787
DEFAULT_TCP_PORT = 7777
SERIAL_BAUD = 115200

DEVICE_TIMEOUT_S = 20        # DS sends PING every 5 s
PERMISSION_TIMEOUT_S = 585   # must stay below the hook's own wait (590 s)
SESSION_EXPIRE_S = 6 * 3600  # forget idle sessions after this long
STALE_WORK_S = 30 * 60       # "working" with no activity for this long -> idle
NAME_MAX = 14
TOOL_MAX = 20
DETAIL_MAX = 180
MAX_ROWS = 6

# USB vendor ids of the USB-serial chips found on ESP32 boards.
KNOWN_USB_VIDS = {
    0x303A: "Espressif native USB",
    0x10C4: "Silicon Labs CP210x",
    0x1A86: "WCH CH34x/CH910x",
    0x0403: "FTDI",
}

WAIT_NOTIFICATIONS = {
    "permission_prompt": "permission",
    "elicitation_dialog": "question",
    "elicitation_url_dialog": "question",
    "agent_needs_input": "input",
}

ANSWERS = ("allow", "deny", "stop", "pc")

_log_lock = threading.Lock()


def log(msg):
    with _log_lock:
        print(time.strftime("[%H:%M:%S] ") + msg, flush=True)


# ---------------------------------------------------------------------------
# Text helpers (the DS font is ASCII only)
# ---------------------------------------------------------------------------

_NAME_BAD = re.compile(r"[^A-Za-z0-9._-]+")
_TOOL_BAD = re.compile(r"[^A-Za-z0-9:/._-]+")


def ascii_name(text, maxlen=NAME_MAX):
    """Single-token ASCII name for the DS (no spaces)."""
    text = _NAME_BAD.sub("-", str(text or "")).strip("-.")
    return text[:maxlen].rstrip("-.")


def ascii_text(text, maxlen=DETAIL_MAX):
    """Printable ASCII; runs of non-ASCII characters collapse into one '?'."""
    out = []
    prev_q = False
    for ch in str(text or ""):
        code = ord(ch)
        if ch in "\r\n\t":
            ch, code = " ", 32
        if 32 <= code < 127:
            out.append(ch)
            prev_q = False
        elif code >= 127 and not prev_q:
            out.append("?")
            prev_q = True
    s = re.sub(r"\s+", " ", "".join(out)).strip()
    if len(s) > maxlen:
        s = s[: maxlen - 3].rstrip() + "..."
    return s


def short_tool(tool):
    t = str(tool or "?")
    if t.startswith("mcp__"):
        t = "mcp:" + "/".join(p for p in t.split("__")[1:] if p)
    t = _TOOL_BAD.sub("", t)
    return t[:TOOL_MAX] or "?"


def _short_path(path):
    path = str(path)
    parts = [p for p in re.split(r"[\\/]+", path) if p]
    if len(path) <= 40 or len(parts) <= 3:
        return path
    return ".../" + "/".join(parts[-3:])


def summarize_tool(tool, tool_input):
    """One-line description of what Claude wants to do, for the DS screen."""
    ti = tool_input if isinstance(tool_input, dict) else {}

    def first(*keys):
        for key in keys:
            value = ti.get(key)
            if isinstance(value, str) and value.strip():
                return value
        return None

    if tool == "Bash":
        text = first("command") or first("description") or "(command)"
    elif tool in ("Edit", "MultiEdit", "Write", "Read", "NotebookEdit"):
        path = first("file_path", "notebook_path", "path")
        text = _short_path(path) if path else tool
    elif tool in ("Glob", "Grep"):
        text = first("pattern") or ""
        if first("path"):
            text += " in " + _short_path(first("path"))
    elif tool == "WebFetch":
        text = first("url") or ""
    elif tool == "WebSearch":
        text = first("query") or ""
    elif tool in ("Task", "Agent"):
        text = first("description", "prompt") or tool
    else:
        try:
            text = json.dumps(ti, ensure_ascii=False, sort_keys=True)
        except (TypeError, ValueError):
            text = str(ti)
    return ascii_text(text) or "-"


def session_display_name(cwd, sid):
    # Split on both separators: Windows paths may reach a non-Windows test run.
    base = re.split(r"[\\/]+", str(cwd or "").rstrip("/\\"))[-1]
    name = ascii_name(base)
    if len(name) >= 2:
        return name
    return "s-" + ascii_name(sid)[:6]


# ---------------------------------------------------------------------------
# PC stats
# ---------------------------------------------------------------------------


class SystemStats:
    def __init__(self):
        self._disk_path = os.path.expanduser("~")
        if psutil is not None:
            try:
                psutil.cpu_percent(None)  # first call primes the counter
            except Exception:
                pass

    def snapshot(self):
        # Many events can ask within a second; psutil's CPU % is measured
        # between calls, so hand out a cached value instead of re-sampling.
        now = time.monotonic()
        cached = getattr(self, "_cached", None)
        if cached is not None and now - cached[0] < 0.9:
            return cached[1]
        result = self._sample()
        self._cached = (now, result)
        return result

    def _sample(self):
        cpu = mem = dsk = -1
        if psutil is not None:
            try:
                cpu = int(round(psutil.cpu_percent(None)))
                mem = int(round(psutil.virtual_memory().percent))
            except Exception:
                pass
        elif hasattr(os, "getloadavg"):
            try:
                load = os.getloadavg()[0]
                cpu = min(100, int(load / (os.cpu_count() or 1) * 100))
            except OSError:
                pass
        try:
            dsk = int(shutil.disk_usage(self._disk_path).free / 1e9)
        except OSError:
            pass
        return cpu, mem, dsk


def _git(cwd, *args):
    if not cwd or not os.path.isdir(cwd):
        return None
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = 0x08000000  # CREATE_NO_WINDOW
    try:
        res = subprocess.run(
            ["git", "-C", cwd] + list(args),
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=5, **kwargs)
    except (OSError, subprocess.SubprocessError):
        return None
    return res.stdout.strip() if res.returncode == 0 else None


class GitWatcher:
    """Counts commits not pushed to any remote, across the active projects."""

    def __init__(self):
        self.value = -1
        self._last = -1e9
        self._busy = False

    def maybe_refresh(self, cwds, force=False):
        now = time.monotonic()
        if self._busy:
            return
        if now - self._last < (5 if force else 30):
            return
        self._busy = True
        self._last = now
        threading.Thread(target=self._run, args=(sorted(set(cwds)),), daemon=True).start()

    def _run(self, cwds):
        try:
            roots = set()
            for cwd in cwds:
                root = _git(cwd, "rev-parse", "--show-toplevel")
                if root:
                    roots.add(root)
            total, counted = 0, False
            for root in roots:
                if not _git(root, "remote"):
                    continue  # no remote: "unpushed" is meaningless
                count = _git(root, "rev-list", "--count", "HEAD", "--not", "--remotes")
                if count and count.isdigit():
                    total += int(count)
                    counted = True
            self.value = total if counted else -1
        finally:
            self._busy = False


# ---------------------------------------------------------------------------
# Core state
# ---------------------------------------------------------------------------


class Session:
    def __init__(self, sid, now):
        self.sid = sid
        self.name = "s-" + ascii_name(sid)[:6]
        self.cwd = ""
        self.state = "I"  # W=working A=needs you D=done I=idle
        self.since = now
        self.last_seen = now
        self.ctx = -1
        self.ctx_at = 0.0
        self.last_wait_ev = -1e9
        self.last_done_ev = -1e9

    def set_state(self, state, now):
        if state != self.state:
            self.state = state
            self.since = now


class PendingRequest:
    def __init__(self, rid, session, tool, detail, created):
        self.rid = rid
        self.session = session
        self.tool = tool
        self.detail = detail
        self.created = created
        self.answer = None
        self.event = threading.Event()


class BridgeState:
    """Everything the bridge knows. All methods are thread-safe."""

    def __init__(self, remote=False, permission_timeout=PERMISSION_TIMEOUT_S,
                 stats=None, git=None):
        self.lock = threading.RLock()
        self.sessions = {}
        self.pending = OrderedDict()
        self.shown_pr = None
        self.shown_more = -1
        self.remote = remote
        self.device_online = False
        self.last_device_rx = 0.0
        self.h5 = -1
        self.wk = -1
        self.limits_at = -1e9
        self.permission_timeout = permission_timeout
        self.stats = stats or SystemStats()
        self.git = git or GitWatcher()
        self.sender = None  # set by the device link
        self.history = deque(maxlen=50)
        self._ticks = 0

    # -- output ------------------------------------------------------------

    def _send(self, line):
        sender = self.sender
        if sender is None or not self.device_online:
            return
        try:
            sender(line)
        except Exception as exc:  # link problems are handled by the link
            log("DS로 보내기 실패: %s" % exc)

    def _note(self, text):
        self.history.append((time.strftime("%H:%M:%S"), text))

    def _status_line(self):
        now = time.time()
        sessions = list(self.sessions.values())
        working = sum(1 for s in sessions if s.state == "W")
        waiting = sum(1 for s in sessions if s.state == "A")
        ctx_values = [s.ctx for s in sessions if s.ctx >= 0 and now - s.ctx_at < 3600]
        ctx = max(ctx_values) if ctx_values else -1
        cpu, mem, dsk = self.stats.snapshot()
        fresh = now - self.limits_at < 6 * 3600
        return ("ST ses=%d wrk=%d wai=%d cpu=%d mem=%d dsk=%d unp=%d ctx=%d "
                "h5=%d wk=%d rem=%d pend=%d clk=%s" % (
                    len(sessions), working, waiting, cpu, mem, dsk, self.git.value,
                    ctx, self.h5 if fresh else -1, self.wk if fresh else -1,
                    1 if self.remote else 0, len(self.pending),
                    time.strftime("%H:%M")))

    def _session_lines(self):
        order = {"A": 0, "W": 1, "D": 2, "I": 3}
        rows = sorted(self.sessions.values(),
                      key=lambda s: (order.get(s.state, 9), -s.last_seen))
        now = time.time()
        names = {}
        lines = ["SC %d %d" % (min(len(rows), MAX_ROWS), len(rows))]
        for idx, sess in enumerate(rows[:MAX_ROWS]):
            name = sess.name or "-"
            if name in names:  # two sessions in the same folder
                name = (name[:NAME_MAX - 3] + "-" + ascii_name(sess.sid)[:2])
            names[name] = True
            ctx = sess.ctx if (sess.ctx >= 0 and now - sess.ctx_at < 3600) else -1
            lines.append("SI %d %s %d %d %s" % (
                idx, sess.state, int(max(0, now - sess.since)), ctx, name))
        return lines

    def _push_sessions(self):
        for line in self._session_lines():
            self._send(line)

    def _sync_pr(self, force=False):
        head = next(iter(self.pending.values()), None)
        if head is None:
            if self.shown_pr:
                self._send("PX %s" % self.shown_pr)
            self.shown_pr = None
            self.shown_more = -1
            return
        more = len(self.pending) - 1
        if force or head.rid != self.shown_pr or more != self.shown_more:
            self._send("PR %s %d %s %s %s" % (head.rid, more, head.tool,
                                               head.session, head.detail))
            self.shown_pr = head.rid
            self.shown_more = more

    def _full_sync(self):
        self._send("HI %d %s" % (PROTO, VERSION))
        self._send(self._status_line())
        self._push_sessions()
        self.shown_pr = None
        self._sync_pr(force=True)

    # -- sessions ----------------------------------------------------------

    def _touch(self, payload, now):
        sid = str(payload.get("session_id") or "")
        sess = self.sessions.get(sid)
        if sess is None:
            sess = Session(sid, now)
            self.sessions[sid] = sess
        sess.last_seen = now
        cwd = payload.get("cwd")
        if not cwd:
            ws = payload.get("workspace")
            if isinstance(ws, dict):
                cwd = ws.get("current_dir") or ws.get("project_dir")
        if cwd:
            sess.cwd = str(cwd)
            sess.name = session_display_name(sess.cwd, sid)
        return sess

    def _wait_event(self, sess, reason, now):
        if now - sess.last_wait_ev < 30:
            return
        sess.last_wait_ev = now
        self._send("EV WAIT %s %s" % (sess.name, reason))
        self._note("WAIT %s %s" % (sess.name, reason))
        log("확인 필요: %s (%s)" % (sess.name, reason))

    def handle_hook(self, payload):
        if not isinstance(payload, dict):
            return
        event = payload.get("hook_event_name") or ""
        sid = str(payload.get("session_id") or "")
        if not sid:
            return
        now = time.time()
        with self.lock:
            if event == "SessionEnd":
                sess = self.sessions.pop(sid, None)
                if sess is not None:
                    log("세션 종료: %s" % sess.name)
                self._push_sessions()
                self._send(self._status_line())
                return

            sess = self._touch(payload, now)
            if event == "SessionStart":
                if payload.get("source") in (None, "startup", "clear"):
                    sess.set_state("I", now)
                log("세션 시작: %s" % sess.name)
            elif event == "UserPromptSubmit":
                sess.set_state("W", now)
            elif event in ("PreToolUse", "PostToolUse", "PostToolUseFailure"):
                # Only "needs you" -> "working" (the prompt was answered on the
                # PC). Async hooks can arrive out of order, so a late tool event
                # must not undo a Stop.
                if sess.state == "A":
                    sess.set_state("W", now)
            elif event == "Notification":
                ntype = payload.get("notification_type") or ""
                if not ntype:
                    msg = str(payload.get("message") or "").lower()
                    if "permission" in msg:
                        ntype = "permission_prompt"
                    elif "waiting for your input" in msg:
                        ntype = "idle_prompt"
                if ntype in WAIT_NOTIFICATIONS:
                    sess.set_state("A", now)
                    self._wait_event(sess, WAIT_NOTIFICATIONS[ntype], now)
                elif ntype == "idle_prompt" and sess.state not in ("D", "I"):
                    sess.set_state("I", now)
            elif event == "Stop":
                sess.set_state("D", now)
                if now - sess.last_done_ev > 2:
                    sess.last_done_ev = now
                    self._send("EV DONE %s" % sess.name)
                    self._note("DONE %s" % sess.name)
                    log("작업 완료: %s" % sess.name)
                self.git.maybe_refresh([s.cwd for s in self.sessions.values()], force=True)
            self._push_sessions()
            self._send(self._status_line())

    def handle_statusline(self, payload):
        now = time.time()
        with self.lock:
            before = self._usage_key()
            if isinstance(payload, dict) and payload.get("session_id"):
                sess = self._touch(payload, now)
                ctx = payload.get("context_window")
                if isinstance(ctx, dict) and ctx.get("used_percentage") is not None:
                    try:
                        sess.ctx = int(round(float(ctx["used_percentage"])))
                        sess.ctx_at = now
                    except (TypeError, ValueError):
                        pass
                limits = payload.get("rate_limits")
                if isinstance(limits, dict):
                    for key, attr in (("five_hour", "h5"), ("seven_day", "wk")):
                        window = limits.get(key)
                        if isinstance(window, dict) and window.get("used_percentage") is not None:
                            try:
                                setattr(self, attr, int(round(float(window["used_percentage"]))))
                                self.limits_at = now
                            except (TypeError, ValueError):
                                pass
            if self._usage_key() != before:  # new CTX / 5H / WEEK: show them now
                self._send(self._status_line())
            return {"device": self.device_online, "remote": self.remote}

    def _usage_key(self):
        return (tuple(sorted((s.sid, s.ctx) for s in self.sessions.values())),
                self.h5, self.wk)

    # -- permission requests ------------------------------------------------

    def request_permission(self, payload, client_alive=lambda: True):
        """Blocks until the DS answers (or it can't). Returns {"decision": ...}."""
        now = time.time()
        payload = payload if isinstance(payload, dict) else {}
        tool = payload.get("tool_name") or "?"
        with self.lock:
            sess = self._touch(payload, now) if payload.get("session_id") else None
            if sess is not None:
                sess.set_state("A", now)
            if not (self.remote and self.device_online):
                if sess is not None:
                    self._wait_event(sess, "permission", now)
                self._push_sessions()
                self._send(self._status_line())
                return {"decision": None,
                        "reason": "remote-off" if not self.remote else "device-offline"}
            rid = secrets.token_hex(3)
            req = PendingRequest(rid, sess.name if sess else "-", short_tool(tool),
                                 summarize_tool(tool, payload.get("tool_input")), now)
            self.pending[rid] = req
            log("권한 요청 -> DS [%s] %s: %s" % (req.session, req.tool, req.detail))
            self._note("ASK %s %s" % (req.session, req.tool))
            self._push_sessions()
            self._sync_pr()
            self._send(self._status_line())

        deadline = time.monotonic() + self.permission_timeout
        reason = "timeout"
        while True:
            if req.event.wait(0.25):
                reason = "answered"
                break
            if time.monotonic() >= deadline:
                break
            if not client_alive():
                reason = "cancelled"
                break
            with self.lock:
                if not self.remote:
                    reason = "remote-off"
                    break
                if not self.device_online:
                    reason = "device-offline"
                    break

        with self.lock:
            self.pending.pop(rid, None)
            answer = req.answer if reason == "answered" else None
            decision = answer if answer in ("allow", "deny", "stop") else None
            if decision is None:
                self._send("PX %s" % rid)
                if answer == "pc":
                    reason = "pc"
            if self.shown_pr == rid:
                self.shown_pr = None
            self._sync_pr()
            later = time.time()
            if sess is not None:
                if decision in ("allow", "deny"):
                    sess.set_state("W", later)
                elif decision == "stop":
                    sess.set_state("I", later)
            self._push_sessions()
            self._send(self._status_line())
        label = {"allow": "허용", "deny": "거절", "stop": "중지"}.get(decision)
        if label:
            log("DS에서 %s: [%s] %s" % (label, req.session, req.tool))
        else:
            log("PC 화면에서 결정하도록 넘김 (%s): [%s] %s" % (reason, req.session, req.tool))
        return {"decision": decision, "reason": reason}

    def _answer(self, rid, choice):
        req = self.pending.get(rid)
        if req is None:
            self._send("PX %s" % rid)  # stale dialog on the DS
            return
        if choice in ANSWERS:
            req.answer = choice
            req.event.set()

    def set_remote(self, on, source="PC"):
        with self.lock:
            if self.remote != on:
                self.remote = on
                log("원격 승인 모드 %s (%s)" % ("ON" if on else "OFF", source))
                self._note("REMOTE %s" % ("ON" if on else "OFF"))
            self._send(self._status_line())

    # -- device ------------------------------------------------------------

    def device_line(self, line):
        parts = line.strip().split()
        if not parts:
            return
        cmd = parts[0].upper()
        now = time.time()
        with self.lock:
            if cmd == "HELLO":
                was_online = self.device_online
                self.device_online = True
                self.last_device_rx = now
                if not was_online:
                    log("DS 연결됨")
                self._full_sync()
                return
            if not self.device_online:
                # The DS is talking but we never saw its HELLO (e.g. the bridge
                # was restarted while the DS stayed connected): ask again.
                if cmd == "PING" and self.sender is not None:
                    try:
                        self.sender("WHO")
                    except Exception:
                        pass
                return
            self.last_device_rx = now
            if cmd == "PING":
                self._send("PONG")
            elif cmd == "ANS" and len(parts) >= 3:
                self._answer(parts[1], parts[2].lower())
            elif cmd == "REM" and len(parts) >= 2:
                self.set_remote(parts[1] == "1", source="DS")

    def device_lost(self, reason=""):
        with self.lock:
            if self.device_online:
                log("DS 연결 끊김%s" % (" (%s)" % reason if reason else ""))
            self.device_online = False
            self.shown_pr = None
            self.shown_more = -1

    # -- periodic ----------------------------------------------------------

    def tick(self):
        now = time.time()
        with self.lock:
            self._ticks += 1
            if self.device_online and now - self.last_device_rx > DEVICE_TIMEOUT_S:
                self.device_lost("응답 없음")
            for sid, sess in list(self.sessions.items()):
                idle_for = now - sess.last_seen
                if sess.state in ("I", "D") and idle_for > SESSION_EXPIRE_S:
                    del self.sessions[sid]
                elif sess.state in ("W", "A") and idle_for > STALE_WORK_S and not self.pending:
                    sess.set_state("I", now)
            self.git.maybe_refresh([s.cwd for s in self.sessions.values()])
            self._send(self._status_line())
            if self._ticks % 2 == 0:
                self._push_sessions()

    def snapshot(self):
        with self.lock:
            return {
                "version": VERSION,
                "device_online": self.device_online,
                "remote": self.remote,
                "sessions": [
                    {"name": s.name, "state": s.state, "cwd": s.cwd, "ctx": s.ctx}
                    for s in self.sessions.values()
                ],
                "pending": [
                    {"id": r.rid, "session": r.session, "tool": r.tool, "detail": r.detail}
                    for r in self.pending.values()
                ],
                "history": list(self.history),
                "status_line": self._status_line(),
            }


# ---------------------------------------------------------------------------
# Device links
# ---------------------------------------------------------------------------


def find_esp32_port():
    if list_ports is None:
        return None
    for info in list_ports.comports():
        if info.vid in KNOWN_USB_VIDS:
            return info.device
    return None


class SerialLink(threading.Thread):
    """ESP32 mode: the ESP32 relays DS lines over USB serial."""

    def __init__(self, state, port=None, baud=SERIAL_BAUD, dtr=False):
        super().__init__(daemon=True, name="serial-link")
        self.state = state
        self.port = port
        self.baud = baud
        self.dtr = dtr
        self.stop_event = threading.Event()
        self._ser = None
        self._wlock = threading.Lock()
        state.sender = self.send

    def send(self, line):
        with self._wlock:
            ser = self._ser
            if ser is None:
                return
            ser.write((line + "\n").encode("ascii", "replace"))

    def send_esp(self, line):
        """Sends a '#' command to the ESP32 itself."""
        self.send(line)

    def _open(self, port):
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = self.baud
        ser.timeout = 0.2
        ser.write_timeout = 2
        # Don't toggle the auto-reset lines of ESP32 dev boards.
        ser.dtr = self.dtr
        ser.rts = False
        ser.open()
        return ser

    def run(self):
        announced = False
        while not self.stop_event.is_set():
            port = self.port or find_esp32_port()
            if not port:
                if not announced:
                    log("ESP32를 찾는 중... USB로 PC에 꽂혀 있는지 확인하세요 "
                        "(--list-ports 로 목록 확인)")
                    announced = True
                self.stop_event.wait(2)
                continue
            try:
                ser = self._open(port)
            except Exception as exc:
                if not announced:
                    log("ESP32 포트(%s)를 열 수 없음: %s" % (port, exc))
                    announced = True
                self.stop_event.wait(2)
                continue
            announced = False
            log("ESP32 연결됨: %s" % port)
            with self._wlock:
                self._ser = ser
            try:
                self.send("#INFO")
                self._read_loop(ser)
            except Exception as exc:
                log("ESP32 연결 끊김: %s" % exc)
            finally:
                with self._wlock:
                    self._ser = None
                try:
                    ser.close()
                except Exception:
                    pass
                self.state.device_lost("ESP32 분리됨")
            self.stop_event.wait(1)

    def _read_loop(self, ser):
        buf = b""
        while not self.stop_event.is_set():
            data = ser.read(256)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._on_line(raw.decode("ascii", "replace").strip())
            if len(buf) > 4096:
                buf = b""

    def _on_line(self, line):
        if not line:
            return
        if line.startswith("#"):
            self._esp_line(line)
        else:
            self.state.device_line(line)

    def _esp_line(self, line):
        parts = line.split()
        tag = parts[0].upper()
        arg = parts[1].upper() if len(parts) > 1 else ""
        if tag == "#ESP" and arg == "READY":
            ssid = parts[2] if len(parts) > 2 else "?"
            log("ESP32 준비됨 - 와이파이 이름: %s" % ssid)
        elif tag == "#DS" and arg == "UP":
            log("DS가 ESP32 와이파이에 접속함 (%s)" % (parts[2] if len(parts) > 2 else "?"))
        elif tag == "#DS" and arg == "DOWN":
            self.state.device_lost("DS 접속 종료")
        elif tag == "#STA" and arg == "BLOCKED":
            log("등록되지 않은 기기의 접속을 막음: %s" % (parts[2] if len(parts) > 2 else "?"))
        elif tag == "#PAIRED":
            log("이 DS를 ESP32에 등록함: %s" % arg)
        elif tag == "#UNPAIRED":
            log("ESP32의 DS 등록을 해제함 (다음에 접속하는 DS가 등록됩니다)")


class TcpLink(threading.Thread):
    """Router mode: the DS connects straight to this PC over TCP."""

    def __init__(self, state, host, port, token):
        super().__init__(daemon=True, name="tcp-link")
        self.state = state
        self.host = host
        self.port = port
        self.token = token or ""
        self.stop_event = threading.Event()
        self._conn = None
        self._wlock = threading.Lock()
        self.bound = threading.Event()
        self.sock = None
        state.sender = self.send

    def send(self, line):
        with self._wlock:
            conn = self._conn
            if conn is None:
                return
            conn.sendall((line + "\n").encode("ascii", "replace"))

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.host, self.port))
        srv.listen(4)
        srv.settimeout(0.5)
        self.sock = srv
        self.port = srv.getsockname()[1]
        self.bound.set()
        log("공유기 모드: DS 접속 대기 중 %s:%d" % (self.host, self.port))
        while not self.stop_event.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(conn, addr), daemon=True).start()
        srv.close()

    def _serve(self, conn, addr):
        conn.settimeout(5)
        buf = b""
        try:
            while b"\n" not in buf and len(buf) < 512:
                chunk = conn.recv(256)
                if not chunk:
                    conn.close()
                    return
                buf += chunk
        except OSError:
            conn.close()
            return
        first, buf = buf.split(b"\n", 1) if b"\n" in buf else (buf, b"")
        hello = first.decode("ascii", "replace").strip().split()
        token = hello[3] if len(hello) > 3 else ""
        if len(hello) < 2 or hello[0].upper() != "HELLO" or (self.token and token != self.token):
            log("DS 인증 실패 (%s) - claude-ds.ini 의 token 값을 확인하세요" % addr[0])
            try:
                conn.sendall(b"ERR auth\n")
            except OSError:
                pass
            conn.close()
            return
        with self._wlock:
            old, self._conn = self._conn, conn
        if old is not None:
            try:
                old.close()
            except OSError:
                pass
        log("DS가 공유기 모드로 접속함 (%s)" % addr[0])
        conn.settimeout(0.5)
        self.state.device_line(first.decode("ascii", "replace"))
        try:
            while not self.stop_event.is_set():
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    self.state.device_line(raw.decode("ascii", "replace"))
                try:
                    chunk = conn.recv(512)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                if len(buf) > 8192:
                    buf = b""
        except OSError:
            pass
        finally:
            with self._wlock:
                current = self._conn is conn
                if current:
                    self._conn = None
            try:
                conn.close()
            except OSError:
                pass
            if current:
                self.state.device_lost("TCP 연결 종료")


# ---------------------------------------------------------------------------
# HTTP endpoint for the hook script
# ---------------------------------------------------------------------------


def make_http_server(state, host="127.0.0.1", port=DEFAULT_HTTP_PORT):
    class Handler(BaseHTTPRequestHandler):
        server_version = "ClaudeDS/" + VERSION
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *args):  # keep the console quiet
            pass

        def _reply(self, code, obj):
            body = json.dumps(obj).encode("utf-8")
            try:
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except OSError:
                pass  # the hook gave up waiting

        def _client_alive(self):
            try:
                readable, _, _ = select.select([self.connection], [], [], 0)
                if readable:
                    return bool(self.connection.recv(1, socket.MSG_PEEK))
                return True
            except (OSError, ValueError):
                return False

        def do_GET(self):
            if self.path.startswith("/status"):
                self._reply(200, state.snapshot())
            elif self.path.startswith("/ping"):
                self._reply(200, {"ok": True, "version": VERSION})
            else:
                self._reply(404, {"error": "not found"})

        def do_POST(self):
            try:
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length) if length > 0 else b""
                payload = json.loads(raw.decode("utf-8")) if raw else {}
            except (ValueError, OSError):
                self._reply(400, {"error": "bad json"})
                return
            if self.path.startswith("/hook"):
                state.handle_hook(payload)
                self._reply(200, {"ok": True})
            elif self.path.startswith("/statusline"):
                self._reply(200, state.handle_statusline(payload))
            elif self.path.startswith("/permission"):
                self._reply(200, state.request_permission(payload, self._client_alive))
            else:
                self._reply(404, {"error": "not found"})

    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    return server


# ---------------------------------------------------------------------------
# Claude Code settings.json (hooks + statusline)
# ---------------------------------------------------------------------------

HOOK_MARKER = "claude_ds_hook.py"

# (event, matcher, async, timeout seconds)
HOOK_SPECS = [
    ("SessionStart", None, True, 10),
    ("UserPromptSubmit", None, True, 10),
    ("PostToolUse", "*", True, 10),
    ("Notification", None, True, 10),
    ("Stop", None, True, 10),
    ("SessionEnd", None, False, 5),
    ("PermissionRequest", "*", False, 600),
]


def default_settings_path():
    base = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join(os.path.expanduser("~"), ".claude")
    return os.path.join(base, "settings.json")


def _windows_has_bash():
    if os.environ.get("CLAUDE_CODE_GIT_BASH_PATH"):
        return True
    for cand in (shutil.which("bash"), r"C:\Program Files\Git\bin\bash.exe",
                 r"C:\Program Files (x86)\Git\bin\bash.exe"):
        if cand and os.path.exists(cand):
            return True
    return False


def hook_command(python, script, extra=(), windows=None, has_bash=None):
    """Command fields for a hook: shell form when a POSIX shell runs hooks."""
    windows = (os.name == "nt") if windows is None else windows
    if windows and not (_windows_has_bash() if has_bash is None else has_bash):
        return {"command": python, "args": [script] + list(extra)}  # exec form

    def quote(path):
        return '"' + path.replace("\\", "/").replace('"', '\\"') + '"'

    return {"command": " ".join([quote(python), quote(script)] + list(extra))}


def _is_ours(entry):
    if not isinstance(entry, dict):
        return False
    if HOOK_MARKER in str(entry.get("command", "")):
        return True
    return any(HOOK_MARKER in str(a) for a in (entry.get("args") or []))


def _strip_ours(hooks):
    for event in list(hooks.keys()):
        groups = hooks.get(event)
        if not isinstance(groups, list):
            continue
        kept = []
        for group in groups:
            if not isinstance(group, dict):
                kept.append(group)
                continue
            inner = [h for h in (group.get("hooks") or []) if not _is_ours(h)]
            if inner:
                group = dict(group)
                group["hooks"] = inner
                kept.append(group)
            elif not group.get("hooks"):
                kept.append(group)  # not ours, leave untouched
        if kept:
            hooks[event] = kept
        else:
            del hooks[event]


def _load_settings(path):
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    return json.loads(text) if text.strip() else {}


def _save_settings(path, data):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    if os.path.exists(path):
        backup = path + time.strftime(".bak-%Y%m%d-%H%M%S")
        shutil.copy2(path, backup)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


def install_hooks(settings_path, python=None, script=None, statusline=True,
                  windows=None, has_bash=None):
    python = python or sys.executable
    script = script or os.path.abspath(os.path.join(os.path.dirname(__file__), HOOK_MARKER))
    data = _load_settings(settings_path)
    hooks = data.get("hooks")
    if not isinstance(hooks, dict):
        hooks = {}
    _strip_ours(hooks)
    for event, matcher, is_async, timeout in HOOK_SPECS:
        entry = {"type": "command"}
        entry.update(hook_command(python, script, windows=windows, has_bash=has_bash))
        entry["timeout"] = timeout
        if is_async:
            entry["async"] = True
        group = {"hooks": [entry]}
        if matcher is not None:
            group = {"matcher": matcher, "hooks": [entry]}
        hooks.setdefault(event, []).append(group)
    data["hooks"] = hooks

    note = "statusline: skipped"
    if statusline:
        existing = data.get("statusLine")
        cmd = hook_command(python, script, ["--statusline"], windows=windows, has_bash=has_bash)
        if existing and not _is_ours(existing):
            note = "statusline: kept your existing statusLine (CTX/5H/WEEK stay empty)"
        elif "args" in cmd:
            note = "statusline: skipped (Git Bash not found on Windows)"
        else:
            data["statusLine"] = {"type": "command", "command": cmd["command"], "padding": 0}
            note = "statusline: installed"
    _save_settings(settings_path, data)
    return note


def uninstall_hooks(settings_path):
    data = _load_settings(settings_path)
    hooks = data.get("hooks")
    if isinstance(hooks, dict):
        _strip_ours(hooks)
        if hooks:
            data["hooks"] = hooks
        else:
            data.pop("hooks", None)
    if _is_ours(data.get("statusLine")):
        data.pop("statusLine", None)
    _save_settings(settings_path, data)


# ---------------------------------------------------------------------------
# Demo mode
# ---------------------------------------------------------------------------


class DemoDriver(threading.Thread):
    """Fake Claude Code activity so the DS can be tested on its own."""

    def __init__(self, state):
        super().__init__(daemon=True, name="demo")
        self.state = state
        self.stop_event = threading.Event()

    def _hook(self, name, event, **extra):
        payload = {"session_id": "demo-" + name, "hook_event_name": event,
                   "cwd": os.path.join(os.path.expanduser("~"), "projects", name)}
        payload.update(extra)
        self.state.handle_hook(payload)

    def _sleep(self, seconds):
        return self.stop_event.wait(seconds)

    def run(self):
        st = self.state
        for name in ("setouchi", "blog", "api-server"):
            self._hook(name, "SessionStart", source="startup")
        st.handle_statusline({
            "session_id": "demo-setouchi",
            "cwd": os.path.join(os.path.expanduser("~"), "projects", "setouchi"),
            "context_window": {"used_percentage": 42},
            "rate_limits": {"five_hour": {"used_percentage": 13},
                            "seven_day": {"used_percentage": 52}},
        })
        log("데모 모드: 가짜 세션 3개로 DS 화면을 시험합니다")
        while not self.stop_event.is_set():
            self._hook("setouchi", "UserPromptSubmit")
            if self._sleep(6):
                return
            self._hook("setouchi", "PostToolUse", tool_name="Bash")
            if self._sleep(6):
                return
            self._hook("setouchi", "Stop")
            if self._sleep(4):
                return
            self._hook("blog", "UserPromptSubmit")
            if self._sleep(4):
                return
            payload = {"session_id": "demo-blog", "hook_event_name": "PermissionRequest",
                       "cwd": os.path.join(os.path.expanduser("~"), "projects", "blog"),
                       "tool_name": "Bash",
                       "tool_input": {"command": "rm -rf dist && npm run build"}}
            if st.remote and st.device_online:
                result = st.request_permission(payload, lambda: not self.stop_event.is_set())
                log("데모 권한 요청 결과: %s" % (result.get("decision") or "PC로 넘김"))
            else:
                st.request_permission(payload)
                self._hook("blog", "Notification", notification_type="permission_prompt",
                           message="Claude needs your permission to use Bash")
                if self._sleep(8):
                    return
            self._hook("blog", "PostToolUse", tool_name="Bash")
            if self._sleep(4):
                return
            self._hook("blog", "Stop")
            if self._sleep(8):
                return


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _ticker(state, stop_event):
    while not stop_event.wait(1.0):
        try:
            state.tick()
        except Exception as exc:  # never let the ticker die
            log("tick error: %s" % exc)


def _unpair(port):
    if serial is None:
        print("pyserial이 필요합니다: pip install pyserial")
        return 1
    port = port or find_esp32_port()
    if not port:
        print("ESP32를 찾지 못했습니다.")
        return 1
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, SERIAL_BAUD, 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.write(b"#UNPAIR\n")
    deadline = time.time() + 3
    buf = b""
    while time.time() < deadline:
        buf += ser.read(256)
        if b"#UNPAIRED" in buf:
            print("완료: ESP32의 DS 등록을 해제했습니다. 다음에 접속하는 DS가 새로 등록됩니다.")
            ser.close()
            return 0
    ser.close()
    print("ESP32에서 응답이 없습니다. 브리지가 실행 중이면 끄고 다시 시도하세요.")
    return 1


def main(argv=None):
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

    ap = argparse.ArgumentParser(
        description="Claude DS bridge - Claude Code status on a Nintendo DS")
    ap.add_argument("--port", help="ESP32 serial port (e.g. COM5 or /dev/cu.usbserial-0001); "
                                   "auto-detected by default")
    ap.add_argument("--tcp", metavar="HOST:PORT",
                    help="router mode: accept the DS over TCP instead of using an ESP32 "
                         "(e.g. 0.0.0.0:7777)")
    ap.add_argument("--token", help="shared secret for --tcp mode (token= in claude-ds.ini)")
    ap.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT,
                    help="local port the hook script talks to (default 8787)")
    ap.add_argument("--remote", action="store_true",
                    help="start with remote approval ON (normally toggled with SELECT on the DS)")
    ap.add_argument("--demo", action="store_true",
                    help="generate fake Claude activity to test the DS")
    ap.add_argument("--install-hooks", action="store_true",
                    help="add the Claude DS hooks to Claude Code's settings.json and exit")
    ap.add_argument("--uninstall-hooks", action="store_true",
                    help="remove the Claude DS hooks from settings.json and exit")
    ap.add_argument("--no-statusline", action="store_true",
                    help="with --install-hooks: don't install the statusline (CTX/5H/WEEK)")
    ap.add_argument("--settings", help="path of Claude Code's settings.json "
                                       "(default: ~/.claude/settings.json)")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    ap.add_argument("--unpair", action="store_true",
                    help="make the ESP32 forget the paired DS and exit")
    ap.add_argument("--dtr", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    if args.install_hooks or args.uninstall_hooks:
        path = args.settings or default_settings_path()
        if args.uninstall_hooks:
            uninstall_hooks(path)
            print("Claude DS 훅을 제거했습니다: %s" % path)
        else:
            note = install_hooks(path, statusline=not args.no_statusline)
            print("Claude DS 훅을 설치했습니다: %s (%s)" % (path, note))
            print("이미 열려 있는 Claude Code 세션은 다시 시작해야 적용됩니다.")
        return 0

    if args.list_ports:
        if list_ports is None:
            print("pyserial이 필요합니다: pip install pyserial")
            return 1
        for info in list_ports.comports():
            mark = "  <- ESP32?" if info.vid in KNOWN_USB_VIDS else ""
            print("%s  %s%s" % (info.device, info.description, mark))
        return 0

    if args.unpair:
        return _unpair(args.port)

    state = BridgeState(remote=args.remote)

    if args.tcp:
        host, _, port = args.tcp.rpartition(":")
        link = TcpLink(state, host or "0.0.0.0", int(port or DEFAULT_TCP_PORT), args.token)
        if not args.token:
            log("경고: 공유기 모드에서 --token 없이 실행 중입니다. 같은 와이파이의 누구나 접속할 수 있어요.")
    else:
        if serial is None:
            print("ESP32 모드에는 pyserial이 필요합니다: pip install pyserial")
            return 1
        link = SerialLink(state, port=args.port, dtr=args.dtr)

    try:
        server = make_http_server(state, port=args.http_port)
    except OSError as exc:
        print("포트 %d를 열 수 없습니다 (%s). 브리지가 이미 실행 중인가요?" % (args.http_port, exc))
        return 1

    log("Claude DS 브리지 %s 시작 (훅 수신: http://127.0.0.1:%d)" % (VERSION, args.http_port))
    if psutil is None:
        log("참고: psutil이 없어 CPU/MEM 값은 표시되지 않습니다 (pip install psutil)")
    log("원격 승인 모드: %s (DS에서 SELECT 버튼으로 켜고 끔)" % ("ON" if state.remote else "OFF"))

    stop_event = threading.Event()
    link.start()
    threading.Thread(target=server.serve_forever, daemon=True, name="http").start()
    threading.Thread(target=_ticker, args=(state, stop_event), daemon=True, name="tick").start()
    demo = None
    if args.demo:
        demo = DemoDriver(state)
        demo.start()
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        log("종료합니다")
    finally:
        stop_event.set()
        link.stop_event.set()
        if demo is not None:
            demo.stop_event.set()
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
