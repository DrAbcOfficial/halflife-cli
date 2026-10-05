"""Wire-level UDP fixtures; no game or MCP dependency required."""
import concurrent.futures
import socket
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rcon_client import RconConnection, PartialResponseError

OOB = b"\xff" * 4


class Server:
    def __init__(self, reply, challenge=OOB + b"challenge rcon 12345\n\0"):
        self.reply = reply
        self.challenge = challenge
        self.commands = []
        self.peers = []
        self.errors = []
        self.connections = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(.05)
        self.port = self.sock.getsockname()[1]
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.done.is_set():
            try:
                data, peer = self.sock.recvfrom(65536)
                if data == OOB + b"challenge rcon\n\0":
                    if self.challenge is not None:
                        self.sock.sendto(self.challenge, peer)
                else:
                    self.commands.append(data)
                    self.peers.append(peer)
                    self.reply(self.sock, peer, data)
            except (socket.timeout, ConnectionResetError):
                continue
            except Exception as exc:
                self.errors.append(exc)
                return

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.done.set()
        self.thread.join(2)
        self.sock.close()
        for connection in self.connections:
            connection.close()
        if self.errors:
            raise self.errors[0]

    def connection(self, **kwargs):
        connection = RconConnection("127.0.0.1", self.port, "pw", timeout=.3,
                                    protocol="goldsrc-udp", quiet_timeout=.04, **kwargs)
        self.connections.append(connection)
        return connection


def print_packet(sock, peer, text=b""):
    sock.sendto(OOB + b"l" + text + b"\0\0", peer)


class UdpRconTests(unittest.TestCase):
    def test_wire_command_and_empty_output(self):
        with Server(lambda s, p, d: print_packet(s, p)) as server:
            self.assertEqual("", server.connection().command("echo hello"))
            self.assertEqual([OOB + b'rcon 12345 "pw" echo hello\0'], server.commands)

    def test_open_uses_read_only_probe(self):
        with Server(lambda s, p, d: print_packet(s, p, b"version")) as server:
            server.connection().open()
            self.assertEqual([OOB + b'rcon 12345 "pw" version\0'], server.commands)

    def test_multiple_datagrams_and_invalid_packets(self):
        def reply(sock, peer, data):
            sock.sendto(b"invalid", peer)
            sock.sendto(OOB + b"lmissing terminator", peer)
            print_packet(sock, peer, b"a" * 1200)
            print_packet(sock, peer, b"b" * 1200)
            print_packet(sock, peer, b"end")
        with Server(reply) as server:
            self.assertEqual("a" * 1200 + "b" * 1200 + "end", server.connection().command("status"))

    def test_wrong_source_is_ignored(self):
        def reply(sock, peer, data):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
                print_packet(stranger, peer, b"forged")
            print_packet(sock, peer, b"real")
        with Server(reply) as server:
            self.assertEqual("real", server.connection().command("version"))

    def test_auth_errors(self):
        for message in (b"Bad rcon_password.\n", b"Bad challenge.\n", b"Rcon banned.\n"):
            with self.subTest(message=message), Server(lambda s, p, d: print_packet(s, p, message)) as server:
                with self.assertRaises(PermissionError):
                    server.connection().command("version")

    def test_timeout_does_not_repeat_command(self):
        with Server(lambda *args: None) as server:
            with self.assertRaisesRegex(TimeoutError, "may have executed"):
                server.connection().command("map osprey")
            self.assertEqual(1, len(server.commands))

    def test_delayed_reply_cannot_pollute_next_command(self):
        timers = []
        def reply(sock, peer, data):
            if b"first" in data:
                print_packet(sock, peer, b"first")
                timer = threading.Timer(.09, print_packet, (sock, peer, b"late"))
                timers.append(timer)
                timer.start()
            else:
                time.sleep(.1)
                print_packet(sock, peer, b"second")
        with Server(reply) as server:
            connection = server.connection()
            self.assertEqual("first", connection.command("echo first"))
            self.assertEqual("second", connection.command("echo second"))
            for timer in timers:
                timer.join()
            self.assertNotEqual(server.peers[0], server.peers[1])

    def test_concurrent_calls_are_serialized_and_close_can_reopen(self):
        with Server(lambda s, p, d: print_packet(s, p, d.split(b'"pw" ')[1][:-1])) as server:
            connection = server.connection()
            with concurrent.futures.ThreadPoolExecutor(4) as pool:
                replies = list(pool.map(connection.command, ["echo a", "echo b", "echo c", "echo d"]))
            self.assertEqual(["echo a", "echo b", "echo c", "echo d"], replies)
            connection.close()
            self.assertEqual("version", connection.command("version"))

    def test_invalid_request_is_rejected_before_send(self):
        with Server(lambda *args: None) as server:
            for command in ("x" * 510, "echo a\0quit", "echo a\nquit"):
                with self.subTest(command=command), self.assertRaises(ValueError):
                    server.connection().command(command)
            self.assertEqual([], server.commands)

    def test_response_limit_and_total_deadline_report_partial_output(self):
        with Server(lambda s, p, d: print_packet(s, p, b"x" * 1200)) as server:
            with self.assertRaisesRegex(ValueError, "response"):
                server.connection(max_response_bytes=100).command("version")

    def test_unknown_protocol_rejected(self):
        with self.assertRaises(ValueError):
            RconConnection("127.0.0.1", 27015, "", protocol="guess")

    def test_invalid_or_silent_challenge_never_sends_command(self):
        for challenge in (None, OOB + b"challenge rcon 999999999999\0", b"garbage"):
            with self.subTest(challenge=challenge), Server(lambda *args: None, challenge=challenge) as server:
                with self.assertRaisesRegex(TimeoutError, "command not sent"):
                    server.connection().command("map osprey")
                self.assertEqual([], server.commands)

    def test_total_timeout_exposes_partial_output_without_retry(self):
        def reply(sock, peer, data):
            for _ in range(20):
                print_packet(sock, peer, b"part")
                time.sleep(.02)
        with Server(reply) as server:
            with self.assertRaises(PartialResponseError) as raised:
                server.connection().command("status")
            self.assertTrue(raised.exception.partial_output.startswith("part"))
            self.assertEqual(1, len(server.commands))

    def test_duplicates_remain_visible_in_arrival_order(self):
        def reply(sock, peer, data):
            for item in (b"second", b"first", b"first"):
                print_packet(sock, peer, item)
        with Server(reply) as server:
            # GoldSrc has no sequence IDs: silently deduplicating identical text
            # would also lose legitimate repeated output.
            self.assertEqual("secondfirstfirst", server.connection().command("status"))


if __name__ == "__main__":
    unittest.main()
