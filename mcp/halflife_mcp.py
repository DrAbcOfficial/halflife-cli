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
from typing import Annotated, Literal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp.server import MCPServer
from mcp.server.mcpserver import Image
from mcp.types import ToolAnnotations
from pydantic import Field

from game_process import BANNER_TIMEOUT_S, QUIT_TIMEOUT_S
from halflifecli.manager import (
    DEFAULT_COMMAND_MAX_LINES,
    DEFAULT_HOLD_MS,
    INT32_MAX,
    INT32_MIN,
    LAUNCH_TIMEOUT_CAP_S,
    MAX_HOLD_MS,
    MOUSE_BUTTONS_MASK,
    QUIT_TIMEOUT_CAP_S,
    Manager,
)
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
    "Call launch_game first: Sven Co-op (225840) is found without help, any "
    "other app needs its appid (10 Counter-Strike, 70 Half-Life) so "
    "MetahookInstallerCLI can report its mod and launcher. After `map <name>`, "
    "use read_console to wait for "
    "the load to finish before snapshot. Before snapshot, make sure a map is "
    "loaded and rendering (the main menu does not render the world). Verify "
    "cvar/command names with find_cvar before running them. Call quit_game when "
    "done. Use run_command for everything else; its output is capped at "
    "max_lines (default 200) and the full text of a capped command is saved to "
    "the file path it returns, so grep that file instead of raising the cap. "
    "To press keys or mouse buttons in the game use send_key / send_mouse. "
    "For UI pointing use move_mouse, or send_mouse with x/y, measured in "
    "snapshot's original_size pixels (returned_size describes the scaled image); "
    "relative motion moves the UI cursor, not the view. Inputs "
    "enter through the engine's own input path, work with the window hidden or "
    "block_input on, and never touch other windows. Do not simulate input with "
    "Win32 SendInput / keybd_event / mouse_event or posted window messages. "
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
    appid: Annotated[int | None, Field(description="Steam app ID (225840 Sven Co-op, 10 Counter-Strike, 70 Half-Life). Any other app is described by MetahookInstallerCLI", ge=1)] = None,
    mod: Annotated[str | None, Field(description="Mod directory under the game root, e.g. cstrike; empty uses the app's default mod")] = None,
    timeout_s: Annotated[int, Field(description="Seconds to wait for the RCON banner", ge=1, le=LAUNCH_TIMEOUT_CAP_S)] = BANNER_TIMEOUT_S,
) -> GameStatus:
    """Launch the game (if not already running) and wait for it to be drivable.

    Sven Co-op is the default target. Pass the appid of another GoldSrc app
    (and optionally its mod directory) to launch that one instead.
    """
    return manager.launch_game(extra_args, game_dir, timeout_s, appid, mod)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def game_status() -> GameStatus:
    """Report whether the game is running and how this server is connected to it."""
    return manager.game_status()


@mcp.tool()
def run_command(
    command: Annotated[str, Field(description="Console command or cvar assignment, e.g. 'status', 'sv_cheats 1'")],
    max_lines: Annotated[int, Field(description="Cap on returned lines so a chatty command cannot flood context; -1 keeps every line, 0 returns only the saved-file marker. When lines are dropped, the full output is saved to a file whose path is returned.", ge=-1)] = DEFAULT_COMMAND_MAX_LINES,
    keep: Annotated[Literal["head", "tail", "both"], Field(description="Which lines to keep when max_lines is hit: head = first lines, tail = last lines, both = first and last halves")] = "head",
) -> str:
    """Run one console command over RCON and return the captured output."""
    return manager.run_command(command, max_lines, keep)


@mcp.tool(annotations=ToolAnnotations(read_only_hint=True))
def find_cvar(
    name: Annotated[str, Field(description="cvar or command name to check")],
) -> str:
    """Check whether a cvar/command exists (cli.find); suggests similar names if not."""
    return manager.find_cvar(name)


@mcp.tool()
def send_key(
    key: Annotated[str, Field(description="Engine key name as used by `bind` (w, SPACE, ENTER, ESCAPE, TAB, SHIFT, CTRL, F1, UPARROW, MOUSE1, MWHEELUP, SEMICOLON, ...), a single character, or a keynum 0-255")],
    action: Annotated[Literal["tap", "press", "release"], Field(description="tap = press, hold for hold_ms, release; press/release send one edge (e.g. hold +forward across other calls)")] = "tap",
    hold_ms: Annotated[int, Field(description="How long a tap keeps the key down", ge=0, le=MAX_HOLD_MS)] = DEFAULT_HOLD_MS,
) -> str:
    """Inject through SDL_PushEvent or the original CGame::WindowProc, bypassing block_input. Keys need a native mapping; mouse keys share send_mouse's cursor/button state. Wheel press is a pulse; release is a no-op."""
    return manager.send_key(key, action, hold_ms)


@mcp.tool()
def move_mouse(
    x: Annotated[int, Field(description="X position or delta in original screenshot pixels", strict=True, ge=INT32_MIN, le=INT32_MAX)],
    y: Annotated[int, Field(description="Y position or delta in original screenshot pixels", strict=True, ge=INT32_MIN, le=INT32_MAX)],
    mode: Annotated[Literal["absolute", "relative"], Field(description="absolute: from the game image's top left; relative: add x/y to the current cursor position")] = "absolute",
) -> str:
    """Move the UI cursor through SDL events (SDL2 engines), clamp to the image and return its absolute x/y. Use snapshot's original_size for coordinates; relative motion moves the UI cursor."""
    return manager.move_mouse(x, y, mode)


@mcp.tool()
def send_mouse(
    buttons: Annotated[int, Field(description="Mouse buttons to press: bitmask 1=left 2=right 4=middle 8=mouse4 16=mouse5", ge=1, le=MOUSE_BUTTONS_MASK)],
    action: Annotated[Literal["tap", "press", "release"], Field(description="tap = press, hold for hold_ms, release; release lifts every button")] = "tap",
    hold_ms: Annotated[int, Field(description="How long a tap keeps the buttons down", ge=0, le=MAX_HOLD_MS)] = DEFAULT_HOLD_MS,
    x: Annotated[int | None, Field(description="Optional X position/delta in original screenshot pixels; provide both x and y", strict=True, ge=INT32_MIN, le=INT32_MAX)] = None,
    y: Annotated[int | None, Field(description="Optional Y position/delta in original screenshot pixels; provide both x and y", strict=True, ge=INT32_MIN, le=INT32_MAX)] = None,
    mode: Annotated[Literal["absolute", "relative"], Field(description="Coordinate mode when x/y are provided")] = "absolute",
) -> str:
    """Optionally move the UI cursor, then inject native mouse-button transitions in one serialized call, bypassing block_input. The held mask is authoritative; release lifts every button. Coordinates use snapshot's original_size and are echoed after moving."""
    return manager.send_mouse(buttons, action, hold_ms, x, y, mode)


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
    """Capture an in-game screenshot (engine `screenshot` command) as a PNG plus original_size=(w,h) and returned_size=(w,h). Mouse coordinates use original pixels; scale displayed coordinates by original_size / returned_size."""
    return manager.snapshot(max_edge)


@mcp.tool()
def quit_game(
    timeout_s: Annotated[int, Field(description="Seconds to wait for a clean exit before killing", ge=1, le=QUIT_TIMEOUT_CAP_S)] = QUIT_TIMEOUT_S,
) -> str:
    """Quit the game cleanly (RCON, then stdin, then kill as a last resort).

    Returns only after the game process has exited, so a following launch_game
    cannot collide with the previous game's single-instance lock."""
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
