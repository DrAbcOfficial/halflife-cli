#!/usr/bin/env python3
"""halflife-cli acceptance test.

Launches Sven Co-op with piped stdio (plugin CLI automation mode), waits for
the RCON banner, then exercises the RCON path: auth, echo, a DPrintf-covered
command, snapshot, and quit. Verifies the screenshot file appears.

The game directory is resolved from --game, then GAME_DIR, then
scripts/game_dir.txt (see find_game.py). There is no default path.

Usage: python scripts/acceptance_test.py [--game "D:\\...\\Sven Co-op"]
"""

import glob
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcon_client
from find_game import resolve_game_dir
from game_process import BANNER_TIMEOUT_S, GameProcess, build_game_argv, screenshots_dir


def parse_args():
    if "--game" in sys.argv:
        return sys.argv[sys.argv.index("--game") + 1]
    return None


def main():
    game_dir, info = resolve_game_dir(parse_args())
    if not game_dir:
        print(f"FAIL: no game directory resolved ({info}).")
        print("Run scripts/find_game.py, set GAME_DIR, write scripts/game_dir.txt, "
              "or pass --game <path>.")
        return 1
    print(f"[test] game dir: {game_dir} (source: {info})")
    mod_dir = os.path.join(game_dir, "svencoop")
    shots_dir = screenshots_dir(game_dir)

    # Fixed config for the run: password set so both auth paths are exercised.
    ini_dir = os.path.join(mod_dir, "metahook", "configs")
    os.makedirs(ini_dir, exist_ok=True)
    ini_path = os.path.join(ini_dir, "halflifecli.ini")
    with open(ini_path, "w") as f:
        f.write("[rcon]\npassword=test123\n\n[cli]\nhide_window=1\ndeveloper=1\n")
    print("[test] wrote config with password=test123")

    failures = []

    # Local-file screenshots come from the engine `screenshot` command; the
    # `snapshot` command is taken over by SteamScreenshots.dll and uploads to
    # Steam without writing a file. Format varies (svencoop writes .tga).
    image_exts = (".bmp", ".tga", ".png", ".jpg", ".jpeg")
    def shots():
        return {os.path.normpath(p) for p in glob.glob(os.path.join(shots_dir, "*"))
                if p.lower().endswith(image_exts)}

    shots_before = shots()

    proc = GameProcess(build_game_argv(game_dir), cwd=game_dir)
    proc.start()
    print(f"[test] launched pid={proc.pid}, waiting for RCON banner...")
    try:
        host, port = proc.wait_for_banner(BANNER_TIMEOUT_S)
    except Exception as e:
        print(f"FAIL: {e}. Game stdout:")
        for line in proc.tail(40):
            print("   |", line)
        proc.kill()
        return 1

    print(f"[test] RCON endpoint {host}:{port}")

    # Port file must exist for headless discovery.
    port_file = os.path.join(mod_dir, "metahook", "configs", "halflifecli.port")
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
    sock2 = socket.create_connection((host, port), timeout=10)
    try:
        rcon_client.send_packet(sock2, 1, rcon_client.SERVERDATA_AUTH, b"definitely-wrong")
        rid, rtype, _ = rcon_client.recv_packet(sock2)
        auth_failed = (rtype == rcon_client.SERVERDATA_AUTH_RESPONSE and rid == -1)
        if auth_failed:
            print("[test] wrong-password rejection ok")
        else:
            failures.append(f"wrong password accepted (id={rid})")
    except Exception as e:
        failures.append(f"wrong-password test exception: {e}")
    finally:
        sock2.close()

    # The wrong-password attempt got the server's one-strike disconnect; open a
    # fresh connection for the authenticated commands.
    sock = socket.create_connection((host, port), timeout=10)
    try:
        rcon_client.connect(sock, host, port, "test123")
        print("[test] auth ok")

        out = rcon_client.run_command(sock, 101, "echo hello_from_rcon")
        if "hello_from_rcon" in out:
            print("[test] echo ok")
        else:
            failures.append(f"echo output missing: {out!r}")

        out = rcon_client.run_command(sock, 102, "version")
        if "Protocol version" in out:
            print(f"[test] version ok ({len(out)} chars)")
        else:
            failures.append(f"version output missing: {out!r}")

        # in-game screenshot: load a small map, give it time to render, shoot
        out = rcon_client.run_command(sock, 103, "map osprey")
        print(f"[test] map osprey -> {out[:80]!r}")
        time.sleep(25)  # map load + a few rendered frames
        out = rcon_client.run_command(sock, 104, "screenshot")
        print(f"[test] screenshot -> {out[:120]!r}")
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
        s = socket.create_connection((host, port), timeout=10)
        try:
            rcon_client.connect(s, host, port, "test123")
            rcon_client.run_command(s, 200, "quit")
        finally:
            s.close()

    result = proc.stop(quit_command=rcon_quit, timeout=30)
    if result.killed:
        failures.append("game had to be killed after quit")
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
