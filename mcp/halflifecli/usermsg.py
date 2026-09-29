"""UserMsg monitor support: console-output parsers and schema file loading.

The parsers mirror the plugin's `cli.usermsg` output format (src/usermsg/
usermsg_monitor.cpp); the schema loader mirrors the plugin's extends-chain
merge (src/usermsg/usermsg_schema.cpp).
"""

import glob
import os
import re
import shutil
import tomllib

from mcp.server.mcpserver.exceptions import ToolError

from game_process import mod_dir
from halflifecli.models import UserMsgEvent, UserMsgEvents, UserMsgMessages, UserMsgStatus
from halflifecli.plugin_config import read_plugin_config

# "cli.usermsg: schema=svencoop.toml coord_size=4 messages=101 display=on
#  hooks: 85 wrapped, 16 self-registered, 0 pending"
USERMSG_STATUS_RE = re.compile(
    r"cli\.usermsg: schema=(\S+) coord_size=(\d+) messages=(\d+) display=(on|off) "
    r"hooks: (\d+) wrapped, (\d+) self-registered, (\d+) pending", re.MULTILINE)

# "#17 [usermsg] CurWeapon size=5 state=1 weaponId=18 clip=35"
USERMSG_EVENT_RE = re.compile(r"#(\d+) \[usermsg\] (\S+) size=(\d+)(?: (.*))?$", re.MULTILINE)
USERMSG_EVENTS_NEWEST_RE = re.compile(
    r"cli\.usermsg: events channel=(\S+) newest=(\d+) name=(.*)$", re.MULTILINE)
USERMSG_EVENTS_MORE_RE = re.compile(r"cli\.usermsg: (\d+) more after #\d+", re.MULTILINE)


def parse_usermsg_status(text):
    """UserMsgStatus from `cli.usermsg` output, or None when unrecognized."""
    m = USERMSG_STATUS_RE.search(text)
    if not m:
        return None
    return UserMsgStatus(
        schema_file=m.group(1), coord_size=int(m.group(2)), messages=int(m.group(3)),
        display=m.group(4) == "on", wrapped=int(m.group(5)),
        self_registered=int(m.group(6)), pending=int(m.group(7)))


def parse_usermsg_events(text):
    """(events, newest_seq, more_after|None) from `cli.usermsg events` output."""
    newest = 0
    m = USERMSG_EVENTS_NEWEST_RE.search(text)
    if m:
        newest = int(m.group(2))
    events = [UserMsgEvent(seq=int(m.group(1)), name=m.group(2), size=int(m.group(3)),
                           detail=m.group(4) or "")
              for m in USERMSG_EVENT_RE.finditer(text)]
    more = None
    m = USERMSG_EVENTS_MORE_RE.search(text)
    if m:
        more = int(m.group(1))
    return events, newest, more


def usermsg_schema_dir(game_dir):
    return os.path.join(mod_dir(game_dir), "metahook", "configs", "usermsgs")


def usermsg_schema_file(game_dir):
    """Schema file name the plugin loads: [usermsg] file, else <moddir>.toml."""
    override = read_plugin_config(game_dir).get("usermsg", {}).get("file", "")
    return override or (os.path.basename(mod_dir(game_dir)) + ".toml")


def load_usermsg_schema(game_dir):
    """Merged UserMsgMessages for the game's schema, following extends chains.

    Reads the same files the plugin loads (mod/metahook/configs/usermsgs/);
    base files load first so the child's definitions win, like the plugin.
    Raises ToolError when the schema file is missing or unparsable.
    """
    schema_dir = usermsg_schema_dir(game_dir)
    root_file = usermsg_schema_file(game_dir)

    merged = {}
    coord_size = 2
    seen = set()

    def load_file(fname):
        """Load one file after its extends chain; returns its own extends."""
        nonlocal coord_size
        key = fname.lower()
        if key in seen:
            raise ToolError(f"usermsg schema extends cycle at {fname}")
        seen.add(key)
        path = os.path.join(schema_dir, fname)
        try:
            with open(path, "rb") as f:
                data = tomllib.load(f)
        except OSError as e:
            raise ToolError(f"usermsg schema {fname} not found in {schema_dir} ({e})")
        except tomllib.TOMLDecodeError as e:
            raise ToolError(f"usermsg schema {fname} is not valid TOML: {e}")
        parent = data.get("extends")
        if parent:
            load_file(parent)  # base definitions first, child overrides below
        for msg in data.get("usermsg", []):
            if isinstance(msg, dict) and msg.get("name"):
                merged[msg["name"]] = msg
        coord_size = data.get("primitives", {}).get("coord_size", coord_size)
        return parent

    extends = load_file(root_file)
    return UserMsgMessages(
        schema_file=root_file, extends=extends, coord_size=coord_size,
        messages=[merged[name] for name in sorted(merged)])


def sync_usermsg_schemas(game_dir, source_dir):
    """Copy the repo's schema TOMLs into the game's usermsgs dir. Returns count."""
    os.makedirs(usermsg_schema_dir(game_dir), exist_ok=True)
    n = 0
    for path in glob.glob(os.path.join(source_dir, "*.toml")):
        shutil.copy2(path, usermsg_schema_dir(game_dir))
        n += 1
    return n
