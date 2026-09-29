# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "mcp>=2.2,<3",
#     "pillow>=10",
#     "pywin32>=311; sys_platform == 'win32'",
# ]
# ///
"""MCP server that drives Sven Co-op / Half-Life through halflife-cli.

Transport is stdio. The game is launched with piped stdio and driven over the
plugin's Source RCON server; a game started by someone else is attached to by
reading the plugin's port file. Only the game process may write to the MCP
server's stdout: all our diagnostics go to stderr via the logging module.

This file is the composition root only: tool signatures, descriptions and
defaults. The implementation lives in the halflifecli package
(models, plugin_config, screenshot, usermsg, manager).
"""

import contextlib
import logging
import os
import sys
from typing import Annotated

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp.server import MCPServer
from mcp.server.mcpserver import Image
from mcp.types import ToolAnnotations
from pydantic import Field

from game_process import BANNER_TIMEOUT_S, QUIT_TIMEOUT_S
from halflifecli.manager import LAUNCH_TIMEOUT_CAP_S, QUIT_TIMEOUT_CAP_S, Manager
from halflifecli.models import (
    ConsoleWindow,
    GameStatus,
    UserMsgEvents,
    UserMsgMessages,
    UserMsgStatus,
)

logging.basicConfig(
    level=logging.INFO,
    stream=sys.stderr,
    format="%(asctime)s %(name)s %(levelname)s %(message)s",
)

DEFAULT_IMAGE_MAX_EDGE = 1280

INSTRUCTIONS = (
    "Drive Sven Co-op / Half-Life through halflife-cli. "
    "Call launch_game first. After `map <name>`, use read_console to wait for "
    "the load to finish before snapshot. Before snapshot, make sure a map is "
    "loaded and rendering (the main menu does not render the world). Verify "
    "cvar/command names with find_cvar before running them. Call quit_game when "
    "done. Use run_command for everything else. "
    "The usermsg_* tools monitor server user messages: usermsg_events returns "
    "decoded network traffic (page with since_seq=usermsg_events(...).newest_seq, "
    "filter by channel or name), usermsg_messages shows the schema's message "
    "layouts, usermsg_status reports hook health. User messages only flow while "
    "connected to a server."
)

manager = Manager()


@contextlib.asynccontextmanager
async def lifespan(_server):
    try:
        yield
    finally:
        try:
            manager.shutdown()
        except Exception:
            logging.getLogger("halflife_mcp").exception("shutdown cleanup failed")


mcp = MCPServer("halflife", instructions=INSTRUCTIONS, lifespan=lifespan)


@mcp.tool()
def launch_game(
    extra_args: Annotated[list[str], Field(description="Extra game launch arguments (windowed and -novid are always added)", default_factory=list)],
    game_dir: Annotated[str | None, Field(description="Game install directory; resolved automatically when omitted")] = None,
    timeout_s: Annotated[int, Field(description="Seconds to wait for the RCON banner", ge=1, le=LAUNCH_TIMEOUT_CAP_S)] = BANNER_TIMEOUT_S,
) -> GameStatus:
    """Launch the game (if not already running) and wait for it to be drivable."""
    return manager.launch_game(extra_args, game_dir, timeout_s)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def game_status() -> GameStatus:
    """Report whether the game is running and how this server is connected to it."""
    return manager.game_status()


@mcp.tool()
def run_command(
    command: Annotated[str, Field(description="Console command or cvar assignment, e.g. 'status', 'sv_cheats 1'")],
) -> str:
    """Run one console command over RCON and return the captured output."""
    return manager.run_command(command)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def find_cvar(
    name: Annotated[str, Field(description="cvar or command name to check")],
) -> str:
    """Check whether a cvar/command exists (cli.find); suggests similar names if not."""
    return manager.find_cvar(name)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def read_console(
    max_lines: Annotated[int, Field(description="Maximum lines to return", ge=1, le=1000)] = 200,
    cursor: Annotated[int | None, Field(description="Resume after this line number; omit for the newest lines")] = None,
) -> ConsoleWindow:
    """Read captured game console output; only available for games this server launched."""
    return manager.read_console(max_lines, cursor)


@mcp.tool()
def snapshot(
    max_edge: Annotated[int, Field(description="Downscale so the longest side is this many px; 0 keeps the original size", ge=0, le=4096)] = DEFAULT_IMAGE_MAX_EDGE,
) -> list:
    """Capture an in-game screenshot (engine `screenshot` command) and return it as a PNG image."""
    return manager.snapshot(max_edge)


@mcp.tool()
def quit_game(
    timeout_s: Annotated[int, Field(description="Seconds to wait for a clean exit before killing", ge=1, le=QUIT_TIMEOUT_CAP_S)] = QUIT_TIMEOUT_S,
) -> str:
    """Quit the game cleanly (RCON, then stdin, then kill as a last resort)."""
    return manager.quit_game(timeout_s)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def usermsg_status() -> UserMsgStatus:
    """Report the UserMsg monitor state: loaded schema, coord size, hook counts."""
    return manager.usermsg_status()


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def usermsg_messages() -> UserMsgMessages:
    """Load the merged UserMsg schema from the game install: message names, field layouts, notes."""
    return manager.usermsg_messages()


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def usermsg_events(
    since_seq: Annotated[int | None, Field(description="Return events with seq greater than this; omit for the newest page")] = None,
    limit: Annotated[int, Field(description="Maximum events per page", ge=1, le=200)] = 50,
    name: Annotated[str | None, Field(description="Only events of this message name (case-insensitive)")] = None,
    channel: Annotated[str | None, Field(description="Only events of this channel (functional group: weapon, status, text, score, screen, world, hud, meta); omit for all channels")] = None,
) -> UserMsgEvents:
    """Read decoded user messages the game received; page forward with since_seq=newest_seq."""
    return manager.usermsg_events(since_seq, limit, name, channel)


@mcp.tool()
def usermsg_set_display(
    enabled: Annotated[bool, Field(description="True prints a [usermsg] line to the console per message; recording is unaffected")],
) -> str:
    """Toggle live [usermsg] console printing of user messages."""
    return manager.usermsg_set_display(enabled)


@mcp.tool()
def usermsg_reload_schema(
    sync_from_repo: Annotated[bool, Field(description="Copy configs/usermsgs/*.toml from the halflife-cli repo into the game install before reloading")] = True,
) -> str:
    """Reload the UserMsg schema TOML (optionally syncing it from the repo first)."""
    return manager.usermsg_reload_schema(sync_from_repo)


if __name__ == "__main__":
    mcp.run(transport="stdio")
