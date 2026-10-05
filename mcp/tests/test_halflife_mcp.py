# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "mcp>=2.2,<3",
#     "pillow>=10",
#     "pywin32>=311; sys_platform == 'win32'",
# ]
# ///
"""Tests for halflife_mcp.py. Run with:

    uv run --script mcp/tests/test_halflife_mcp.py

Set HALFLIFE_E2E=1 to also drive the real game end-to-end (needs the plugin
built and installed). Never attaches to or launches a real game otherwise.
"""

import os
import socket
import struct
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import anyio

import game_process
import rcon_client
import halflife_mcp
from game_process import BANNER_RE, GameProcess
from halflifecli import manager as manager_module
from halflifecli.manager import DEFAULT_COMMAND_MAX_LINES, Manager, truncate_output
from halflifecli.plugin_config import read_plugin_config
from halflifecli.screenshot import find_new_screenshot, shot_to_png
from halflifecli.usermsg import (
    load_usermsg_schema,
    parse_usermsg_events,
    parse_usermsg_status,
    sync_usermsg_schemas,
)
from mcp.server.mcpserver.exceptions import ToolError

os.environ["HALFLIFE_DISABLE_ATTACH"] = "1"


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------

class TestReadPluginConfig(unittest.TestCase):
    def test_sections_and_values(self):
        with tempfile.TemporaryDirectory() as d:
            cfg_dir = os.path.join(d, "svencoop", "metahook", "configs", "halflifecli")
            os.makedirs(cfg_dir)
            with open(os.path.join(cfg_dir, "halflifecli.toml"), "wb") as f:
                f.write(b'[rcon]\npassword = "abc"\nbind = "127.0.0.1"\n\n[cli]\nhide_window = 1\n')
            cfg = read_plugin_config(d)
            self.assertEqual(cfg["rcon"]["password"], "abc")
            self.assertEqual(cfg["rcon"]["bind"], "127.0.0.1")
            self.assertEqual(cfg["cli"]["hide_window"], 1)
            self.assertNotIn("hide_window", cfg.get("rcon", {}))

    def test_absent_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(read_plugin_config(d), {})

    def test_malformed_file(self):
        with tempfile.TemporaryDirectory() as d:
            cfg_dir = os.path.join(d, "svencoop", "metahook", "configs", "halflifecli")
            os.makedirs(cfg_dir)
            with open(os.path.join(cfg_dir, "halflifecli.toml"), "wb") as f:
                f.write(b"[rcon\npassword = ")
            self.assertEqual(read_plugin_config(d), {})


class TestRconConnectHost(unittest.TestCase):
    def test_host_mapping(self):
        from game_process import rcon_connect_host
        self.assertEqual(rcon_connect_host("127.0.0.1"), "127.0.0.1")
        self.assertEqual(rcon_connect_host("192.168.1.7"), "192.168.1.7")
        self.assertEqual(rcon_connect_host("0.0.0.0"), "127.0.0.1")
        self.assertEqual(rcon_connect_host(""), "127.0.0.1")
        self.assertEqual(rcon_connect_host("localhost"), "127.0.0.1")


class TestBannerRe(unittest.TestCase):
    def test_listening(self):
        m = BANNER_RE.search("halflife-cli: RCON listening on 127.0.0.1:54321 (password: none)")
        self.assertEqual((m.group(1), m.group(2)), ("127.0.0.1", "54321"))

    def test_failed(self):
        m = BANNER_RE.search("halflife-cli: RCON failed to start (bind failed (10048))")
        self.assertEqual(m.group(1), None)
        self.assertIn("bind failed", m.group(3))


class TestConsoleBuffer(unittest.TestCase):
    def _proc(self, lines, buffer_lines=8):
        return GameProcess(["dummy"], cwd=".", buffer_lines=buffer_lines)

    def test_cursor_and_drop(self):
        p = self._proc(range(0))
        with p._cond:
            for i in range(10):
                p._lines.append(str(i)); p._next_seq += 1
            sl = p.lines_since(0, 100)
        self.assertEqual(sl.lines, ["2", "3", "4", "5", "6", "7", "8", "9"])  # 8-line ring
        self.assertEqual(sl.dropped, 2)
        self.assertEqual(sl.next_cursor, 10)

    def test_latest_window(self):
        p = self._proc(range(0))
        with p._cond:
            for i in range(5):
                p._lines.append(str(i)); p._next_seq += 1
            self.assertEqual(p.tail(2), ["3", "4"])
            self.assertEqual(p.next_cursor, 5)


class TestShotToPng(unittest.TestCase):
    def test_convert_and_downscale(self):
        from PIL import Image as PILImage
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "shot.bmp")
            PILImage.new("RGB", (2560, 1440), (255, 0, 0)).save(src, format="BMP")
            png, w, h = shot_to_png(src, 1280)
            self.assertTrue(png[:8] == b"\x89PNG\r\n\x1a\n")
            self.assertEqual((w, h), (1280, 720))
            png2, w2, h2 = shot_to_png(src, 0)
            self.assertEqual((w2, h2), (2560, 1440))

    def test_tga_input(self):
        from PIL import Image as PILImage
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "osprey.tga")
            PILImage.new("RGB", (64, 32), (0, 255, 0)).save(src, format="TGA")
            png, w, h = shot_to_png(src, 1280)
            self.assertEqual((w, h), (64, 32))
            self.assertTrue(png[:8] == b"\x89PNG\r\n\x1a\n")


class TestFindNewScreenshot(unittest.TestCase):
    def test_appears(self):
        with tempfile.TemporaryDirectory() as d:
            def write():
                import time
                time.sleep(0.6)
                with open(os.path.join(d, "s.bmp"), "wb") as f:
                    f.write(b"\x42M" + b"\0" * 100)
            t = threading.Thread(target=write)
            t.start()
            path = find_new_screenshot(set(), d, timeout=5, poll=0.2)
            t.join()
            self.assertTrue(path.endswith("s.bmp"))


# ---------------------------------------------------------------------------
# UserMsg monitor helpers
# ---------------------------------------------------------------------------

class TestUserMsgParsers(unittest.TestCase):
    def test_parse_status(self):
        line = ("cli.usermsg: schema=svencoop.toml coord_size=4 messages=101 "
                "display=on hooks: 85 wrapped, 16 self-registered, 0 pending")
        st = parse_usermsg_status(line)
        self.assertEqual(st.schema_file, "svencoop.toml")
        self.assertEqual(st.coord_size, 4)
        self.assertEqual(st.messages, 101)
        self.assertTrue(st.display)
        self.assertEqual((st.wrapped, st.self_registered, st.pending), (85, 16, 0))

    def test_parse_status_garbage(self):
        self.assertIsNone(parse_usermsg_status("some other console spam"))
        self.assertIsNone(parse_usermsg_status(""))

    def test_parse_events(self):
        text = "\n".join([
            "Some unrelated engine line",
            "cli.usermsg: events channel=usermsg newest=42 name=*",
            "#40 [usermsg] CurWeapon size=5 state=1 weaponId=18 clip=35",
            "#41 [usermsg] SayText size=48 client=1 text=\"hi there\"",
            "#42 [usermsg] StartSound size=19 raw 18 00 01",
            "cli.usermsg: 7 more after #42 (raise since)",
        ])
        events, newest, more = parse_usermsg_events(text)
        self.assertEqual(newest, 42)
        self.assertEqual(more, 7)
        self.assertEqual([e.seq for e in events], [40, 41, 42])
        self.assertEqual(events[0].name, "CurWeapon")
        self.assertEqual(events[0].size, 5)
        self.assertEqual(events[0].detail, "state=1 weaponId=18 clip=35")
        self.assertEqual(events[2].detail, "raw 18 00 01")

    def test_parse_events_empty(self):
        text = "cli.usermsg: events channel=usermsg newest=0 name=*\ncli.usermsg: no matching events"
        events, newest, more = parse_usermsg_events(text)
        self.assertEqual(events, [])
        self.assertEqual(newest, 0)
        self.assertIsNone(more)

    def _write_game_dir(self, root):
        cfg = os.path.join(root, "svencoop", "metahook", "configs", "halflifecli")
        schemas = os.path.join(cfg, "usermsgs")
        os.makedirs(schemas)
        with open(os.path.join(cfg, "halflifecli.toml"), "wb") as f:
            f.write(b"[rcon]\npassword = \"x\"\n")
        with open(os.path.join(schemas, "valve.toml"), "wb") as f:
            f.write(b"[primitives]\ncoord_size = 2\n\n"
                    b"[[usermsg]]\nname = \"Health\"\nchannel = \"status\"\n"
                    b"fields = [ { name = \"health\", type = \"byte\" } ]\n\n"
                    b"[[usermsg]]\nname = \"ScoreInfo\"\nfields = []\n")
        with open(os.path.join(schemas, "svencoop.toml"), "wb") as f:
            f.write(b"extends = \"valve.toml\"\n\n[primitives]\ncoord_size = 4\n\n"
                    b"[[usermsg]]\nname = \"Health\"\n"
                    b"fields = [ { name = \"health\", type = \"long\" } ]\n\n"
                    b"[[usermsg]]\nname = \"SvenOnly\"\nnote = \"n\"\nraw = true\nfields = []\n")

    def test_load_schema_merge(self):
        with tempfile.TemporaryDirectory() as d:
            self._write_game_dir(d)
            schema = load_usermsg_schema(d)
            self.assertEqual(schema.schema_file, "svencoop.toml")
            self.assertEqual(schema.extends, "valve.toml")
            self.assertEqual(schema.coord_size, 4)
            names = [m["name"] for m in schema.messages]
            self.assertIn("ScoreInfo", names)   # inherited from valve
            self.assertIn("SvenOnly", names)
            health = next(m for m in schema.messages if m["name"] == "Health")
            self.assertEqual(health["fields"][0]["type"], "long")  # overridden
            self.assertEqual(health["channel"], "status")  # inherited from the base
            sven_only = next(m for m in schema.messages if m["name"] == "SvenOnly")
            self.assertEqual(sven_only["channel"], "usermsg")  # default channel

    def test_load_schema_missing(self):
        with tempfile.TemporaryDirectory() as d:
            from mcp.server.mcpserver.exceptions import ToolError
            with self.assertRaises(ToolError):
                load_usermsg_schema(d)

    def test_sync_schemas(self):
        with tempfile.TemporaryDirectory() as src, tempfile.TemporaryDirectory() as game:
            with open(os.path.join(src, "a.toml"), "wb") as f:
                f.write(b"[[usermsg]]\nname = \"X\"\nfields = []\n")
            n = sync_usermsg_schemas(game, src)
            self.assertEqual(n, 1)
            self.assertTrue(os.path.exists(
                os.path.join(game, "svencoop", "metahook", "configs", "halflifecli", "usermsgs", "a.toml")))


# ---------------------------------------------------------------------------
# Command output limits
# ---------------------------------------------------------------------------

class TestTruncateOutput(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self._log_dir = patch.object(manager_module, "COMMAND_LOG_DIR", self._tmp.name)
        self._log_dir.start()
        self.addCleanup(self._log_dir.stop)

    def test_negative_keeps_every_line(self):
        text = "\n".join(f"line{i}" for i in range(50))
        self.assertEqual(text, truncate_output(text, -1, "head", "cmd"))
        self.assertEqual([], os.listdir(self._tmp.name))

    def test_under_or_at_cap_is_unchanged(self):
        text = "a\nb\nc"
        for max_lines in (3, 5):
            with self.subTest(max_lines=max_lines):
                self.assertEqual(text, truncate_output(text, max_lines, "head", "cmd"))
        self.assertEqual([], os.listdir(self._tmp.name))

    def test_bad_keep_raises(self):
        for max_lines in (-1, 1):
            with self.subTest(max_lines=max_lines), self.assertRaises(ToolError):
                truncate_output("a\nb", max_lines, "middle", "cmd")
        self.assertEqual([], os.listdir(self._tmp.name))

    def test_head_keeps_start_and_writes_full_log(self):
        text = "\n".join(f"line{i}" for i in range(100))
        lines = truncate_output(text, 10, "head", "cvarlist").split("\n")
        self.assertEqual([f"line{i}" for i in range(10)], lines[:10])
        self.assertIn("90 of 100 lines omitted", lines[10])
        path = lines[10].split("full output: ", 1)[1].removesuffix("]")
        self.assertTrue(os.path.exists(path))
        with open(path, encoding="utf-8") as f:
            self.assertEqual(text, f.read())

    def test_tail_keeps_end(self):
        text = "\n".join(f"line{i}" for i in range(100))
        lines = truncate_output(text, 10, "tail", "cmd").split("\n")
        self.assertEqual(11, len(lines))  # marker + 10 tail lines
        self.assertIn("90 of 100 lines omitted", lines[0])
        self.assertEqual([f"line{i}" for i in range(90, 100)], lines[1:])

    def test_both_keeps_start_and_end(self):
        text = "\n".join(f"line{i}" for i in range(100))
        for max_lines, head_count in ((10, 5), (9, 5), (1, 1)):
            with self.subTest(max_lines=max_lines):
                lines = truncate_output(text, max_lines, "both", "cmd").split("\n")
                self.assertEqual(max_lines + 1, len(lines))
                self.assertEqual([f"line{i}" for i in range(head_count)], lines[:head_count])
                self.assertIn(f"{100 - max_lines} of 100 lines omitted", lines[head_count])
                tail_start = 100 - (max_lines - head_count)
                self.assertEqual([f"line{i}" for i in range(tail_start, 100)], lines[head_count + 1:])

    def test_zero_returns_only_marker(self):
        text = "\n".join(f"line{i}" for i in range(7))
        for keep in ("head", "tail", "both"):
            with self.subTest(keep=keep):
                out = truncate_output(text, 0, keep, "cmd")
                self.assertNotIn("\n", out)
                self.assertIn("7 of 7 lines omitted; full output: ", out)

    def test_same_command_gets_distinct_logs(self):
        text = "a\nb\nc"
        first = truncate_output(text, 1, "head", "cvarlist").split("full output: ", 1)[1]
        second = truncate_output(text, 1, "head", "cvarlist").split("full output: ", 1)[1]
        self.assertNotEqual(first, second)
        self.assertEqual(2, len(os.listdir(self._tmp.name)))

    def test_unicode_output_and_command_filename(self):
        text = "状态正常\n地图已加载\n玩家就绪"
        out = truncate_output(text, 1, "head", 'echo ../状态; "ok"')
        path = out.split("full output: ", 1)[1].removesuffix("]")
        self.assertEqual(self._tmp.name, os.path.dirname(path))
        with open(path, encoding="utf-8") as f:
            self.assertEqual(text, f.read())

    def test_unwritable_log_dir_still_returns_capped_output(self):
        blocker = os.path.join(self._tmp.name, "not-a-dir")
        with open(blocker, "w"):
            pass
        with patch.object(manager_module, "COMMAND_LOG_DIR", blocker):
            with self.assertLogs(manager_module.log, "WARNING"):
                lines = truncate_output("a\nb\nc", 1, "head", "cmd").split("\n")
        self.assertEqual("a", lines[0])
        self.assertIn("2 of 3 lines omitted; full output not saved: ", lines[1])


# ---------------------------------------------------------------------------
# Fake RCON server + tool tests over the in-memory MCP client
# ---------------------------------------------------------------------------

class FakeRconServer:
    """Minimal Source RCON server for one command round-trip, in a thread.

    `canned` maps a command substring to a literal reply body; the first
    matching entry wins, otherwise the command is echoed (+ " fake-output").
    """

    def __init__(self, password="", echo=True, delay=0.0, canned=None):
        self.password = password
        self.echo = echo
        self.delay = delay
        self.canned = list((canned or {}).items())
        self.commands = []  # every executed command, in arrival order
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        conn, _ = self._listener.accept()
        with conn:
            conn.settimeout(5)
            authed = False
            while True:
                size = conn.recv(4)
                if len(size) < 4:
                    break
                (n,) = struct.unpack("<i", size)
                payload = b""
                while len(payload) < n:
                    chunk = conn.recv(n - len(payload))
                    if not chunk:
                        return
                    payload += chunk
                rid, rtype = struct.unpack("<ii", payload[:8])
                body = payload[8:].split(b"\0", 1)[0]
                if rtype == rcon_client.SERVERDATA_AUTH:
                    ok = not self.password or body == self.password.encode()
                    if ok:
                        authed = True
                        reply = (rid, rcon_client.SERVERDATA_AUTH_RESPONSE, b"")
                    else:
                        reply = (-1, rcon_client.SERVERDATA_AUTH_RESPONSE, b"")
                elif rtype == rcon_client.SERVERDATA_EXECCOMMAND:
                    if not authed:
                        return
                    if self.delay:
                        import time; time.sleep(self.delay)
                    cmd = body.decode()
                    self.commands.append(cmd)
                    text = None
                    for needle, reply_body in self.canned:
                        if needle in cmd:
                            text = reply_body.encode()
                            break
                    if text is None:
                        text = (cmd + " fake-output\n").encode() if self.echo else b""
                    reply = (rid, rcon_client.SERVERDATA_RESPONSE_VALUE, text)
                else:
                    continue
                rbody = reply[2]
                out = struct.pack("<iii", 10 + len(rbody), reply[0], reply[1]) + rbody + b"\0\0"
                conn.sendall(out)

    def close(self):
        self._listener.close()


class FakeGame:
    """A GameProcess shell used by Manager without a real engine process."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.alive = True
        self.pid = 1234
        self.returncode = None

    @property
    def next_cursor(self):
        return 0

    def lines_since(self, cursor, max_lines):
        return game_process.ConsoleSlice(lines=[], next_cursor=0, dropped=0)


def _attach_fake(manager, fake):
    """Point a Manager at a fake game without triggering resolve/port files."""
    manager._proc = fake
    manager._game_dir = tempfile.gettempdir()
    manager._host = fake.host
    manager._port = fake.port
    manager._password = ""


class TestToolsOverMemory(unittest.TestCase):
    def _client(self, manager):
        # build a fresh server bound to a Manager injected with a fake game
        import contextlib
        from mcp.server import MCPServer

        @contextlib.asynccontextmanager
        async def lifespan(_server):
            yield
        server = MCPServer("halflife-test", lifespan=lifespan)

        def launch(extra_args, game_dir, timeout_s):
            return manager.launch_game(extra_args, game_dir, timeout_s)
        def status():
            return manager.game_status()
        def run(command, max_lines=DEFAULT_COMMAND_MAX_LINES, keep="head"):
            return manager.run_command(command, max_lines, keep)
        def find(name):
            return manager.find_cvar(name)
        def read_console(max_lines, cursor):
            return manager.read_console(max_lines, cursor)

        for fn, name in ((launch, "launch_game"), (status, "game_status"),
                         (run, "run_command"), (find, "find_cvar"),
                         (read_console, "read_console")):
            server.tool(name=name)(fn)
        return server

    def test_run_and_find(self):
        fake_srv = FakeRconServer(password="pw", echo=True)
        try:
            mgr = Manager()
            _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
            mgr._password = "pw"
            server = self._client(mgr)

            async def main():
                from mcp import Client
                async with Client(server) as client:
                    r = await client.call_tool("run_command", {"command": "status"})
                    self.assertFalse(r.is_error)
                    self.assertIn("fake-output", r.content[0].text)
                    r = await client.call_tool("find_cvar", {"name": "sv_cheats"})
                    self.assertIn("cli.find sv_cheats", r.content[0].text)
            anyio.run(main)
        finally:
            fake_srv.close()

    def test_run_command_truncates_and_saves_full_output(self):
        fake_srv = FakeRconServer(password="pw", canned={"status": "a\nb\nc\nd\ne\n"})
        self.addCleanup(fake_srv.close)
        mgr = Manager()
        _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
        mgr._password = "pw"
        self.addCleanup(lambda: mgr._rcon.close() if mgr._rcon else None)
        server = self._client(mgr)
        with tempfile.TemporaryDirectory() as log_dir:
            with patch.object(manager_module, "COMMAND_LOG_DIR", log_dir):
                async def main():
                    from mcp import Client
                    async with Client(server) as client:
                        full = await client.call_tool("run_command", {"command": "status"})
                        self.assertFalse(full.is_error)
                        self.assertEqual("a\nb\nc\nd\ne", full.content[0].text)
                        capped = await client.call_tool(
                            "run_command", {"command": "status", "max_lines": 2})
                        self.assertFalse(capped.is_error)
                        lines = capped.content[0].text.split("\n")
                        self.assertEqual(["a", "b"], lines[:2])
                        self.assertIn("3 of 5 lines omitted", lines[2])
                        path = lines[2].split("full output: ", 1)[1].removesuffix("]")
                        with open(path, encoding="utf-8") as f:
                            self.assertEqual("a\nb\nc\nd\ne", f.read())
                anyio.run(main)

    def test_bad_keep_is_rejected_before_sending(self):
        mgr = Manager()
        with patch.object(mgr, "_connection_locked") as connection:
            for max_lines in (-1, 1):
                with self.subTest(max_lines=max_lines), self.assertRaises(ToolError):
                    mgr.run_command("map crossfire", max_lines, "middle")
            connection.assert_not_called()

    def test_real_run_command_schema_caps_by_default(self):
        async def main():
            from mcp import Client
            async with Client(halflife_mcp.mcp) as client:
                tools = {t.name: t for t in (await client.list_tools()).tools}
                props = tools["run_command"].input_schema["properties"]
                self.assertEqual(200, props["max_lines"]["default"])
                self.assertEqual(-1, props["max_lines"]["minimum"])
                self.assertEqual("head", props["keep"]["default"])
                self.assertEqual(["head", "tail", "both"], props["keep"]["enum"])
                self.assertNotIn("truncate", props)
        anyio.run(main)

    def test_real_run_command_default_cap_and_internal_full_output(self):
        text = "\n".join(f"line{i}" for i in range(201))
        mgr = Manager()
        with tempfile.TemporaryDirectory() as log_dir:
            with patch.object(manager_module, "COMMAND_LOG_DIR", log_dir), \
                    patch.object(mgr, "_connection_locked") as connection, \
                    patch.object(halflife_mcp, "manager", mgr):
                connection.return_value.command.return_value = text
                async def main():
                    from mcp import Client
                    async with Client(halflife_mcp.mcp) as client:
                        capped = await client.call_tool("run_command", {"command": "cvarlist"})
                        self.assertFalse(capped.is_error)
                        lines = capped.content[0].text.split("\n")
                        self.assertEqual([f"line{i}" for i in range(200)], lines[:200])
                        self.assertIn("1 of 201 lines omitted", lines[200])
                        full = await client.call_tool(
                            "run_command", {"command": "cvarlist", "max_lines": -1})
                        self.assertEqual(text, full.content[0].text)
                anyio.run(main)
                self.assertEqual(text, mgr.run_command("cvarlist"))
                own = 'cli.find: "sv_cheats" exists (cvar, value "0")'
                connection.return_value.command.return_value = text + "\n" + own
                self.assertEqual(own, mgr.find_cvar("sv_cheats"))
                self.assertEqual(1, len(os.listdir(log_dir)))

    def test_real_run_command_rejects_invalid_options_before_sending(self):
        mgr = Manager()
        with patch.object(mgr, "_connection_locked") as connection, \
                patch.object(halflife_mcp, "manager", mgr):
            async def main():
                from mcp import Client
                async with Client(halflife_mcp.mcp) as client:
                    for options in ({"max_lines": -2}, {"keep": "middle"}):
                        with self.subTest(options=options):
                            result = await client.call_tool(
                                "run_command", {"command": "map crossfire", **options})
                            self.assertTrue(result.is_error)
            anyio.run(main)
            connection.assert_not_called()

    def test_wrong_password_is_tool_error(self):
        fake_srv = FakeRconServer(password="right", echo=True)
        try:
            mgr = Manager()
            _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
            mgr._password = "wrong"
            server = self._client(mgr)

            async def main():
                from mcp import Client
                async with Client(server) as client:
                    r = await client.call_tool("run_command", {"command": "status"})
                    self.assertTrue(r.is_error)
            anyio.run(main)
        finally:
            fake_srv.close()

    def test_probe_with_stale_port_file(self):
        # A port file left by an earlier run, nobody listening: not attached.
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "svencoop.exe"), "wb").close()
            data_dir = os.path.join(d, "svencoop", "metahook", "configs", "halflifecli")
            os.makedirs(data_dir)
            with socket.socket() as s:
                s.bind(("127.0.0.1", 0))
                closed_port = s.getsockname()[1]
            with open(os.path.join(data_dir, "halflifecli.port"), "w") as f:
                f.write(f"{closed_port}\n")
            saved = {k: os.environ.get(k) for k in ("GAME_DIR", "HALFLIFE_DISABLE_ATTACH")}
            os.environ["GAME_DIR"] = d
            os.environ.pop("HALFLIFE_DISABLE_ATTACH")
            try:
                self.assertEqual("none", Manager().game_status().mode)
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v

    def test_no_game(self):
        mgr = Manager()
        server = self._client(mgr)

        async def main():
            from mcp import Client
            async with Client(server) as client:
                r = await client.call_tool("run_command", {"command": "status"})
                self.assertTrue(r.is_error)
                self.assertIn("launch_game", r.content[0].text)
        anyio.run(main)

    def test_usermsg_tools(self):
        report = ("cli.usermsg: schema=svencoop.toml coord_size=4 messages=101 "
                  "display=on hooks: 85 wrapped, 16 self-registered, 0 pending")
        events_page = "\n".join([
            "cli.usermsg: events channel=usermsg newest=42 name=CurWeapon",
            "#40 [usermsg] CurWeapon size=5 state=1 weaponId=18 clip=35",
            "#42 [usermsg] CurWeapon size=5 state=0 weaponId=18 clip=-1",
        ])
        fake_srv = FakeRconServer(password="pw", echo=True, canned={
            "cli.usermsg events": events_page,
            "cli.usermsg off": "cli.usermsg: display off (recording continues)",
            "cli.usermsg": report,
        })
        try:
            mgr = Manager()
            _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
            mgr._password = "pw"

            async def main():
                st = await anyio.to_thread.run_sync(mgr.usermsg_status)
                self.assertEqual(st.messages, 101)
                self.assertEqual(st.wrapped, 85)
                got = await anyio.to_thread.run_sync(
                    lambda: mgr.usermsg_events(39, 50, "CurWeapon"))
                self.assertEqual(got.newest_seq, 42)
                self.assertEqual([e.seq for e in got.events], [40, 42])
                self.assertEqual(got.events[0].name, "CurWeapon")
                self.assertIsNone(got.more_after)
                off = await anyio.to_thread.run_sync(lambda: mgr.usermsg_set_display(False))
                self.assertIn("display off", off)
            anyio.run(main)
        finally:
            fake_srv.close()

    def _input_manager(self, canned):
        fake_srv = FakeRconServer(password="pw", echo=True, canned=canned)
        mgr = Manager()
        _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
        mgr._password = "pw"
        return fake_srv, mgr

    def test_send_key_actions(self):
        fake_srv, mgr = self._input_manager({
            "cli.trapkey w 1": "cli.trapkey: w (119) down",
            "cli.trapkey w 0": "cli.trapkey: w (119) up",
            "cli.trapkey SPACE 1": "cli.trapkey: SPACE (32) down",
        })
        try:
            async def main():
                tap = await anyio.to_thread.run_sync(lambda: mgr.send_key("w", "tap", 0))
                self.assertEqual("cli.trapkey: w (119) down\ncli.trapkey: w (119) up", tap)
                press = await anyio.to_thread.run_sync(lambda: mgr.send_key("SPACE", "press", 0))
                self.assertEqual("cli.trapkey: SPACE (32) down", press)
                await anyio.to_thread.run_sync(lambda: mgr.send_key("w", "release", 0))
            anyio.run(main)
            self.assertEqual(["cli.trapkey w 1", "cli.trapkey w 0", "cli.trapkey SPACE 1", "cli.trapkey w 0"],
                             fake_srv.commands)
        finally:
            fake_srv.close()

    def test_send_mouse_tap_releases_all_buttons(self):
        fake_srv, mgr = self._input_manager({})
        try:
            async def main():
                await anyio.to_thread.run_sync(lambda: mgr.send_mouse(3, "tap", 0))
            anyio.run(main)
            self.assertEqual(["cli.trapmouse 3 1", "cli.trapmouse 0 0"], fake_srv.commands)
        finally:
            fake_srv.close()

    def test_send_input_plugin_errors(self):
        fake_srv, mgr = self._input_manager({
            "cli.trapkey nosuchkey": 'cli.trapkey: error: unknown key "nosuchkey"',
        })
        try:
            async def main():
                from mcp.server.mcpserver.exceptions import ToolError
                with self.assertRaises(ToolError) as caught:
                    await anyio.to_thread.run_sync(lambda: mgr.send_key("nosuchkey", "tap", 0))
                self.assertIn("unknown key", str(caught.exception))
            anyio.run(main)
            # a failed press is not followed by a release
            self.assertEqual(["cli.trapkey nosuchkey 1"], fake_srv.commands)
        finally:
            fake_srv.close()

    def test_send_input_rejects_bad_arguments(self):
        from mcp.server.mcpserver.exceptions import ToolError
        mgr = Manager()
        for key in ("", "a b", "w;quit", '"w'):
            with self.assertRaises(ToolError):
                mgr.send_key(key, "tap", 0)
        for buttons in (0, 32, -1):
            with self.assertRaises(ToolError):
                mgr.send_mouse(buttons, "tap", 0)
        with self.assertRaises(ToolError):
            mgr.send_key("w", "hold", 0)

    def test_usermsg_status_rejects_garbage(self):
        fake_srv = FakeRconServer(password="pw", echo=True)  # echoes, no report line
        try:
            mgr = Manager()
            _attach_fake(mgr, FakeGame("127.0.0.1", fake_srv.port))
            mgr._password = "pw"

            async def main():
                from mcp.server.mcpserver.exceptions import ToolError
                with self.assertRaises(ToolError):
                    await anyio.to_thread.run_sync(mgr.usermsg_status)
            anyio.run(main)
        finally:
            fake_srv.close()


# ---------------------------------------------------------------------------
# stdio smoke test
# ---------------------------------------------------------------------------

class TestStdioSmoke(unittest.TestCase):
    def test_tool_listing(self):
        from mcp import Client
        from mcp.client.stdio import StdioServerParameters
        script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "halflife_mcp.py")
        params = StdioServerParameters(command="uv", args=["run", "--script", script])

        async def main():
            async with Client(params) as client:
                tools = await client.list_tools()
                names = {t.name for t in tools.tools}
                expected = {"launch_game", "game_status", "run_command",
                            "find_cvar", "send_key", "send_mouse",
                            "read_console", "snapshot", "quit_game",
                            "usermsg_status", "usermsg_messages", "usermsg_events",
                            "usermsg_set_display", "usermsg_reload_schema"}
                self.assertEqual(names, expected)

        anyio.run(main)


# ---------------------------------------------------------------------------
# End-to-end (opt-in)
# ---------------------------------------------------------------------------

class TestEndToEnd(unittest.TestCase):
    def test_full_cycle(self):
        if os.environ.get("HALFLIFE_E2E") != "1":
            self.skipTest("set HALFLIFE_E2E=1 to run the real-game end-to-end test")

        async def main():
            from mcp import Client
            server = halflife_mcp.mcp
            async with Client(server) as client:
                async def call(name, args=None):
                    r = await client.call_tool(name, args or {})
                    if r.is_error:
                        raise AssertionError(f"{name} failed: {r.content[0].text}")
                    return r

                launched = await call("launch_game", {"timeout_s": 120})
                self.assertTrue(launched.structured_content.get("running"))
                self.assertEqual(launched.structured_content.get("mode"), "managed")
                self.assertEqual("goldsrc-udp", launched.structured_content.get("protocol"))
                status = await call("game_status")
                self.assertTrue(status.structured_content.get("running"))

                echo = await call("run_command", {"command": "echo hello_from_e2e"})
                self.assertIn("hello_from_e2e", echo.content[0].text)

                found = await call("find_cvar", {"name": "sv_cheats"})
                self.assertIn("exists", found.content[0].text)
                await call("send_key", {"key": "k", "action": "release"})
                await call("send_mouse", {"buttons": 1, "action": "release"})
                await call("usermsg_status")
                await call("usermsg_messages")
                await call("usermsg_set_display", {"enabled": False})
                await call("usermsg_reload_schema", {"sync_from_repo": False})
                await call("usermsg_set_display", {"enabled": True})

                await call("run_command", {"command": "map osprey"})
                import time
                deadline = time.time() + 25
                loaded = False
                cursor = None
                while time.time() < deadline:
                    window = (await call("read_console", {"max_lines": 500, "cursor": cursor})).structured_content
                    cursor = window["next_cursor"]
                    # Map-load markers seen in the console stream: the map's
                    # sound cache line and the materials-list load.
                    if any(("osprey" in ln.lower()) or ("loading materials list" in ln.lower())
                           for ln in window["lines"]):
                        loaded = True
                        break
                    time.sleep(1)
                self.assertTrue(loaded, "map load not observed in console")

                shot = await call("snapshot", {"max_edge": 1280})
                types = {b.type for b in shot.content}
                self.assertIn("image", types)
                self.assertTrue(any(getattr(b, "mime_type", "") == "image/png" for b in shot.content))
                events = await call("usermsg_events")
                self.assertTrue(events.structured_content.get("events"))

                stopped = await call("quit_game", {"timeout_s": 30})
                self.assertNotIn("killed", stopped.content[0].text)
                self.assertIn("exit code 0", stopped.content[0].text)

        anyio.run(main)


if __name__ == "__main__":
    unittest.main(verbosity=2)
