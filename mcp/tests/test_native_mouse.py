"""Real-game mouse/focus regression; run after installing the plugin.

    python mcp/tests/test_native_mouse.py

The game directory resolves like the other scripts (GAME_DIR, mcp/game_dir.txt,
then the Sven Co-op search). HALFLIFE_MOUSE_APPID / HALFLIFE_MOUSE_MOD select
another GoldSrc app; focus_lock needs Sven Co-op gamedata. Each run temporarily
replaces the mod's plugin config and restores it. Physical input can take the
cursor over while block_input is off, so keep the mouse idle during the test.
The restart test needs ThreadGuard.dll in plugins.lst: without it SvEngine's
own bug makes quit after _restart fail fast (0xC0000409).
"""

import contextlib
import os
import re
import sys
import time
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import rcon_client
from find_game import resolve_target
from game_process import BANNER_RE, GameProcess, build_game_argv, plugin_config_path
from halflifecli.plugin_config import read_endpoint_file

POSITION = re.compile(r"cli\.mousemove: x=([0-9]+) y=([0-9]+)")
IMAGE_SIZE = (800, 600)
FIRST_ACTIVATION_TIMEOUT_S = 20
RESTART_TIMEOUT_S = 120
QUIT_TIMEOUT_S = 60


class TestNativeMouse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        appid = os.environ.get("HALFLIFE_MOUSE_APPID")
        cls.target, reason = resolve_target(None, appid, os.environ.get("HALFLIFE_MOUSE_MOD"))
        if cls.target is None:
            raise unittest.SkipTest(f"no installed game ({reason})")

    @contextlib.contextmanager
    def game(self, focus_option=""):
        path = plugin_config_path(self.target)
        saved = None
        if os.path.isfile(path):
            with open(path, "rb") as stream:
                saved = stream.read()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as stream:
            stream.write('[rcon]\npassword = ""\n[cli]\nhide_window = 2\n'
                         'block_input = true\ninput_lock = true\n' + focus_option)
        width, height = IMAGE_SIZE
        proc = GameProcess(build_game_argv(self.target, ["-w", str(width), "-h", str(height)]),
                           cwd=self.target.directory)
        # A test may reconnect (engine restart); cleanup quits through the current one.
        session = types.SimpleNamespace(proc=proc, conn=None)
        try:
            proc.start()
            proc.wait_for_banner()
            session.conn = self.connect(proc)
            yield session
        finally:
            try:
                if proc.alive:
                    conn = session.conn
                    result = proc.stop(quit_command=(lambda: conn.command("quit")) if conn else None,
                                       timeout=QUIT_TIMEOUT_S)
                    self.assertFalse(result.killed, result.summary)
                    self.assertEqual(0, result.exit_code, result.summary)
            finally:
                if session.conn is not None:
                    session.conn.close()
                if saved is None:
                    os.remove(path)
                else:
                    with open(path, "wb") as stream:
                        stream.write(saved)

    def connect(self, proc):
        endpoint = read_endpoint_file(self.target, expected_pid=proc.pid)
        self.assertIsNotNone(endpoint, "no fresh RCON endpoint metadata")
        conn = rcon_client.RconConnection(endpoint.host, endpoint.port, "", protocol=endpoint.protocol)
        conn.open()
        return conn

    def focus(self, conn):
        """The cli.focuslock reply once the engine has activated at least once."""
        deadline = time.monotonic() + FIRST_ACTIVATION_TIMEOUT_S
        while True:
            reply = conn.command("cli.focuslock")
            if "engine_active=unknown" not in reply or time.monotonic() > deadline:
                return reply
            time.sleep(0.5)

    def position(self, conn, command="cli.mousemove", expected=None):
        reply = conn.command(command)
        match = POSITION.search(reply)
        self.assertIsNotNone(match, reply)
        actual = tuple(map(int, match.groups()))
        if expected is not None:
            self.assertEqual(expected, actual, reply)
        return actual

    def test_motion_modes_and_locks(self):
        with self.game() as session:
            conn = session.conn
            self.assertIn("on engine_active=1", self.focus(conn))
            for mode in (1, 2, 0):
                conn.command(f"cli.window {mode}")
                for block in ("on", "off"):
                    conn.command(f"cli.blockinput {block}")
                    for input_lock in ("on", "off"):
                        conn.command(f"cli.inputlock {input_lock}")
                        for focus_lock in ("on", "off"):
                            with self.subTest(mode=mode, block=block, input_lock=input_lock, focus_lock=focus_lock):
                                focus = conn.command(f"cli.focuslock {focus_lock}")
                                if focus_lock == "on":
                                    self.assertIn("engine_active=1", focus)
                                elif mode == 2:
                                    self.assertIn("engine_active=0", focus)
                                self.position(conn, "cli.mousemove absolute 120 180", (120, 180))
                                self.position(conn, "cli.mousemove relative -40 20", (80, 200))
                                self.assertIn("x=80 y=200", conn.command("cli.trapmouse 1 1"))
                                self.position(conn, "cli.mousemove relative 20 -10", (100, 190))
                                self.assertIn("x=100 y=190", conn.command("cli.trapmouse 0 0"))
                                # Several command/frame boundaries keep the virtual position.
                                self.position(conn, expected=(100, 190))
                                self.position(conn, expected=(100, 190))
                print(f"window mode {mode}: 8 lock combinations verified", flush=True)
            last = (IMAGE_SIZE[0] - 1, 0)
            self.position(conn, "cli.mousemove relative 2147483647 -2147483648", last)
            for invalid in ("absolute 2147483648 0", "absolute nope 2", "bad 1 2", "relative 1"):
                self.assertIn("usage: cli.mousemove", conn.command("cli.mousemove " + invalid))
                self.position(conn, expected=last)
            self.assertIn("usage: cli.focuslock", conn.command("cli.focuslock maybe"))
            self.assertIn("focus guard=", conn.command("cli.inputlock"))
            print("bounds and invalid arguments verified", flush=True)

    def test_restart_reinstalls_cursor_and_focus(self):
        with self.game() as session:
            conn = session.conn
            self.assertIn("on engine_active=1", self.focus(conn))
            self.position(conn, "cli.mousemove absolute 300 200", (300, 200))
            cursor = session.proc.next_cursor
            try:
                conn.command("_restart")
            except OSError:
                pass  # the restart may tear the socket down before replying
            banner = session.proc.wait_for(BANNER_RE, RESTART_TIMEOUT_S, since=cursor)
            self.assertIsNone(banner.group(3), banner.group(0))
            conn.close()
            session.conn = conn = self.connect(session.proc)
            self.assertIn("on engine_active=1", self.focus(conn))
            self.position(conn, "cli.mousemove absolute 10 20", (10, 20))
            print("engine restart verified", flush=True)

    def test_focus_config_can_disable_lock(self):
        with self.game("focus_lock = false\n") as session:
            conn = session.conn
            self.assertIn("off engine_active=0", self.focus(conn))
            self.assertIn("on engine_active=1", conn.command("cli.focuslock on"))
            self.assertIn("off engine_active=0", conn.command("cli.focuslock off"))

    def test_focus_config_requires_boolean(self):
        with self.game("focus_lock = 0\n") as session:
            self.assertIn("on engine_active=1", self.focus(session.conn))


if __name__ == "__main__":
    unittest.main()
