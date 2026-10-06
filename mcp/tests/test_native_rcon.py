"""Real-game Native UDP RCON regression; run after installing the plugin.

    python mcp/tests/test_native_rcon.py

Sven Co-op is the default target. HALFLIFE_RCON_APPID / HALFLIFE_RCON_MOD /
HALFLIFE_RCON_DIR select another GoldSrc app (an explicit directory serves a
non-Steam install), HALFLIFE_RCON_MAP overrides the map loaded for the
active-server phase. For example:

    set HALFLIFE_RCON_APPID=10& set HALFLIFE_RCON_MOD=cstrike& set HALFLIFE_RCON_DIR=D:\\CS3266

One session covers the menu poller, failure accounting and banning (from a
127.0.0.2 peer), the empty-password locality rule, the active-server path,
returning to the menu and a clean RCON quit. The mod's plugin config is
temporarily replaced and restored. 127.0.0.2 is banned and unbanned during the
run; the failure cvars keep the test's values only until quit.
"""

import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import rcon_client
from find_game import resolve_target
from game_process import GameProcess, build_game_argv, plugin_config_path, port_file_path
from halflifecli.plugin_config import read_endpoint_file
from rcon_client import CHALLENGE_RE, OOB_HEADER, PRINT_HEADER

PASSWORD = "test123"
PEER = "127.0.0.2"
DEFAULT_MAPS = {"svencoop": "osprey", "valve": "crossfire", "cstrike": "de_dust2", "cryoffear": "c_apartment1"}
MAP_TIMEOUT_S = 90
QUIT_TIMEOUT_S = 60
PEER_TIMEOUT_S = 2
LONG_OUTPUT = 4096  # a full cvarlist needs several 1200-byte datagrams


class Peer:
    """A fixed PEER:port client. The engine identifies a failing peer by
    address and port, so failure accounting needs one socket throughout."""

    def __init__(self, host, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((PEER, 0))
        self.sock.connect((host, port))
        self.sock.settimeout(PEER_TIMEOUT_S)

    def close(self):
        self.sock.close()

    def rcon(self, password, command):
        """Challenge + rcon; the joined print text, or the raw reply when no
        challenge arrives (a banned peer only receives the ban notice)."""
        self.sock.send(OOB_HEADER + b"challenge rcon\n\0")
        try:
            reply = self.sock.recv(65536)
        except TimeoutError:
            return "<no challenge reply>"
        match = CHALLENGE_RE.fullmatch(reply)
        if not match:
            return reply.decode(errors="replace")
        self.sock.send(OOB_HEADER + b"rcon " + match[1] + f' "{password}" {command}'.encode() + b"\0")
        text = []
        while True:
            try:
                packet = self.sock.recv(65536)
            except TimeoutError:
                return "".join(text)
            if packet.startswith(PRINT_HEADER):
                text.append(packet[len(PRINT_HEADER):].rstrip(b"\0").decode(errors="replace"))


class TestNativeRcon(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.target, reason = resolve_target(os.environ.get("HALFLIFE_RCON_DIR"), os.environ.get("HALFLIFE_RCON_APPID"),
                                            os.environ.get("HALFLIFE_RCON_MOD"))
        if cls.target is None:
            raise unittest.SkipTest(f"no installed game ({reason})")
        cls.map = os.environ.get("HALFLIFE_RCON_MAP") or DEFAULT_MAPS.get(cls.target.mod)
        if not cls.map:
            raise unittest.SkipTest(f"no default map for mod {cls.target.mod}; set HALFLIFE_RCON_MAP")

    def setUp(self):
        path = plugin_config_path(self.target)
        self.saved = None
        if os.path.isfile(path):
            with open(path, "rb") as stream:
                self.saved = stream.read()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as stream:
            stream.write(f'[rcon]\npassword = "{PASSWORD}"\n[cli]\nhide_window = 1\n')
        self.proc = GameProcess(build_game_argv(self.target), cwd=self.target.directory)

    def tearDown(self):
        try:
            if self.proc.alive:
                self.proc.kill()
        finally:
            path = plugin_config_path(self.target)
            if self.saved is None:
                os.remove(path)
            else:
                with open(path, "wb") as stream:
                    stream.write(self.saved)

    def console_has(self, text):
        return any(text in line for line in self.proc.lines_since(0).lines)

    def connect(self, password=PASSWORD):
        conn = rcon_client.RconConnection(self.endpoint.host, self.endpoint.port, password, protocol=self.endpoint.protocol)
        conn.open()
        return conn

    def wait_for_map(self, conn):
        deadline = time.monotonic() + MAP_TIMEOUT_S
        while time.monotonic() < deadline:
            # A loading engine may drop a request or replace the challenge it
            # issued; status is idempotent, so poll through both.
            try:
                if self.map in conn.command("status"):
                    return
            except (TimeoutError, PermissionError):
                pass
            time.sleep(1)
        self.fail(f"map {self.map} did not become active")

    def test_native_udp_rcon(self):
        self.proc.start()
        self.proc.wait_for_banner()
        self.endpoint = read_endpoint_file(self.target, expected_pid=self.proc.pid)
        self.assertIsNotNone(self.endpoint, "no fresh endpoint metadata")
        self.assertEqual("goldsrc-udp", self.endpoint.protocol)
        self.assertFalse(self.console_has("Native UDP unavailable"))
        with open(port_file_path(self.target)) as stream:
            self.assertEqual(str(self.endpoint.port), stream.read().strip())
        peer = Peer(self.endpoint.host, self.endpoint.port)
        self.addCleanup(peer.close)

        # Main menu: the poller (native or dispatcher) serves challenge and rcon.
        with self.assertRaises(PermissionError):
            self.connect("definitely-wrong")
        conn = self.connect()
        self.assertIn("hello_menu", conn.command("echo hello_menu"))
        self.assertIn("protocol=goldsrc-udp status=ready", conn.command("cli.rconinfo"))
        cvars = conn.command("cvarlist")
        self.assertGreater(len(cvars), LONG_OUTPUT)
        self.assertIn("CVars", cvars[-200:])  # the closing count arrived
        # The dispatcher command is inert without a pending packet.
        self.assertNotIn("dispatched_from_console", conn.command("cli._rconpacket 1 2 echo dispatched_from_console"))
        print("menu: auth, echo, long output verified", flush=True)

        # Failure accounting: two failures within the window mark the peer,
        # the next request is refused and banned through addip.
        conn.command("sv_rcon_minfailures 2")
        conn.command("sv_rcon_maxfailures 5")
        conn.command("sv_rcon_minfailuretime 30")
        conn.command("sv_rcon_banpenalty 0")
        try:
            self.assertIn("Bad rcon_password", peer.rcon("wrong", "version"))
            self.assertIn("Bad rcon_password", peer.rcon("wrong", "version"))
            self.assertIn("Rcon banned", peer.rcon(PASSWORD, "version"))
            self.assertIn(PEER, conn.command("listip").replace(" ", ""))
            self.assertNotIn("Protocol version", peer.rcon(PASSWORD, "version"))
        finally:
            conn.command(f"removeip {PEER}")
            conn.command("resetrcon")
        self.assertIn("Protocol version", peer.rcon(PASSWORD, "version"))
        print("menu: failure accounting, ban and resetrcon verified", flush=True)

        # An empty password is accepted only from 127.0.0.1.
        conn.command('rcon_password ""')
        local = self.connect("")
        self.assertIn("hello_empty", local.command("echo hello_empty"))
        self.assertIn("Bad rcon_password", peer.rcon("", "echo hello_empty"))
        local.command(f'rcon_password "{PASSWORD}"')
        local.close()
        print("menu: empty password locality verified", flush=True)

        # Active server: the engine's connectionless handler reaches the hooks.
        conn.command(f"map {self.map}")
        self.wait_for_map(conn)
        self.assertIn("hello_active", conn.command("echo hello_active"))
        self.assertIn("Protocol version", peer.rcon(PASSWORD, "version"))
        self.assertEqual(self.endpoint, read_endpoint_file(self.target, expected_pid=self.proc.pid))
        print(f"active: map {self.map}, echo and peer rcon verified", flush=True)

        conn.command("disconnect")
        time.sleep(2)
        self.assertIn("hello_again", conn.command("echo hello_again"))
        print("menu after disconnect verified", flush=True)

        result = self.proc.stop(quit_command=lambda: conn.command("quit"), timeout=QUIT_TIMEOUT_S)
        conn.close()
        self.assertFalse(result.killed, result.summary)
        self.assertEqual(0, result.exit_code, result.summary)
        self.assertIsNone(read_endpoint_file(self.target))
        self.assertFalse(os.path.exists(port_file_path(self.target)))
        print("quit: exit code 0, endpoint stopped", flush=True)


if __name__ == "__main__":
    unittest.main()
