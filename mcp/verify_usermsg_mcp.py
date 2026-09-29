# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "mcp>=2.2,<3",
#     "pywin32>=311; sys_platform == 'win32'",
# ]
# ///
"""Live check of the Manager-level usermsg tools (what the MCP tools call).

Launches the game, connects to the acceptance server, and exercises
usermsg_status / usermsg_events / usermsg_messages / usermsg_set_display /
usermsg_reload_schema. Ad-hoc: not part of the committed test suite.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["HALFLIFE_DISABLE_ATTACH"] = "1"

import halflife_mcp
from find_game import resolve_game_dir
from game_process import BANNER_TIMEOUT_S, GameProcess, build_game_argv

TARGET = "124.221.167.52:28347"


def main():
    gd, _ = resolve_game_dir(None)
    proc = GameProcess(build_game_argv(gd), cwd=gd)
    proc.start()
    host, port = proc.wait_for_banner(BANNER_TIMEOUT_S)
    print("banner:", host, port)

    mgr = halflife_mcp.Manager()
    mgr._proc = proc
    mgr._game_dir = gd
    mgr._host = host
    mgr._port = port
    mgr._password = halflife_mcp.plugin_rcon_password(gd)

    failures = []
    try:
        st = mgr.usermsg_status()
        print("status:", st.model_dump())

        proc.send_stdin(f"connect {TARGET}")
        time.sleep(35)

        ev = mgr.usermsg_events(None, 8, None)
        print("events newest:", ev.newest_seq, "count:", len(ev.events))
        for e in ev.events[:4]:
            print("  ", e.seq, e.name, e.size, e.detail[:70])
        if not ev.events:
            failures.append("no events over MCP path")

        ev2 = mgr.usermsg_events(max(0, ev.newest_seq - 200), 20, "SayText")
        print("SayText-filtered:", [(e.seq, e.detail[:40]) for e in ev2.events])
        if any(e.name != "SayText" for e in ev2.events):
            failures.append("name filter leaked non-SayText events")

        msgs = mgr.usermsg_messages()
        print("schema:", msgs.schema_file, "coord", msgs.coord_size,
              "messages:", len(msgs.messages), "extends:", msgs.extends)
        if len(msgs.messages) < 100:
            failures.append(f"schema message count low: {len(msgs.messages)}")

        print("set_display:", mgr.usermsg_set_display(False))
        print("reload:", mgr.usermsg_reload_schema(True))
        st2 = mgr.usermsg_status()
        print("status after reload:", st2.model_dump())
    finally:
        proc.stop()
        print("game stopped")

    if failures:
        print("FAIL:", failures)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
