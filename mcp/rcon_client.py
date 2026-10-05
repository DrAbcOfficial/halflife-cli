#!/usr/bin/env python3
"""GoldSrc UDP (Sven) and Source TCP (legacy engines) RCON client.

Protocol (little-endian): [size:int32][id:int32][type:int32][body][NUL][NUL],
size = 10 + len(body). Types: AUTH=3, AUTH_RESPONSE=2 (response direction),
EXECCOMMAND=2 (request direction), RESPONSE_VALUE=0.

Usage:
  python rcon_client.py --protocol goldsrc-udp <host> <port> <password> <command> [command...]
Example:
  python rcon_client.py 127.0.0.1 54321 "" "echo hello" "status" "snapshot"
"""

import socket
import re
import struct
import sys
import threading
import time

SERVERDATA_AUTH = 3
SERVERDATA_AUTH_RESPONSE = 2
SERVERDATA_EXECCOMMAND = 2
SERVERDATA_RESPONSE_VALUE = 0

MAX_BODY = 4096
MAX_PACKET = 4110  # 10 + 4096 body, matches the server-side cap
CONNECT_TIMEOUT_S = 10
FIRST_COMMAND_ID = 100
MAX_COMMAND_ID = 0x7FFFFFFF  # request ids are signed int32 on the wire
PROTOCOLS = ("source-tcp", "goldsrc-udp")
OOB_HEADER = b"\xff" * 4
PRINT_HEADER = OOB_HEADER + b"l"
CHALLENGE_RE = re.compile(rb"\xff{4}challenge rcon (\d+)\n?\0?")
MAX_UDP_REQUEST = 510  # engine's 512-byte request buffer, excluding OOB header
MAX_UDP_RESPONSE = 1024 * 1024
QUIET_TIMEOUT_S = .1


class PartialResponseError(TimeoutError):
    """Total deadline expired after some output; never retry the command."""

    def __init__(self, partial_output):
        super().__init__("RCON total timeout with partial output; command may have executed")
        self.partial_output = partial_output


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed by server")
        data += chunk
    return data


def recv_packet(sock: socket.socket):
    size = struct.unpack("<i", recv_exact(sock, 4))[0]
    if size < 10 or size > MAX_PACKET:
        raise ValueError(f"packet size out of range: {size}")
    payload = recv_exact(sock, size)
    rid, rtype = struct.unpack("<ii", payload[:8])
    body = payload[8:-2]  # strip trailing NUL + empty-string NUL
    return rid, rtype, body


def send_packet(sock: socket.socket, rid: int, ptype: int, body: bytes):
    data = struct.pack("<ii", rid, ptype) + body + b"\x00\x00"
    sock.sendall(struct.pack("<i", len(data)) + data)


def connect(sock: socket.socket, host: str, port: int, password: str) -> None:
    """Perform the RCON auth handshake on an already-connected socket."""
    send_packet(sock, 1, SERVERDATA_AUTH, password.encode())
    # The server may or may not send the optional empty RESPONSE_VALUE first;
    # keep reading until we see the AUTH_RESPONSE.
    deadline = time.time() + 5
    while time.time() < deadline:
        rid, rtype, _ = recv_packet(sock)
        if rtype == SERVERDATA_AUTH_RESPONSE:
            if rid == -1:
                raise PermissionError("RCON authentication failed")
            return
    raise TimeoutError("no auth response within 5s")


def run_command(sock: socket.socket, rid: int, command: str) -> str:
    send_packet(sock, rid, SERVERDATA_EXECCOMMAND, command.encode())
    deadline = time.time() + 10
    while time.time() < deadline:
        resp_id, resp_type, body = recv_packet(sock)
        if resp_type == SERVERDATA_RESPONSE_VALUE and resp_id == rid:
            return body.decode(errors="replace")
    raise TimeoutError(f"no response for command within 10s: {command}")


class RconConnection:
    """A persistent authenticated RCON connection, safe to share between threads.

    Connects lazily on the first command. On any I/O error the socket is
    dropped and the error propagates; the next command reconnects. A failed
    command is never retried automatically: the game may already have run it,
    and console commands (map, quit, ...) are not idempotent.
    """

    def __init__(self, host: str, port: int, password: str, timeout: float = CONNECT_TIMEOUT_S,
                 *, protocol: str = "source-tcp", quiet_timeout: float = QUIET_TIMEOUT_S,
                 max_response_bytes: int = MAX_UDP_RESPONSE):
        if protocol not in PROTOCOLS:
            raise ValueError(f"unsupported RCON protocol: {protocol}")
        if timeout <= 0 or quiet_timeout <= 0 or max_response_bytes <= 0:
            raise ValueError("RCON timeouts and response limit must be positive")
        self.host = host
        self.port = port
        self.protocol = protocol
        self._password = password
        self._timeout = timeout
        self._quiet_timeout = quiet_timeout
        self._max_response_bytes = max_response_bytes
        self._sock = None
        self._next_id = FIRST_COMMAND_ID
        self._lock = threading.Lock()

    def _ensure_connected(self) -> socket.socket:
        if self._sock is None:
            sock = socket.create_connection((self.host, self.port), timeout=self._timeout)
            try:
                connect(sock, self.host, self.port, self._password)
            except BaseException:
                sock.close()
                raise
            self._sock = sock
        return self._sock

    def _take_id(self) -> int:
        rid = self._next_id
        self._next_id = FIRST_COMMAND_ID if rid >= MAX_COMMAND_ID else rid + 1
        return rid

    def open(self) -> None:
        """Probe reachability and auth. UDP uses the read-only version command."""
        if self.protocol == "goldsrc-udp":
            self.command("version")
            return
        with self._lock:
            try:
                self._ensure_connected()
            except BaseException:
                self._close_locked()
                raise

    def command(self, text: str) -> str:
        body = text.encode()
        if len(body) > MAX_BODY:
            raise ValueError(f"command is {len(body)} bytes, RCON allows at most {MAX_BODY}")
        with self._lock:
            try:
                if self.protocol == "goldsrc-udp":
                    return self._udp_command(text)
                return run_command(self._ensure_connected(), self._take_id(), text)
            except BaseException:
                self._close_locked()
                raise

    def _udp_command(self, text: str) -> str:
        """One socket/challenge per command; no command retransmission.

        UDP has no request IDs or completion frame. Aggregate arrival order
        until a quiet window (default 100ms) or the total deadline. Loss,
        duplicates and reordering cannot be detected or repaired. A quiet
        window can truncate delayed output; a total timeout reports partial
        output separately. A new socket isolates previous commands' replies.
        """
        if any(c in self._password for c in '\"\r\n\0'):
            raise ValueError("RCON password cannot contain quotes, line breaks or NUL")
        if any(c in text for c in "\r\n\0"):
            raise ValueError("RCON command cannot contain line breaks or NUL")
        suffix = (' "' + self._password + '" ' + text).encode() + b"\0"
        if len(b"rcon 4294967295" + suffix) > MAX_UDP_REQUEST:
            raise ValueError(f"UDP RCON request exceeds {MAX_UDP_REQUEST} bytes")
        # Keep the old port reserved until the new socket has its own port.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, MAX_UDP_RESPONSE)
            sock.connect((self.host, self.port))  # kernel filters foreign sources
        except BaseException:
            sock.close()
            raise
        self._close_locked()
        self._sock = sock
        deadline = time.monotonic() + self._timeout
        sock.send(OOB_HEADER + b"challenge rcon\n\0")
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no UDP RCON challenge; command not sent")
            sock.settimeout(remaining)
            try:
                packet = sock.recv(65536)
            except socket.timeout:
                raise TimeoutError("no UDP RCON challenge; command not sent") from None
            match = CHALLENGE_RE.fullmatch(packet)
            if match and len(match[1]) <= 10 and int(match[1]) <= 0xFFFFFFFF:
                break
        sock.send(OOB_HEADER + b"rcon " + match[1] + suffix)
        chunks = bytearray()
        received = False
        last_reply = 0.0
        while True:
            now = time.monotonic()
            remaining = deadline - now
            if remaining <= 0:
                if received:
                    raise PartialResponseError(chunks.decode(errors="replace"))
                raise TimeoutError("no UDP RCON response; command may have executed")
            quiet_remaining = self._quiet_timeout - (now - last_reply) if received else remaining
            if quiet_remaining <= 0:
                return chunks.decode(errors="replace")
            sock.settimeout(min(remaining, quiet_remaining))
            try:
                packet = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                raise TimeoutError("UDP RCON peer became unavailable; command may have executed") from exc
            if not packet.startswith(PRINT_HEADER) or not packet.endswith(b"\0"):
                continue
            chunk = packet[len(PRINT_HEADER):].rstrip(b"\0")
            if b"\0" in chunk:
                continue
            if chunk.startswith((b"Bad rcon_password.", b"Bad challenge.", b"Rcon banned.")):
                raise PermissionError(chunk.decode(errors="replace").strip())
            if len(chunks) + len(chunk) > self._max_response_bytes:
                raise ValueError("UDP RCON response exceeds configured byte limit; output incomplete")
            chunks.extend(chunk)
            received = True
            last_reply = time.monotonic()

    def _close_locked(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def close(self) -> None:
        with self._lock:
            self._close_locked()


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", choices=PROTOCOLS, required=True)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("password")
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args()
    connection = RconConnection(args.host, args.port, args.password, protocol=args.protocol)
    try:
        connection.open()
        print(f"[ready] {args.protocol} {args.host}:{args.port}")
        for cmd in args.commands:
            out = connection.command(cmd)
            print(f"$ {cmd}\n{out}\n")
    finally:
        connection.close()
