#!/usr/bin/env python3
"""halflife-cli acceptance test.

Launches Sven Co-op with piped stdio (plugin CLI automation mode), waits for
the RCON banner, then exercises the RCON path: auth, echo, a DPrintf-covered
command, snapshot, and quit. Verifies the screenshot file appears.

Usage: python scripts/acceptance_test.py [--game "D:\\...\\Sven Co-op"]
"""

import glob
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcon_client

DEFAULT_GAME = r"D:\SteamLibrary\steamapps\common\Sven Co-op"
BANNER_TIMEOUT = 120


def parse_args():
    game = DEFAULT_GAME
    if "--game" in sys.argv:
        game = sys.argv[sys.argv.index("--game") + 1]
    return game


def read_stdout(proc, lines, evt):
    try:
        for raw in iter(proc.stdout.readline, b""):
            text = raw.decode(errors="replace").rstrip()
            lines.append(text)
    except Exception:
        pass
    finally:
        evt.set()


def main():
    game_dir = parse_args()
    exe = os.path.join(game_dir, "svencoop.exe")
    mod_dir = os.path.join(game_dir, "svencoop")
    if not os.path.exists(exe):
        print(f"FAIL: {exe} not found")
        return 1

    # Fixed config for the run: password set so both auth paths are exercised.
    ini_dir = os.path.join(mod_dir, "metahook", "configs")
    os.makedirs(ini_dir, exist_ok=True)
    ini_path = os.path.join(ini_dir, "halflifecli.ini")
    with open(ini_path, "w") as f:
        f.write("[rcon]\npassword=test123\n\n[cli]\nhide_window=1\ndeveloper=1\n")
    print("[test] wrote config with password=test123")

    shots_before = set(glob.glob(os.path.join(mod_dir, "screenshots", "*.bmp")))

    proc = subprocess.Popen(
        [exe, "-windowed", "-novid"],
        cwd=game_dir,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    lines = []
    evt = threading.Event()
    t = threading.Thread(target=read_stdout, args=(proc, lines, evt), daemon=True)
    t.start()

    print(f"[test] launched pid={proc.pid}, waiting for RCON banner...")
    deadline = time.time() + BANNER_TIMEOUT
    banner = None
    while time.time() < deadline:
        for line in lines:
            m = re.search(r"RCON listening on ([\d.]+):(\d+)", line)
            if m:
                banner = m
                break
        if banner:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    if not banner:
        print("FAIL: RCON banner not seen. Game stdout:")
        for line in lines[-40:]:
            print("   |", line)
        proc.kill()
        return 1

    host, port = banner.group(1), int(banner.group(2))
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
            proc.kill()
            return 1

    failures = []

    # wrong password must be rejected (config sets password=test123)
    sock2 = __import__("socket").create_connection((host, port), timeout=10)
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

    sock = __import__("socket").create_connection((host, port), timeout=10)
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

        # in-game snapshot: load a small map, give it time to render, shoot
        out = rcon_client.run_command(sock, 103, "map osprey")
        print(f"[test] map osprey -> {out[:80]!r}")
        time.sleep(25)  # map load + a few rendered frames
        out = rcon_client.run_command(sock, 104, "snapshot")
        print(f"[test] snapshot -> {out[:120]!r}")
    except Exception as e:
        failures.append(f"rcon exception: {e}")
    finally:
        sock.close()

    # snapshot file check (svengine writes snapshots to <mod>/screenshots/)
    time.sleep(2)
    new_shots = set(glob.glob(os.path.join(mod_dir, "screenshots", "*.bmp"))) - shots_before
    if new_shots:
        newest = max(new_shots, key=os.path.getmtime)
        size_kb = os.path.getsize(newest) // 1024
        print(f"[test] screenshot written: {os.path.basename(newest)} ({size_kb} KB)")
    else:
        failures.append("no new .bmp snapshot file in screenshots dir")

    # clean shutdown through RCON
    try:
        sock = __import__("socket").create_connection((host, port), timeout=10)
        rcon_client.connect(sock, host, port, "test123")
        rcon_client.run_command(sock, 200, "quit")
        print("[test] quit sent")
        sock.close()
    except Exception as e:
        print(f"[test] quit via rcon failed ({e}); sending 'quit' via stdin")
        try:
            proc.stdin.write(b"quit\n")
            proc.stdin.flush()
        except Exception:
            pass

    try:
        rc = proc.wait(timeout=30)
        print(f"[test] game exited, code={rc}")
    except subprocess.TimeoutExpired:
        failures.append("game did not exit after quit")
        proc.kill()

    print("[test] last game stdout lines:")
    for line in lines[-15:]:
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
