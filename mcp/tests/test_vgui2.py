"""VGUI2 wire protocol and serialized client behavior."""
import json
import pathlib
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from halflifecli.vgui2 import VGUI2Client, decode_reply, encode_request


def reply(value, request="abc"):
    raw = json.dumps(value, ensure_ascii=False).encode()
    chunks = [raw[i:i + 384].hex() for i in range(0, len(raw), 384)]
    return "\n".join([f"VGUI2 {request} begin {len(chunks)}"] +
                     [f"VGUI2 {request} data {i} {s}" for i, s in enumerate(chunks)] +
                     [f"VGUI2 {request} end {len(chunks)}"])


class FakeManager:
    def __init__(self, values):
        self._input_lock = threading.RLock()
        self.values = iter(values)
        self.calls = []

    def run_command(self, command, max_lines=-1):
        assert max_lines == -1
        tokens = command.split()
        self.calls.append((tokens[1], json.loads(bytes.fromhex(tokens[4]))))
        value = next(self.values)
        if isinstance(value, Exception):
            raise value
        return reply(value, tokens[3])


class ProtocolTests(unittest.TestCase):
    def test_unicode_encoding_and_reordered_reply(self):
        expected = {"name": '中文\n";quit', "padding": "a" * 1500}
        encoded = encode_request(expected)
        self.assertEqual(expected, json.loads(bytes.fromhex(encoded)))
        self.assertEqual(expected, decode_reply("noise\n" + "\n".join(reversed(reply(expected).splitlines())), "abc"))

    def test_incomplete_duplicate_wrong_id_and_malformed(self):
        lines = reply({"name": "a" * 1000}).splitlines()
        for bad in (lines[:-1], lines[1:], lines[:2] + lines[3:], lines + [lines[1]],
                    ["VGUI2 abc data -1 00"], ["VGUI2 abc begin 999999999"]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                decode_reply("\n".join(bad), "abc")
        with self.assertRaises(ValueError):
            decode_reply(reply({}), "different")

    def test_supported_payload_budgets(self):
        for size in (1600, 12000):
            value = {"padding": "x" * (size - 15)}
            wire = reply(value, "0" * 32)
            self.assertEqual(value, decode_reply(wire, "0" * 32))
            if size == 1600:
                self.assertLess(len(wire.encode()), 4096)

    def test_tree_inspect_and_poll_without_replaying_action(self):
        manager = FakeManager([{"nodes": [], "next_cursor": None}, {"semantic_support": False},
                               {"status": "pending", "operation": "op"},
                               {"status": "pending", "operation": "op"}, {"status": "success"}])
        client = VGUI2Client(manager)
        self.assertEqual("", client.tree()["text_tree"])
        self.assertFalse(client.inspect("p")["semantic_support"])
        with patch("halflifecli.vgui2.time.sleep"):
            self.assertEqual("success", client.action("click", ref="p")["status"])
        self.assertEqual(["tree", "inspect", "click", "result", "result"], [c[0] for c in manager.calls])

    def test_transport_failure_does_not_repeat_click(self):
        manager = FakeManager([OSError("lost reply")])
        with self.assertRaises(OSError):
            VGUI2Client(manager).action("click", ref="p")
        self.assertEqual(1, len(manager.calls))

    def test_set_text_placeholder_stops_before_upload(self):
        manager = FakeManager([{"error": "VGUI2 set_text is unsupported"}])
        with self.assertRaisesRegex(ValueError, "unsupported"):
            VGUI2Client(manager).set_text("p", "中文" * 1000)
        self.assertEqual(1, len(manager.calls))
        self.assertEqual("begin", manager.calls[0][1]["phase"])
        self.assertNotIn("text", manager.calls[0][1])

    def test_invalid_input(self):
        manager = FakeManager([])
        client = VGUI2Client(manager)
        for text in ("a\0b", "x" * 65537):
            with self.assertRaises(ValueError):
                client.set_text("p", text)
        with self.assertRaises(ValueError):
            client.inspect("p", -1)
        with self.assertRaises(ValueError):
            client.tree(max_depth=-1)
        self.assertEqual([], manager.calls)


if __name__ == "__main__":
    unittest.main()
