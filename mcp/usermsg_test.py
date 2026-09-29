#!/usr/bin/env python3
"""UserMsg monitor acceptance test for halflife-cli.

Launches Sven Co-op with piped stdio, verifies the plugin loaded the
svencoop.toml schema and prints [usermsg] lines.

Modes:
  python mcp/usermsg_test.py
      Local smoke test: load a map, watch a few seconds of [usermsg] output.
  python mcp/usermsg_test.py --connect HOST:PORT
      Connect to a game server and watch its user message traffic instead.

The game directory is resolved like acceptance_test.py (--game, GAME_DIR,
mcp/game_dir.txt).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testcommon import launch_test_game, parse_game_arg, resolve_or_fail

UM_PREFIX = "[usermsg]"
USAGE = ("Run mcp/find_game.py, set GAME_DIR, write mcp/game_dir.txt, "
         "or pass --game <path>.")


def parse_args():
    argv = sys.argv[1:]
    connect = None
    if "--connect" in argv:
        connect = argv[argv.index("--connect") + 1]
    return parse_game_arg(argv), connect


def collect(proc, cursor, seconds):
    end = time.time() + seconds
    lines = []
    while time.time() < end:
        sl = proc.lines_since(cursor)
        cursor = sl.next_cursor
        lines.extend(sl.lines)
        time.sleep(0.5)
    return cursor, lines


def main():
    game_override, connect_target = parse_args()
    game_dir, info = resolve_or_fail(game_override, USAGE)
    if not game_dir:
        return 1
    print(f"[test] game dir: {game_dir} (source: {info})")

    proc, host, port = launch_test_game(game_dir)
    if proc is None:
        return 1

    failures = []
    try:
        cursor, lines = collect(proc, proc.next_cursor, 5)
        # The schema-loaded line may predate the console capture being up;
        # what matters is that cli.usermsg answers with a hook report below.

        proc.send_stdin("cli.usermsg")
        cursor, lines = collect(proc, cursor, 3)
        text = "\n".join(lines)
        if "hooks:" not in text:
            failures.append("cli.usermsg produced no hook report")
        else:
            for line in lines:
                if "cli.usermsg" in line:
                    print("   |", line)

        proc.send_stdin("cli.usermsg CurWeapon")
        cursor, lines = collect(proc, cursor, 3)
        text = "\n".join(lines)
        if "state : byte" not in text:
            failures.append("cli.usermsg CurWeapon did not print its field layout")
        else:
            print("   | <CurWeapon layout printed>")

        if connect_target:
            print(f"[test] connecting to {connect_target} ...")
            proc.send_stdin(f"connect {connect_target}")
            cursor, lines = collect(proc, cursor, 45)
        else:
            print("[test] loading map osprey ...")
            proc.send_stdin("map osprey")
            cursor, lines = collect(proc, cursor, 45)

        um_lines = [ln for ln in lines if ln.startswith(UM_PREFIX)]
        print(f"[test] captured {len(um_lines)} [usermsg] lines")
        for ln in um_lines[:40]:
            print("   |", ln)

        if not um_lines:
            failures.append("no [usermsg] lines captured")
        # The engine may drop output produced before the console UI exists,
        # so additionally query live traffic: a few seconds of game time on a
        # server reliably emits ScoreInfo/TeamInfo/CurWeapon etc.
        cursor, lines = collect(proc, cursor, 10)
        um_lines2 = [ln for ln in lines if ln.startswith(UM_PREFIX)]
        if not um_lines2 and not um_lines:
            failures.append("still no [usermsg] lines after settle wait")
        for ln in um_lines2[:20]:
            print("   |", ln)

        proc.send_stdin("cli.usermsg pending")
        cursor, lines = collect(proc, cursor, 3)
        for line in lines:
            if "cli.usermsg" in line:
                print("   |", line)

        proc.send_stdin("cli.usermsg events limit 5")
        cursor, lines = collect(proc, cursor, 3)
        text = "\n".join(lines)
        if "cli.usermsg: events newest=" not in text:
            failures.append("cli.usermsg events produced no report header")
        ev_lines = [ln for ln in lines if ln.startswith("#")]
        print(f"   | events page: {len(ev_lines)} event line(s)")
        for ln in ev_lines[:5]:
            print("   |", ln)

        proc.send_stdin("cli.usermsg events limit 5 name CurWeapon")
        cursor, lines = collect(proc, cursor, 3)
        bad = [ln for ln in lines if ln.startswith("#") and "[usermsg] CurWeapon" not in ln]
        if bad:
            failures.append(f"name filter leaked {len(bad)} non-CurWeapon event(s): {bad[:2]}")

        proc.send_stdin("cli.usermsg list")
        cursor, lines = collect(proc, cursor, 3)
        listed = [ln for ln in lines if ln.startswith(("wrapped ", "self    ", "pending "))]
        if not listed:
            failures.append("cli.usermsg list produced no per-message lines")
        else:
            print(f"   | list: {len(listed)} message line(s)")
    finally:
        print("[test] quitting game ...")
        result = proc.stop()
        print("[test] stop:", result.summary)

    if failures:
        print("\nFAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
