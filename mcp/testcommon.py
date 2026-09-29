#!/usr/bin/env python3
"""Shared bootstrap for the acceptance-style test scripts (acceptance_test.py,
usermsg_test.py): game-directory resolution and the launch + RCON banner wait
with identical failure reporting.
"""

import os
import sys

from find_game import resolve_game_dir
from game_process import BANNER_TIMEOUT_S, GameProcess, build_game_argv


def parse_game_arg(argv=None):
    """The --game <path> value from the command line, or None."""
    argv = sys.argv[1:] if argv is None else argv
    if "--game" in argv:
        return argv[argv.index("--game") + 1]
    return None


def launch_test_game(game_dir):
    """Start the game and wait for the plugin's RCON banner.

    Returns (proc, host, port), or (None, None, None) after printing the
    failure and the last stdout lines (the process is killed).
    """
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
        return None, None, None
    print(f"[test] RCON endpoint {host}:{port}")
    return proc, host, port


def resolve_or_fail(game_override, usage_hint):
    """Resolve the game dir or print FAIL + guidance and return (None, None)."""
    game_dir, info = resolve_game_dir(game_override)
    if game_dir:
        return game_dir, info
    print(f"FAIL: no game directory resolved ({info}).")
    print(usage_hint)
    return None, info
