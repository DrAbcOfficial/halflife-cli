"""Reads the plugin's halflifecli.toml and RCON port file from a game install.

Both files live under <game>/<mod>/metahook/configs/; paths are built by
game_process. Everything here tolerates absence: a missing or broken file
reads as "no setting", like the plugin's own compiled-in defaults.
"""

import os
import dataclasses
import ipaddress
import json
import time
import tomllib

from game_process import plugin_config_path, port_file_path, plugin_config_dir, rcon_connect_host
from rcon_client import PROTOCOLS


@dataclasses.dataclass(frozen=True)
class Endpoint:
    protocol: str
    host: str
    port: int
    pid: int | None = None
    process_start_filetime: str | None = None


def process_start_filetime(pid):
    """Windows process identity, not just PID existence (PIDs are reused)."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        exit_code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)) or exit_code.value != 259:
            return None
        values = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(v) for v in values)):
            return None
        return str((values[0].dwHighDateTime << 32) | values[0].dwLowDateTime)
    finally:
        kernel.CloseHandle(handle)


def read_endpoint_file(game_dir, expected_pid=None):
    """Validated metadata, or an explicitly Source-TCP legacy single-line file.

    Present but invalid/non-ready metadata is authoritative: never downgrade to
    a stale port file or guess the protocol by sending a command twice.
    Callers must still actively probe authentication before reporting ready.
    """
    path = os.path.join(plugin_config_dir(game_dir), "halflifecli.endpoint.json")
    try:
        with open(path, encoding="utf-8") as stream:
            data = json.load(stream)
        if not isinstance(data, dict) or data.get("version") != 1 or data.get("status") != "ready":
            return None
        protocol, port, pid = data.get("protocol"), data.get("port"), data.get("pid")
        if protocol not in PROTOCOLS or type(port) is not int or not 0 < port <= 65535:
            return None
        if type(pid) is not int or pid <= 0 or (expected_pid is not None and expected_pid != pid):
            return None
        bind = str(ipaddress.IPv4Address(data["bind"]))
        start = data.get("process_start_filetime")
        if not isinstance(start, str) or not start.isdecimal() or process_start_filetime(pid) != start:
            return None
        published = data.get("published_at")
        started = int(start) / 10000000 - 11644473600
        if type(published) not in (int, float) or not started <= published + 1 <= time.time() + 6:
            return None
        return Endpoint(protocol, rcon_connect_host(bind), port, pid, start)
    except FileNotFoundError:
        port = read_port_file(game_dir)
        if port is not None:
            return Endpoint("source-tcp", rcon_connect_host(plugin_rcon_bind(game_dir)), port)
    except (OSError, ValueError, KeyError, TypeError, OverflowError):
        pass
    return None


def read_plugin_config(game_dir):
    """halflifecli.toml as parsed tables, or {} when absent/unreadable."""
    path = plugin_config_path(game_dir)
    try:
        with open(path, "rb") as f:
            return tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError):
        return {}


def plugin_rcon_password(game_dir):
    return read_plugin_config(game_dir).get("rcon", {}).get("password", "")


def plugin_rcon_bind(game_dir):
    return read_plugin_config(game_dir).get("rcon", {}).get("bind", "127.0.0.1")


def read_port_file(game_dir):
    """RCON port from the plugin's port file, or None when absent/unparsable."""
    try:
        with open(port_file_path(game_dir), "r", encoding="ascii", errors="ignore") as f:
            port = int(f.read().strip())
            return port if 0 < port <= 65535 else None
    except (OSError, ValueError):
        return None
