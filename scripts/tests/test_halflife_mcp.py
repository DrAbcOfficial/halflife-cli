# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "mcp>=2.2,<3",
#     "pillow>=10",
#     "pywin32>=311; sys_platform == 'win32'",
# ]
# ///
"""Tests for halflife_mcp.py. Run with:

    uv run --script scripts/tests/test_halflife_mcp.py

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

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import anyio

import game_process
import rcon_client
import halflife_mcp
from game_process import BANNER_RE, GameProcess
from halflife_mcp import find_new_screenshot, read_plugin_config, shot_to_png

os.environ["HALFLIFE_DISABLE_ATTACH"] = "1"


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------

class TestReadPluginConfig(unittest.TestCase):
    def test_sections_and_values(self):
        with tempfile.TemporaryDirectory() as d:
            cfg_dir = os.path.join(d, "svencoop", "metahook", "configs")
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
            cfg_dir = os.path.join(d, "svencoop", "metahook", "configs")
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
# Fake RCON server + tool tests over the in-memory MCP client
# ---------------------------------------------------------------------------

class FakeRconServer:
    """Minimal Source RCON server for one command round-trip, in a thread."""

    def __init__(self, password="", echo=True, delay=0.0):
        self.password = password
        self.echo = echo
        self.delay = delay
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
                    text = (body.decode() + " fake-output\n").encode() if self.echo else b""
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
        def run(command):
            return manager.run_command(command)
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
            mgr = halflife_mcp.Manager()
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

    def test_wrong_password_is_tool_error(self):
        fake_srv = FakeRconServer(password="right", echo=True)
        try:
            mgr = halflife_mcp.Manager()
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

    def test_no_game(self):
        mgr = halflife_mcp.Manager()
        server = self._client(mgr)

        async def main():
            from mcp import Client
            async with Client(server) as client:
                r = await client.call_tool("run_command", {"command": "status"})
                self.assertTrue(r.is_error)
                self.assertIn("launch_game", r.content[0].text)
        anyio.run(main)


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
                            "find_cvar", "read_console", "snapshot", "quit_game"}
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

                echo = await call("run_command", {"command": "echo hello_from_e2e"})
                self.assertIn("hello_from_e2e", echo.content[0].text)

                found = await call("find_cvar", {"name": "sv_cheats"})
                self.assertIn("exists", found.content[0].text)

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

                await call("quit_game", {"timeout_s": 30})

        anyio.run(main)


if __name__ == "__main__":
    unittest.main(verbosity=2)
