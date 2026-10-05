"""Session/tool regression against the real UDP client and a local fixture."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from game_process import GameTarget
from halflifecli.manager import Manager
from halflifecli.plugin_config import Endpoint
from test_udp_rcon import Server, print_packet


class FakeProcess:
    rcon_protocol = "goldsrc-udp"
    pid = 123
    alive = True
    returncode = None

    def __init__(self, host, port):
        self.host, self.port = host, port

    def start(self):
        return self

    def wait_for_banner(self, timeout):
        return self.host, self.port


class ManagerUdpTests(unittest.TestCase):
    def test_managed_launch_tools_and_endpoint_restart(self):
        def reply(sock, peer, data):
            command = data.split(b'"pw" ')[1][:-1].decode()
            canned = {
                "cli.find sv_cheats": 'cli.find: "sv_cheats" exists (cvar, value "0")\n',
                "cli.trapkey k 1": "cli.trapkey: injected\n",
                "cli.trapkey k 0": "cli.trapkey: injected\n",
            }
            print_packet(sock, peer, canned.get(command, command + "\n").encode())
        with Server(reply) as first, Server(reply) as second, tempfile.TemporaryDirectory() as directory:
            endpoint = Endpoint("goldsrc-udp", "127.0.0.1", first.port, 123, "123456")
            process = FakeProcess(endpoint.host, endpoint.port)
            manager = Manager()
            with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "1"}), \
                 patch("halflifecli.manager.resolve_target",
                       return_value=(GameTarget(directory, source="test"), None)), \
                 patch("halflifecli.manager.GameProcess", return_value=process), \
                 patch("halflifecli.manager.plugin_rcon_password", return_value="pw"), \
                 patch("halflifecli.manager.read_endpoint_file", return_value=endpoint) as metadata:
                status = manager.launch_game([], directory, 2)
                self.assertEqual(("managed", "goldsrc-udp", first.port), (status.mode, status.protocol, status.port))
                self.assertIn("sv_cheats", manager.find_cvar("sv_cheats"))
                self.assertIn("injected", manager.send_key("k", "tap", 0))
                self.assertEqual("echo before", manager.run_command("echo before"))
                metadata.return_value = Endpoint("goldsrc-udp", "127.0.0.1", second.port, 123, "123456")
                self.assertEqual("echo after", manager.run_command("echo after"))
                self.assertEqual(1, len(second.commands))
                metadata.return_value = None
                with self.assertRaisesRegex(Exception, "stale process"):
                    manager.run_command("map osprey")
                self.assertEqual(1, len(second.commands))
                manager._rcon.close()

    def test_attach_probes_udp_and_does_not_quit_external_game(self):
        with Server(lambda s, p, d: print_packet(s, p, b"ready")) as server:
            manager = Manager()
            endpoint = Endpoint("goldsrc-udp", "127.0.0.1", server.port, 123, "123456")
            with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "0"}), \
                 patch("halflifecli.manager.resolve_target",
                       return_value=(GameTarget("test", source="test"), None)), \
                 patch("halflifecli.manager.read_endpoint_file", return_value=endpoint), \
                 patch("halflifecli.manager.plugin_rcon_password", return_value="pw"):
                self.assertEqual("attached", manager.game_status().mode)
                self.assertEqual("ready", manager.run_command("echo ok"))
                manager.shutdown()
                self.assertFalse(any(b"quit" in command for command in server.commands))

    def test_attached_auth_failure_is_not_ready(self):
        with Server(lambda s, p, d: print_packet(s, p, b"Bad rcon_password.\n")) as server:
            manager = Manager()
            endpoint = Endpoint("goldsrc-udp", "127.0.0.1", server.port, 123, "123456")
            with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "0"}), \
                 patch("halflifecli.manager.resolve_target",
                       return_value=(GameTarget("test", source="test"), None)), \
                 patch("halflifecli.manager.read_endpoint_file", return_value=endpoint), \
                 patch("halflifecli.manager.plugin_rcon_password", return_value="pw"):
                self.assertFalse(manager.game_status().running)
                manager.shutdown()


if __name__ == "__main__":
    unittest.main()
