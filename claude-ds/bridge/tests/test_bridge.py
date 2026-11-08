"""Tests for the Claude DS bridge and hook script.

Run from the bridge directory:  python -m unittest discover -s tests -v
Only the standard library is needed (the serial test is skipped when
pyserial is missing or the OS has no pty support).
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE_DIR = os.path.dirname(HERE)
sys.path.insert(0, BRIDGE_DIR)

import claude_ds_bridge as b  # noqa: E402
import claude_ds_hook as hook  # noqa: E402

HOOK_SCRIPT = os.path.join(BRIDGE_DIR, "claude_ds_hook.py")


class FakeStats:
    def snapshot(self):
        return 30, 36, 653


class FakeGit:
    value = 4

    def maybe_refresh(self, cwds, force=False):
        pass


def load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def make_state(**kw):
    return b.BridgeState(stats=FakeStats(), git=FakeGit(), **kw)


class FakeDS:
    """Speaks the DS side of the protocol over TCP."""

    def __init__(self, port, token=""):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.seen = []
        self.send("HELLO claude-ds 1 %s" % token)

    def send(self, line):
        self.sock.sendall((line + "\n").encode("ascii"))

    def wait_for(self, prefix, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = raw.decode("ascii")
                self.seen.append(line)
                if line.startswith(prefix):
                    return line
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            self.buf += chunk
        raise AssertionError("no line starting with %r; saw %r" % (prefix, self.seen[-15:]))

    def wait_for_all(self, prefix, needles, timeout=5.0):
        """Waits for a line with `prefix` that contains every string in `needles`."""
        deadline = time.time() + timeout
        while True:
            line = self.wait_for(prefix, max(0.05, deadline - time.time()))
            if all(n in line for n in needles):
                return line

    def close(self):
        self.sock.close()


def run_hook(http_port, payload, args=(), timeout=10):
    env = dict(os.environ, CLAUDE_DS_BRIDGE="http://127.0.0.1:%d" % http_port)
    return subprocess.run([sys.executable, HOOK_SCRIPT] + list(args),
                          input=json.dumps(payload).encode("utf-8"),
                          capture_output=True, env=env, timeout=timeout)


def start_hook(http_port, payload):
    env = dict(os.environ, CLAUDE_DS_BRIDGE="http://127.0.0.1:%d" % http_port)
    proc = subprocess.Popen([sys.executable, HOOK_SCRIPT], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    proc.stdin.write(json.dumps(payload).encode("utf-8"))
    proc.stdin.close()
    # communicate() would flush the closed pipe on Python <= 3.12 and fail.
    proc.stdin = None
    return proc


class TextHelpers(unittest.TestCase):
    def test_ascii_name(self):
        self.assertEqual(b.ascii_name("my project!"), "my-project")
        self.assertEqual(b.ascii_name("a" * 40), "a" * b.NAME_MAX)
        self.assertEqual(b.ascii_name("세토우치"), "")

    def test_ascii_text_collapses_non_ascii(self):
        self.assertEqual(b.ascii_text("echo 안녕하세요 world"), "echo ? world")
        self.assertTrue(b.ascii_text("x" * 500).endswith("..."))
        self.assertLessEqual(len(b.ascii_text("x" * 500)), b.DETAIL_MAX)

    def test_session_name_fallback(self):
        self.assertEqual(b.session_display_name("/home/u/setouchi", "abc123"), "setouchi")
        self.assertEqual(b.session_display_name("C:\\Users\\u\\미술여행", "abc123ef"), "s-abc123")

    def test_short_tool(self):
        self.assertEqual(b.short_tool("Bash"), "Bash")
        self.assertEqual(b.short_tool("mcp__github__create_issue"), "mcp:github/create_is")
        self.assertEqual(len(b.short_tool("mcp__github__create_issue")), b.TOOL_MAX)

    def test_summarize(self):
        self.assertEqual(b.summarize_tool("Bash", {"command": "npm test"}), "npm test")
        self.assertEqual(b.summarize_tool("Edit", {"file_path": "/a/b/c/d/e.py"}), "/a/b/c/d/e.py")
        long_path = "/home/someone/projects/website/src/components/Header.tsx"
        self.assertEqual(b.summarize_tool("Edit", {"file_path": long_path}),
                         ".../src/components/Header.tsx")
        self.assertEqual(b.summarize_tool("WebFetch", {"url": "https://x.y"}), "https://x.y")
        self.assertIn("q", b.summarize_tool("mcp__x__y", {"q": 1}))


class HookOutput(unittest.TestCase):
    def test_permission_output_shapes(self):
        allow = hook.permission_output("allow")["hookSpecificOutput"]
        self.assertEqual(allow["hookEventName"], "PermissionRequest")
        self.assertEqual(allow["decision"], {"behavior": "allow"})
        deny = hook.permission_output("deny")["hookSpecificOutput"]["decision"]
        self.assertEqual(deny["behavior"], "deny")
        self.assertNotIn("interrupt", deny)
        stop = hook.permission_output("stop")["hookSpecificOutput"]["decision"]
        self.assertEqual(stop["behavior"], "deny")
        self.assertIs(stop["interrupt"], True)
        self.assertIsNone(hook.permission_output(None))
        self.assertIsNone(hook.permission_output("pc"))

    def test_statusline_text(self):
        payload = {"model": {"display_name": "Opus"},
                   "context_window": {"used_percentage": 41.6},
                   "rate_limits": {"five_hour": {"used_percentage": 13},
                                   "seven_day": {"used_percentage": 52.2}}}
        self.assertEqual(hook.statusline_text(payload, {"device": True, "remote": True}),
                         "DS:on+R | Opus | ctx 42% | 5h 13% | 7d 52%")
        self.assertEqual(hook.statusline_text({}, None), "DS:off")

    def test_hook_without_bridge_is_silent(self):
        # Nothing listens on this port: the hook must exit 0 with no output.
        res = run_hook(1, {"hook_event_name": "PermissionRequest", "session_id": "x",
                           "tool_name": "Bash", "tool_input": {"command": "ls"}})
        self.assertEqual(res.returncode, 0)
        self.assertEqual(res.stdout, b"")
        res = run_hook(1, {"model": {"display_name": "M"}}, args=["--statusline"])
        self.assertEqual(res.stdout.decode().strip(), "DS:off | M")


class SettingsInstall(unittest.TestCase):
    def test_install_and_uninstall_keep_user_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "settings.json")
            original = {
                "model": "opus",
                "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "say done"}]}]},
            }
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(original, fh)

            note = b.install_hooks(path, python="/usr/bin/python3",
                                   script="/opt/claude-ds/claude_ds_hook.py",
                                   windows=False)
            self.assertEqual(note, "statusline: installed")
            data = load_json(path)
            self.assertEqual(data["model"], "opus")
            stop_cmds = [h["command"] for g in data["hooks"]["Stop"] for h in g["hooks"]]
            self.assertIn("say done", stop_cmds)
            self.assertIn('"/usr/bin/python3" "/opt/claude-ds/claude_ds_hook.py"', stop_cmds)
            perm = data["hooks"]["PermissionRequest"][0]
            self.assertEqual(perm["matcher"], "*")
            self.assertEqual(perm["hooks"][0]["timeout"], 600)
            self.assertNotIn("async", perm["hooks"][0])
            self.assertTrue(data["hooks"]["PostToolUse"][0]["hooks"][0]["async"])
            self.assertIn("--statusline", data["statusLine"]["command"])

            # Installing twice must not duplicate entries.
            b.install_hooks(path, python="/usr/bin/python3",
                            script="/opt/claude-ds/claude_ds_hook.py", windows=False)
            data = load_json(path)
            self.assertEqual(len(data["hooks"]["PermissionRequest"]), 1)
            self.assertEqual(len(data["hooks"]["Stop"]), 2)

            b.uninstall_hooks(path)
            data = load_json(path)
            self.assertEqual(data, original)
            backups = [n for n in os.listdir(tmp) if ".bak-" in n]
            self.assertTrue(backups)

    def test_existing_statusline_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "settings.json")
            with open(path, "w") as fh:
                json.dump({"statusLine": {"type": "command", "command": "mine.sh"}}, fh)
            note = b.install_hooks(path, python="p", script="/x/claude_ds_hook.py", windows=False)
            self.assertIn("kept", note)
            self.assertEqual(load_json(path)["statusLine"]["command"], "mine.sh")

    def test_windows_without_bash_uses_exec_form(self):
        cmd = b.hook_command("C:\\Py\\python.exe", "C:\\cds\\claude_ds_hook.py",
                             windows=True, has_bash=False)
        self.assertEqual(cmd, {"command": "C:\\Py\\python.exe",
                               "args": ["C:\\cds\\claude_ds_hook.py"]})
        cmd = b.hook_command("C:\\Py\\python.exe", "C:\\cds\\claude_ds_hook.py",
                             windows=True, has_bash=True)
        self.assertEqual(cmd["command"], '"C:/Py/python.exe" "C:/cds/claude_ds_hook.py"')


class Integration(unittest.TestCase):
    """Bridge in TCP (router) mode + real hook script subprocesses."""

    def setUp(self):
        self.state = make_state(permission_timeout=20)
        self.link = b.TcpLink(self.state, "127.0.0.1", 0, "s3cret")
        self.link.start()
        self.assertTrue(self.link.bound.wait(5))
        self.server = b.make_http_server(self.state, port=0)
        self.http_port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.ds = None

    def tearDown(self):
        if self.ds:
            self.ds.close()
        self.server.shutdown()
        self.server.server_close()
        self.link.stop_event.set()
        self.link.join(timeout=3)

    def connect(self, token="s3cret"):
        self.ds = FakeDS(self.link.port, token)
        self.ds.wait_for("HI 1")
        self.ds.wait_for("ST ")
        return self.ds

    def hook(self, event, sid="sess-1", **extra):
        payload = {"session_id": sid, "hook_event_name": event, "cwd": "/home/u/setouchi"}
        payload.update(extra)
        res = run_hook(self.http_port, payload)
        self.assertEqual(res.returncode, 0, res.stderr)
        return res

    def test_wrong_token_is_rejected(self):
        sock = socket.create_connection(("127.0.0.1", self.link.port), timeout=5)
        sock.sendall(b"HELLO claude-ds 1 nope\n")
        self.assertEqual(sock.recv(64), b"ERR auth\n")
        sock.close()
        self.assertFalse(self.state.device_online)

    def test_status_and_events_flow(self):
        ds = self.connect()
        self.hook("SessionStart", source="startup")
        line = ds.wait_for("SI 0 ")
        self.assertTrue(line.startswith("SI 0 I "), line)
        self.assertTrue(line.endswith(" setouchi"), line)

        self.hook("UserPromptSubmit", prompt="hi")
        ds.wait_for("SI 0 W ")
        st = ds.wait_for("ST ")
        self.assertIn("ses=1", st)
        self.assertIn("cpu=30", st)
        self.assertIn("unp=4", st)

        self.hook("Stop")
        self.assertEqual(ds.wait_for("EV DONE"), "EV DONE setouchi")
        ds.wait_for("SI 0 D ")

        # A late async PostToolUse must not flip "done" back to "working".
        self.hook("PostToolUse", tool_name="Bash")
        time.sleep(0.2)
        self.assertEqual(self.state.sessions["sess-1"].state, "D")

        self.hook("Notification", notification_type="permission_prompt",
                  message="Claude needs your permission to use Bash")
        self.assertEqual(ds.wait_for("EV WAIT"), "EV WAIT setouchi permission")
        ds.wait_for("SI 0 A ")

        res = run_hook(self.http_port, {"session_id": "sess-1", "cwd": "/home/u/setouchi",
                                        "context_window": {"used_percentage": 49},
                                        "rate_limits": {"seven_day": {"used_percentage": 52}}},
                       args=["--statusline"])
        self.assertEqual(res.stdout.decode().strip(), "DS:on | ctx 49% | 7d 52%")
        ds.wait_for_all("ST ", ["ctx=49", "wk=52", "h5=-1"])

        self.hook("SessionEnd", reason="other")
        ds.wait_for("SC 0 0")

    def test_permission_remote_off_returns_immediately(self):
        self.connect()
        start = time.time()
        res = self.hook("PermissionRequest", tool_name="Bash", tool_input={"command": "ls"})
        self.assertLess(time.time() - start, 5)
        self.assertEqual(res.stdout, b"")

    def _ask(self, ds, command):
        proc = start_hook(self.http_port, {
            "session_id": "sess-1", "hook_event_name": "PermissionRequest",
            "cwd": "/home/u/setouchi", "tool_name": "Bash",
            "tool_input": {"command": command}})
        line = ds.wait_for("PR ")
        parts = line.split(" ", 5)
        self.assertEqual(parts[2:5], ["0", "Bash", "setouchi"])
        self.assertEqual(parts[5], command)
        return proc, parts[1]

    def test_permission_answers_from_ds(self):
        ds = self.connect()
        ds.send("REM 1")
        ds.wait_for("ST ")
        deadline = time.time() + 5
        while not self.state.remote and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(self.state.remote)

        expected = {
            "allow": {"behavior": "allow"},
            "deny": {"behavior": "deny",
                     "message": "The user denied this from the Nintendo DS remote."},
            "stop": {"behavior": "deny",
                     "message": "The user pressed STOP on the Nintendo DS remote.",
                     "interrupt": True},
        }
        for answer, decision in expected.items():
            proc, rid = self._ask(ds, "rm -rf dist && npm run build")
            ds.send("ANS %s %s" % (rid, answer))
            out, _ = proc.communicate(timeout=10)
            self.assertEqual(json.loads(out.decode()), {
                "hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                       "decision": decision}})

        # "PC" hands the decision back to the normal dialog: no output.
        proc, rid = self._ask(ds, "git push")
        ds.send("ANS %s pc" % rid)
        out, _ = proc.communicate(timeout=10)
        self.assertEqual(out, b"")
        ds.wait_for("PX %s" % rid)

    def test_pending_request_released_when_ds_disconnects(self):
        ds = self.connect()
        ds.send("REM 1")
        deadline = time.time() + 5
        while not self.state.remote and time.time() < deadline:
            time.sleep(0.05)
        proc, _ = self._ask(ds, "make deploy")
        ds.close()
        self.ds = None
        out, _ = proc.communicate(timeout=10)
        self.assertEqual(out, b"")

    def test_two_requests_are_queued(self):
        ds = self.connect()
        ds.send("REM 1")
        deadline = time.time() + 5
        while not self.state.remote and time.time() < deadline:
            time.sleep(0.05)
        first, rid1 = self._ask(ds, "echo one")
        second = start_hook(self.http_port, {
            "session_id": "sess-2", "hook_event_name": "PermissionRequest",
            "cwd": "/home/u/blog", "tool_name": "Write",
            "tool_input": {"file_path": "/home/u/blog/post.md"}})
        line = ds.wait_for("PR %s 1 " % rid1)  # same request, now "+1 more"
        self.assertIn("echo one", line)
        ds.send("ANS %s allow" % rid1)
        line = ds.wait_for("PR ")
        parts = line.split(" ", 5)
        self.assertEqual(parts[2:6], ["0", "Write", "blog", "/home/u/blog/post.md"])
        ds.send("ANS %s deny" % parts[1])
        self.assertIn(b'"allow"', first.communicate(timeout=10)[0])
        self.assertIn(b'"deny"', second.communicate(timeout=10)[0])

    def test_unknown_answer_clears_stale_dialog(self):
        ds = self.connect()
        ds.send("ANS abc123 allow")
        ds.wait_for("PX abc123")


@unittest.skipIf(b.serial is None or os.name == "nt", "needs pyserial and a pty")
class SerialMode(unittest.TestCase):
    """Bridge in ESP32 mode; the test plays the ESP32 on a pseudo terminal."""

    def test_relay_over_serial(self):
        import pty
        master, slave = pty.openpty()
        os.set_blocking(master, False)
        state = make_state()
        link = b.SerialLink(state, port=os.ttyname(slave))
        link.start()
        received = b""

        def read_until(token, timeout=5):
            nonlocal received
            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    received += os.read(master, 4096)
                except BlockingIOError:
                    time.sleep(0.02)
                if token in received:
                    return
            raise AssertionError("missing %r in %r" % (token, received[-300:]))

        try:
            read_until(b"#INFO\n")
            os.write(master, b"\x00garbage from boot rom\r\n#ESP READY CLAUDE-DS 1\n")
            os.write(master, b"#DS UP 192.168.4.2\nHELLO claude-ds 1\n")
            read_until(b"HI 1 ")
            self.assertTrue(state.device_online)
            os.write(master, b"PING\n")
            read_until(b"PONG\n")
            os.write(master, b"#DS DOWN\n")
            deadline = time.time() + 5
            while state.device_online and time.time() < deadline:
                time.sleep(0.02)
            self.assertFalse(state.device_online)
            # After a bridge restart the DS keeps pinging; the bridge asks WHO.
            os.write(master, b"PING\n")
            read_until(b"WHO\n")
        finally:
            link.stop_event.set()
            link.join(timeout=3)
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
