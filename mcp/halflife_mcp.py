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
"""

import contextlib
import glob
import io
import logging
import os
import sys
import threading
import time
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rcon_client
from find_game import resolve_game_dir
from game_process import (
    BANNER_TIMEOUT_S,
    EXIT_CODE_GRACE_S,
    QUIT_TIMEOUT_S,
    GameProcess,
    RconStartError,
    build_game_argv,
    mod_dir,
    plugin_config_path,
    port_file_path,
    rcon_connect_host,
    screenshots_dir,
)

from mcp.server import MCPServer
from mcp.server.mcpserver import Image
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import BaseModel, Field
from typing import Annotated

logging.basicConfig(
    level=logging.INFO,
    stream=sys.stderr,
    format="%(asctime)s %(name)s %(levelname)s %(message)s",
)
log = logging.getLogger("halflife_mcp")

DEFAULT_IMAGE_MAX_EDGE = 1280
SNAPSHOT_TIMEOUT_S = 10
SNAPSHOT_POLL_S = 0.5
RCON_BODY_MAX = rcon_client.MAX_BODY
LAUNCH_TIMEOUT_CAP_S = 600
QUIT_TIMEOUT_CAP_S = 300
# The local-file screenshot command writes one of these, depending on engine
# and mod config (svencoop's `screenshot` writes .tga; `snapshot` is taken over
# by SteamScreenshots.dll and uploads to Steam instead of writing a file).
IMAGE_EXTENSIONS = (".bmp", ".tga", ".png", ".jpg", ".jpeg")
CLI_FIND_PREFIX = "cli.find:"

INSTRUCTIONS = (
    "Drive Sven Co-op / Half-Life through halflife-cli. "
    "Call launch_game first. After `map <name>`, use read_console to wait for "
    "the load to finish before snapshot. Before snapshot, make sure a map is "
    "loaded and rendering (the main menu does not render the world). Verify "
    "cvar/command names with find_cvar before running them. Call quit_game when "
    "done. Use run_command for everything else."
)


class GameStatus(BaseModel):
    running: bool
    pid: int | None = None
    mode: str  # "managed" (started by this server), "attached", or "none"
    host: str | None = None
    port: int | None = None
    exit_code: int | None = None
    game_dir: str | None = None


class ConsoleWindow(BaseModel):
    lines: list[str]
    next_cursor: int
    dropped: int = 0
    note: str | None = None


# ---------------------------------------------------------------------------
# Pure helpers (unit-testable without a game)
# ---------------------------------------------------------------------------

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
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def find_new_screenshot(before, directory, timeout, poll=SNAPSHOT_POLL_S):
    """Return the newest image file in `directory` that appeared after `before`.

    The engine writes the file asynchronously, so a candidate is only returned
    once its size is stable across one poll interval. Only image files count.
    Raises TimeoutError when nothing appears in time.
    """
    def scan():
        return {
            os.path.abspath(p)
            for p in glob.glob(os.path.join(directory, "*"))
            if p.lower().endswith(IMAGE_EXTENSIONS)
        }

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new = scan() - before
        if new:
            candidate = max(new, key=os.path.getmtime)
            size = os.path.getsize(candidate)
            time.sleep(poll)
            if os.path.getsize(candidate) == size:
                return candidate
        else:
            time.sleep(poll)
    raise TimeoutError("no new screenshot appeared within %ss" % timeout)


def shot_to_png(path, max_edge):
    """Decode a screenshot (BMP/TGA/...), optionally downscale, return (png, w, h)."""
    from PIL import Image as PILImage  # imported lazily so pure tests skip Pillow

    with PILImage.open(path) as img:
        img = img.convert("RGB")
        if max_edge and max(img.size) > max_edge:
            img.thumbnail((max_edge, max_edge))
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        return buf.getvalue(), img.width, img.height


# ---------------------------------------------------------------------------
# Runtime state
# ---------------------------------------------------------------------------

class Manager:
    """Owns the managed game process and lazily attaches to an external one."""

    def __init__(self):
        self._proc = None          # GameProcess, set while this server owns the game
        self._game_dir = None
        self._rcon = None          # RconConnection for the managed game
        self._host = None
        self._port = None
        self._password = ""
        self._attached_rcon = None
        self._attached_key = None

    # -- helpers ------------------------------------------------------------

    def _probe_attached(self):
        """Detect an externally launched game; returns GameStatus or None.

        Caller holds the lock. A game counts as attached only when its port
        file exists and its RCON server answers an auth handshake.
        """
        if os.environ.get("HALFLIFE_DISABLE_ATTACH") == "1":
            return None
        gd, _ = resolve_game_dir(None)
        if not gd:
            return None
        port = read_port_file(gd)
        if port is None:
            return None
        host = rcon_connect_host(plugin_rcon_bind(gd))
        password = plugin_rcon_password(gd)
        try:
            rcon_client.RconConnection(host, port, password).open()
        except Exception:
            return None
        return GameStatus(running=True, pid=None, mode="attached",
                          host=host, port=port, game_dir=gd)

    def _status_locked(self):
        if self._proc is not None and self._proc.alive:
            return GameStatus(running=True, pid=self._proc.pid, mode="managed",
                              host=self._host, port=self._port, game_dir=self._game_dir)
        if self._proc is not None:
            return GameStatus(running=False, pid=self._proc.pid, mode="managed",
                              exit_code=self._proc.returncode, game_dir=self._game_dir)
        return self._probe_attached() or GameStatus(running=False, mode="none")

    def _connection_locked(self):
        """RconConnection for the current game; raises ToolError when none."""
        if self._proc is not None and self._proc.alive:
            if self._rcon is None:
                self._rcon = rcon_client.RconConnection(self._host, self._port, self._password)
            return self._rcon
        attached = self._probe_attached()
        if attached is None:
            raise ToolError("no game running; call launch_game first")
        key = (attached.game_dir, attached.host, attached.port)
        if self._attached_rcon is None or self._attached_key != key:
            self._attached_rcon = rcon_client.RconConnection(
                attached.host, attached.port, plugin_rcon_password(attached.game_dir))
            self._attached_key = key
        return self._attached_rcon

    # -- tools --------------------------------------------------------------

    def launch_game(self, extra_args, game_dir, timeout_s):
        timeout_s = min(max(timeout_s or BANNER_TIMEOUT_S, 1), LAUNCH_TIMEOUT_CAP_S)
        with _session_lock:
            if self._proc is not None:
                if self._proc.alive:
                    return self._status_locked()
                self._proc = None
            attached = self._probe_attached()
            if attached is not None:
                raise ToolError(
                    "a game is already running (started outside this server) at "
                    f"{attached.host}:{attached.port}. Use it directly, or close it first.")
            gd, source = resolve_game_dir(game_dir)
            if not gd:
                raise ToolError(f"no game directory resolved ({source})")
            proc = GameProcess(build_game_argv(gd, extra_args), cwd=gd)
            proc.start()
            try:
                host, port = proc.wait_for_banner(timeout_s)
            except RconStartError as e:
                proc.stop(timeout=EXIT_CODE_GRACE_S)
                raise ToolError(str(e))
            except Exception:
                proc.stop(timeout=EXIT_CODE_GRACE_S)
                raise
            self._proc = proc
            self._game_dir = gd
            self._host = host
            self._port = port
            self._password = plugin_rcon_password(gd)
            self._rcon = None  # connect lazily on the first command
            return self._status_locked()

    def game_status(self):
        with _session_lock:
            return self._status_locked()

    def run_command(self, command):
        cmd = (command or "").strip()
        if not cmd:
            raise ToolError("empty command")
        with _session_lock:
            conn = self._connection_locked()
        try:
            out = conn.command(cmd)
        except ValueError as e:
            raise ToolError(str(e))
        except Exception as e:
            with _session_lock:
                exit_code = self._proc.returncode if self._proc else None
            if exit_code is not None:
                raise ToolError(f"game is no longer running (exit code {exit_code}): {e}")
            raise ToolError(f"RCON command failed: {e}")
        return out.strip() or "(no output)"

    def find_cvar(self, name):
        cmd = "cli.find %s" % (name or "").strip().replace("\n", " ")
        out = self.run_command(cmd)
        # The RCON reply carries every console line printed in that frame
        # (texture loads, etc.); keep only cli.find's own lines when present.
        own = [ln for ln in out.splitlines() if ln.startswith(CLI_FIND_PREFIX)]
        return "\n".join(own) if own else out

    def read_console(self, max_lines, cursor):
        with _session_lock:
            proc = self._proc
        if proc is None:
            return ConsoleWindow(lines=[], next_cursor=cursor or 0,
                                 note="no game launched by this server (attached games expose no console stream)")
        if cursor is None:
            cursor = proc.next_cursor
        sl = proc.lines_since(cursor, max_lines)
        return ConsoleWindow(lines=sl.lines, next_cursor=sl.next_cursor, dropped=sl.dropped)

    def snapshot(self, max_edge):
        with _session_lock:
            if self._proc is not None and self._proc.alive:
                conn = self._connection_locked()
                game_dir = self._game_dir
            else:
                attached = self._probe_attached()
                if attached is None:
                    raise ToolError("no game running; call launch_game first")
                key = (attached.game_dir, attached.host, attached.port)
                if self._attached_rcon is None or self._attached_key != key:
                    self._attached_rcon = rcon_client.RconConnection(
                        attached.host, attached.port, plugin_rcon_password(attached.game_dir))
                    self._attached_key = key
                conn = self._attached_rcon
                game_dir = attached.game_dir
        shots_dir = screenshots_dir(game_dir)
        os.makedirs(shots_dir, exist_ok=True)
        before = {
            os.path.abspath(p)
            for p in glob.glob(os.path.join(shots_dir, "*"))
            if p.lower().endswith(IMAGE_EXTENSIONS)
        }
        # `snapshot` is taken over by SteamScreenshots.dll (uploads to Steam, no
        # local file); the engine's own `screenshot` command writes a local file.
        try:
            conn.command("screenshot")
        except Exception as e:
            raise ToolError(f"RCON screenshot command failed: {e}")
        try:
            path = find_new_screenshot(before, shots_dir, SNAPSHOT_TIMEOUT_S)
        except TimeoutError:
            raise ToolError("no new screenshot appeared; is a map loaded and rendering? (off-screen mode 1)")
        try:
            png, width, height = shot_to_png(path, max_edge)
        except Exception as e:
            raise ToolError(f"screenshot could not be read: {e}")
        return [Image(data=png, format="png"), f"saved: {path} ({width}x{height})"]

    def quit_game(self, timeout_s):
        timeout_s = min(max(timeout_s or QUIT_TIMEOUT_S, 1), QUIT_TIMEOUT_CAP_S)
        with _session_lock:
            proc = self._proc
            if proc is None or not proc.alive:
                try:
                    conn = self._connection_locked()
                except ToolError:
                    return "no game running; nothing to quit"
                try:
                    conn.command("quit")
                except Exception as e:
                    # an exiting game often drops the connection before replying
                    return f"quit sent to attached game over RCON (no reply: {e})"
                return "quit sent to attached game over RCON"
            try:
                conn = self._connection_locked()
            except Exception:
                conn = None
            rcon_quit = (lambda: conn.command("quit")) if conn is not None else None
        result = proc.stop(quit_command=rcon_quit, timeout=timeout_s)
        return result.summary

    def shutdown(self):
        """Stop the game this server launched; never touches attached games."""
        with _session_lock:
            proc = self._proc
            conn = self._rcon
        if proc is None or not proc.alive:
            return
        rcon_quit = (lambda: conn.command("quit")) if conn is not None else None
        log.info("stopping managed game: %s", proc.stop(quit_command=rcon_quit).summary)


manager = Manager()
# Tools run on AnyIO worker threads, so the session state below needs guarding.
_session_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

@contextlib.asynccontextmanager
async def lifespan(_server):
    try:
        yield
    finally:
        try:
            manager.shutdown()
        except Exception:
            log.exception("shutdown cleanup failed")


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


if __name__ == "__main__":
    mcp.run(transport="stdio")
