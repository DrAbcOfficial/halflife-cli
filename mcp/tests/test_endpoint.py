import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halflifecli.plugin_config import read_endpoint_file
from game_process import plugin_config_dir


class EndpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(plugin_config_dir(self.temp.name))
        self.directory.mkdir(parents=True)
        self.created = str(int((time.time() - 10 + 11644473600) * 10000000))
        self.record = dict(version=1, status="ready", protocol="goldsrc-udp", bind="0.0.0.0",
                           port=27015, pid=123, process_start_filetime=self.created, published_at=time.time())
        self.identity = patch("halflifecli.plugin_config.process_start_filetime", return_value=self.created)
        self.identity.start()
        self.addCleanup(self.identity.stop)

    def write(self):
        (self.directory / "halflifecli.endpoint.json").write_text(json.dumps(self.record))

    def test_metadata_uses_actual_endpoint_not_old_bind(self):
        (self.directory / "halflifecli.toml").write_text('[rcon]\nbind="192.168.1.3"')
        self.write()
        endpoint = read_endpoint_file(self.temp.name)
        self.assertEqual(("goldsrc-udp", "127.0.0.1", 27015, 123),
                         (endpoint.protocol, endpoint.host, endpoint.port, endpoint.pid))

    def test_stale_pid_reuse_is_rejected(self):
        self.record["process_start_filetime"] = "123"
        self.write()
        self.assertIsNone(read_endpoint_file(self.temp.name))

    def test_malformed_or_stopped_metadata_never_falls_back_to_port(self):
        (self.directory / "halflifecli.port").write_text("12345\n")
        for field, value in (("version", 2), ("status", "stopped"), ("protocol", "guess"),
                             ("port", 0), ("port", True), ("bind", "example.com"),
                             ("published_at", time.time() + 60)):
            with self.subTest(field=field, value=value):
                original = self.record[field]
                self.record[field] = value
                self.write()
                self.assertIsNone(read_endpoint_file(self.temp.name))
                self.record[field] = original

    def test_legacy_single_line_is_tcp_only(self):
        (self.directory / "halflifecli.port").write_text("12345\n")
        endpoint = read_endpoint_file(self.temp.name)
        self.assertEqual(("source-tcp", 12345, None), (endpoint.protocol, endpoint.port, endpoint.pid))

    def test_expected_process_id_must_match(self):
        self.write()
        self.assertIsNone(read_endpoint_file(self.temp.name, expected_pid=124))


if __name__ == "__main__":
    unittest.main()
