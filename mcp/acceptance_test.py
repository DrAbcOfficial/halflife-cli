#!/usr/bin/env python3
"""halflife-cli acceptance test.

Launches Sven Co-op with piped stdio (plugin CLI automation mode), waits for
the RCON banner, then exercises the RCON path: auth, echo, a DPrintf-covered
command, snapshot, and quit. Verifies the screenshot file appears.

The game directory is resolved from --game, then GAME_DIR, then
mcp/game_dir.txt (see find_game.py). There is no default path.

Usage: python mcp/acceptance_test.py [--game "D:\\...\\Sven Co-op"]
"""

import glob
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcon_client
from game_process import IMAGE_EXTENSIONS, plugin_config_path, port_file_path, screenshots_dir
from testcommon import launch_test_game, parse_game_arg, resolve_or_fail
from halflifecli.plugin_config import read_endpoint_file

USAGE = ("Run mcp/find_game.py, set GAME_DIR, write mcp/game_dir.txt, "
         "or pass --game <path>.")


def main():
    game_dir, info = resolve_or_fail(parse_game_arg(), USAGE)
    if not game_dir:
        return 1
    print(f"[test] game dir: {game_dir} (source: {info})")
    shots_dir = screenshots_dir(game_dir)

    # Fixed config for the run: password set so both auth paths are exercised.
    cfg_path = plugin_config_path(game_dir)
    os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
    config = Path(cfg_path)
    previous = config.read_bytes() if config.exists() else None
    try:
        config.write_text('[rcon]\npassword = "test123"\n\n[cli]\nhide_window = 1\ndeveloper = 1\n')
        return run_test(game_dir, shots_dir)
    finally:
        if previous is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(previous)


def run_test(game_dir, shots_dir):

    failures = []

    # Local-file screenshots come from the engine `screenshot` command; the
    # `snapshot` command is taken over by SteamScreenshots.dll and uploads to
    # Steam without writing a file. Format varies (svencoop writes .tga).
    def shots():
        return {os.path.normpath(p) for p in glob.glob(os.path.join(shots_dir, "*"))
                if p.lower().endswith(IMAGE_EXTENSIONS)}

    shots_before = shots()

    proc, host, port = launch_test_game(game_dir)
    if proc is None:
        return 1
    endpoint = read_endpoint_file(game_dir, expected_pid=proc.pid)
    if endpoint is None or endpoint.protocol != "goldsrc-udp":
        proc.stop()
        print("FAIL: no fresh Native UDP endpoint")
        return 1
    if (endpoint.host, endpoint.port) != (host, port):
        failures.append("endpoint metadata disagrees with banner")

    # Port file must exist for headless discovery.
    port_file = port_file_path(game_dir)
    if not os.path.exists(port_file):
        print(f"FAIL: port file missing: {port_file}")
        failures.append("port file missing")
    else:
        port_file_content = open(port_file).read().strip()
        print(f"[test] port file: {port_file_content!r}")
        if port_file_content != str(port):
            print(f"FAIL: port file {port_file_content!r} != banner {port}")
            failures.append("port file disagrees with banner")

    # wrong password must be rejected (config sets password=test123)
    sock2 = rcon_client.RconConnection(host, port, "definitely-wrong", protocol=endpoint.protocol)
    try:
        sock2.open()
        failures.append("wrong password accepted")
    except PermissionError:
        print("[test] wrong-password rejection ok")
    except Exception as e:
        failures.append(f"wrong-password test exception: {e}")
    finally:
        sock2.close()

    sock = rcon_client.RconConnection(host, port, "test123", protocol=endpoint.protocol)
    try:
        sock.open()
        print("[test] auth ok")

        out = sock.command("echo hello_from_rcon")
        if "hello_from_rcon" in out:
            print("[test] echo ok")
        else:
            failures.append(f"echo output missing: {out!r}")

        out = sock.command("version")
        if "Protocol version" in out:
            print(f"[test] version ok ({len(out)} chars)")
        else:
            failures.append(f"version output missing: {out!r}")

        # in-game screenshot: load a small map, give it time to render, shoot
        info = sock.command("cli.rconinfo")
        if "protocol=goldsrc-udp status=ready" not in info or f"port={port}" not in info:
            failures.append(f"rconinfo mismatch: {info}")
        output = sock.command("cvarlist")
        if len(output) <= 4096 or "Total CVars" not in output:
            failures.append(f"long output incomplete ({len(output)} chars)")
        print(f"[test] cvar list: {len(output)} chars")
        out = sock.command("map osprey")
        print(f"[test] map osprey -> {out[:80]!r}")
        time.sleep(25)  # map load + a few rendered frames
        if "hello_active" not in sock.command("echo hello_active"):
            failures.append("active-server echo failed")
        current = read_endpoint_file(game_dir, expected_pid=proc.pid)
        if current != endpoint:
            failures.append("endpoint changed after map load")
        out = sock.command("screenshot")
        print(f"[test] screenshot -> {out[:120]!r}")
        sock.command("cli.usermsg reload")
        time.sleep(0.3)
        if "pending" not in sock.command("cli.usermsg"):
            failures.append("usermsg state unavailable after schema reload")
        sock.command("disconnect")
        time.sleep(1)
        if "hello_menu" not in sock.command("echo hello_menu"):
            failures.append("menu RCON failed after disconnect")
        cursor = proc.next_cursor
        proc.send_stdin("echo hello_stdin_menu")
        time.sleep(0.5)
        if not any("hello_stdin_menu" in line for line in proc.lines_since(cursor).lines):
            failures.append("stdin did not execute in menu")
        sock.command("map osprey")
        time.sleep(10)
        if "hello_reload" not in sock.command("echo hello_reload"):
            failures.append("RCON failed after second map load")
        print("[test] schema reload, disconnect, menu stdin and second map ok")
    except Exception as e:
        failures.append(f"rcon exception: {e}")
    finally:
        sock.close()

    # screenshot file check (the engine writes to <mod>/screenshots/)
    time.sleep(2)
    new_shots = shots() - shots_before
    if new_shots:
        newest = max(new_shots, key=os.path.getmtime)
        size_kb = os.path.getsize(newest) // 1024
        print(f"[test] screenshot written: {os.path.basename(newest)} ({size_kb} KB)")
    else:
        failures.append("no new screenshot file in screenshots dir")

    # clean shutdown through RCON
    def rcon_quit():
        s = rcon_client.RconConnection(host, port, "test123", protocol=endpoint.protocol)
        try:
            s.command("quit")
        finally:
            s.close()

    result = proc.stop(quit_command=rcon_quit, timeout=30)
    if result.killed:
        failures.append("game had to be killed after quit")
    if result.exit_code != 0:
        failures.append(f"game exited abnormally ({result.exit_code})")
    metadata = Path(plugin_config_path(game_dir)).with_name("halflifecli.endpoint.json")
    stopped = json.loads(metadata.read_text())
    if stopped.get("status") != "stopped" or stopped.get("pid") != proc.pid:
        failures.append("shutdown did not invalidate endpoint metadata")
    if os.path.exists(port_file):
        failures.append("shutdown left a stale port file")
    print(f"[test] game exit: {result.summary}")

    print("[test] last game stdout lines:")
    for line in proc.tail(15):
        print("   |", line)

    if failures:
        print("\nFAILURES:")
        for f in failures:
            print(" -", f)
        return 1
    print("\nACCEPTANCE TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
