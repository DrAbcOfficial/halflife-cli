import dataclasses
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from game_process import GameTarget, plugin_config_dir
from halflifecli.plugin_config import (
    linux_process_identity,
    process_start_filetime,
    read_endpoint_file,
)


class NativeEndpointTests(unittest.TestCase):
    def test_validates_identity_and_refreshes_within_the_same_process(self):
        with tempfile.TemporaryDirectory() as root, subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(20)"],
        ) as child:
            try:
                target = GameTarget(root, native=True)
                directory = Path(plugin_config_dir(target))
                directory.mkdir(parents=True)
                file = directory / "halflifecli.endpoint.json"
                record = dict(version=1, status="ready", protocol="goldsrc-udp",
                              bind="0.0.0.0", port=27015, pid=child.pid, published_at=time.time())
                if os.name == "nt":
                    record["process_start_filetime"] = process_start_filetime(child.pid)
                    start_field = "process_start_filetime"
                else:
                    record["boot_id"], record["process_start_ticks"] = linux_process_identity(child.pid)
                    start_field = "process_start_ticks"
                self.assertTrue(record[start_field])

                def publish(**changes):
                    file.write_text(json.dumps({**record, **changes}), encoding="utf-8")

                publish()
                endpoint = read_endpoint_file(target, expected_pid=child.pid)
                self.assertEqual(("127.0.0.1", 27015, child.pid, True),
                                 (endpoint.host, endpoint.port, endpoint.pid, endpoint.native))
                self.assertEqual(endpoint, read_endpoint_file(dataclasses.replace(target, native=False)))
                self.assertIsNone(read_endpoint_file(target, expected_pid=child.pid + 1))
                publish(**{start_field: "0"})
                self.assertIsNone(read_endpoint_file(target))
                publish(status="stopped")
                self.assertIsNone(read_endpoint_file(target))
                publish(published_at=record["published_at"] + 0.1)
                self.assertNotEqual(endpoint, read_endpoint_file(target))
                publish(port=27016)
                self.assertEqual(27016, read_endpoint_file(target).port)
            finally:
                child.terminate()
                child.wait()
            self.assertIsNone(read_endpoint_file(target))

    def test_native_port_file_is_not_a_legacy_tcp_endpoint(self):
        with tempfile.TemporaryDirectory() as root:
            target = GameTarget(root, native=True)
            directory = Path(plugin_config_dir(target))
            directory.mkdir(parents=True)
            (directory / "halflifecli.port").write_text("27015\n127.0.0.1\n", encoding="ascii")
            self.assertIsNone(read_endpoint_file(target))

    @unittest.skipUnless(sys.platform.startswith("linux"), "requires the Linux launcher")
    def test_managed_linux_launcher_refreshes_a_live_udp_endpoint(self):
        from halflifecli.manager import Manager
        from rcon_client import RconConnection

        launcher_source = textwrap.dedent("""\
            #!/usr/bin/env python3
            import json
            import os
            import socket
            import sys
            import time
            from pathlib import Path

            assert "-hlcli" in sys.argv
            assert os.environ["LD_LIBRARY_PATH"].split(":")[0] == os.getcwd()
            record = dict(version=1, protocol="goldsrc-udp", bind="127.0.0.1",
                          pid=os.getpid(), boot_id=Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
                          process_start_ticks=Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19])
            endpoint = Path("svencoop/halflifecli/halflifecli.endpoint.json")

            def publish(status, port):
                temp = endpoint.with_suffix(".tmp")
                temp.write_text(json.dumps(dict(record, status=status, port=port, published_at=time.time())))
                temp.replace(endpoint)

            def open_udp():
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(("127.0.0.1", 0))
                return sock

            sock = open_udp()
            port = sock.getsockname()[1]
            publish("ready", port)
            print(f"halflife-cli: RCON listening on 127.0.0.1:{port} (goldsrc-udp, password: set)", flush=True)
            prefix = bytes([255]) * 4
            while True:
                data, peer = sock.recvfrom(4096)
                if data == prefix + b"challenge rcon\\n\\0":
                    sock.sendto(prefix + b"challenge rcon 12345\\n\\0", peer)
                    continue
                expected = prefix + b'rcon 12345 "fixture" '
                if not data.startswith(expected):
                    sock.sendto(prefix + b"lBad rcon_password.\\n\\0\\0", peer)
                    continue
                command = data[len(expected):-1]
                sock.sendto(prefix + b"l" + command + b"\\0\\0", peer)
                if command in (b"_restart", b"quit"):
                    publish("stopped", 0)
                    if command == b"quit":
                        break
                    new_socket = open_udp()
                    sock.close()
                    sock = new_socket
                    publish("ready", sock.getsockname()[1])
            sock.close()
            """)

        with tempfile.TemporaryDirectory() as root:
            launcher = Path(root) / "svencoop_linux"
            launcher.write_text(launcher_source, encoding="utf-8")
            launcher.chmod(0o700)
            target = GameTarget(root, native=True)
            directory = Path(plugin_config_dir(target))
            directory.mkdir(parents=True)
            (directory / "halflifecli.toml").write_text('[rcon]\npassword="fixture"\n', encoding="utf-8")
            manager = Manager()
            try:
                before = manager.launch_game([], root, 5)
                wrong = RconConnection(before.host, before.port, "wrong", protocol="goldsrc-udp")
                try:
                    with self.assertRaises(PermissionError):
                        wrong.open()
                finally:
                    wrong.close()
                self.assertEqual("_restart", manager.run_command("_restart"))
                after = manager.game_status()
                self.assertEqual(before.pid, after.pid)
                self.assertNotEqual(before.port, after.port)
                self.assertEqual("echo ready", manager.run_command("echo ready"))
                self.assertIn("exit code 0", manager.quit_game(5))
                self.assertIsNone(read_endpoint_file(target))
            finally:
                manager.shutdown()


if __name__ == "__main__":
    unittest.main()
