#!/usr/bin/env python3
"""Minimal Source RCON client for halflife-cli acceptance testing.

Protocol (little-endian): [size:int32][id:int32][type:int32][body][NUL][NUL],
size = 10 + len(body). Types: AUTH=3, AUTH_RESPONSE=2 (response direction),
EXECCOMMAND=2 (request direction), RESPONSE_VALUE=0.

Usage:
  python rcon_client.py <host> <port> <password> <command> [command...]
Example:
  python rcon_client.py 127.0.0.1 54321 "" "echo hello" "status" "snapshot"
"""

import socket
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

    def __init__(self, host: str, port: int, password: str, timeout: float = CONNECT_TIMEOUT_S):
        self.host = host
        self.port = port
        self._password = password
        self._timeout = timeout
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
        """Connect and authenticate now instead of on the first command."""
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
                return run_command(self._ensure_connected(), self._take_id(), text)
            except BaseException:
                self._close_locked()
                raise

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
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(2)
    host, port, password = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    commands = sys.argv[4:]

    sock = socket.create_connection((host, port), timeout=10)
    try:
        connect(sock, host, port, password)  # auth handshake; socket already connected
        print(f"[connected+authed] {host}:{port}")
        for i, cmd in enumerate(commands):
            out = run_command(sock, 100 + i, cmd)
            print(f"$ {cmd}\n{out}\n")
    finally:
        sock.close()
