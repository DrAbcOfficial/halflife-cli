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
import time

SERVERDATA_AUTH = 3
SERVERDATA_AUTH_RESPONSE = 2
SERVERDATA_EXECCOMMAND = 2
SERVERDATA_RESPONSE_VALUE = 0

MAX_PACKET = 4110  # 10 + 4096 body, matches the server-side cap


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
