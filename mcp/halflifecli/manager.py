"""The game session state machine behind the MCP tools.

Manager owns the game process this server launched ("managed") and lazily
attaches to an externally started one by probing the plugin's port file.
All tool calls run under the instance lock: tools execute on AnyIO worker
threads.
"""

import glob
import logging
import os
import threading
import time

import rcon_client
from find_game import resolve_game_dir
from game_process import (
    BANNER_TIMEOUT_S,
    EXIT_CODE_GRACE_S,
    IMAGE_EXTENSIONS,
    QUIT_TIMEOUT_S,
    GameProcess,
    RconStartError,
    build_game_argv,
    rcon_connect_host,
    screenshots_dir,
)
from mcp.server.mcpserver import Image
from mcp.server.mcpserver.exceptions import ToolError

from halflifecli.models import ConsoleWindow, GameStatus, UserMsgEvents
from halflifecli.plugin_config import plugin_rcon_password, read_endpoint_file
from halflifecli.screenshot import find_new_screenshot, shot_to_png
from halflifecli.usermsg import (
    load_usermsg_schema,
    parse_usermsg_events,
    parse_usermsg_status,
    sync_usermsg_schemas,
)

log = logging.getLogger(__name__)

LAUNCH_TIMEOUT_CAP_S = 600
QUIT_TIMEOUT_CAP_S = 300
SNAPSHOT_TIMEOUT_S = 10
CLI_FIND_PREFIX = "cli.find:"
INPUT_ACTIONS = ("tap", "press", "release")
DEFAULT_HOLD_MS = 100
MAX_HOLD_MS = 5000
MOUSE_BUTTONS_MASK = 0x1F  # left, right, middle, mouse4, mouse5


class Manager:
    """Owns the managed game process and lazily attaches to an external one."""

    def __init__(self):
        self._lock = threading.Lock()
        self._proc = None          # GameProcess, set while this server owns the game
        self._game_dir = None
        self._rcon = None          # RconConnection for the managed game
        self._host = None
        self._port = None
        self._password = ""
        self._protocol = "source-tcp"
        self._endpoint = None
        self._attached_rcon = None
        self._attached_key = None

    # -- helpers (caller holds the lock) ------------------------------------

    def _probe_attached(self):
        """Detect an externally launched game; returns GameStatus or None.

        A game counts as attached only when its port file exists and its RCON
        server answers an auth handshake.
        """
        if os.environ.get("HALFLIFE_DISABLE_ATTACH") == "1":
            return None
        gd, _ = resolve_game_dir(None)
        if not gd:
            return None
        endpoint = read_endpoint_file(gd)
        if endpoint is None:
            return None
        password = plugin_rcon_password(gd)
        key = (gd, endpoint, password)
        if self._attached_key != key:
            if self._attached_rcon is not None:
                self._attached_rcon.close()
            self._attached_rcon = rcon_client.RconConnection(
                endpoint.host, endpoint.port, password, timeout=2, protocol=endpoint.protocol)
            self._attached_key = key
        try:
            self._attached_rcon.open()
        except Exception:
            self._attached_rcon.close()
            return None
        return GameStatus(running=True, pid=endpoint.pid, mode="attached",
                          host=endpoint.host, port=endpoint.port, protocol=endpoint.protocol, game_dir=gd)

    def _refresh_managed_endpoint_locked(self):
        if self._endpoint is None and self._protocol == "source-tcp":
            return  # legacy launch / injected test session
        endpoint = read_endpoint_file(self._game_dir, expected_pid=self._proc.pid)
        if endpoint is None:
            raise ToolError("game RCON endpoint is not ready or belongs to a stale process")
        if endpoint != self._endpoint:
            if self._rcon is not None:
                self._rcon.close()
            self._rcon = None
            self._endpoint = endpoint
            self._host, self._port, self._protocol = endpoint.host, endpoint.port, endpoint.protocol

    def _status_locked(self):
        if self._proc is not None and self._proc.alive:
            return GameStatus(running=True, pid=self._proc.pid, mode="managed",
                              host=self._host, port=self._port, protocol=self._protocol, game_dir=self._game_dir)
        if self._proc is not None:
            return GameStatus(running=False, pid=self._proc.pid, mode="managed",
                              exit_code=self._proc.returncode, game_dir=self._game_dir)
        return self._probe_attached() or GameStatus(running=False, mode="none")

    def _connection_locked(self):
        """RconConnection for the current game; raises ToolError when none."""
        if self._proc is not None and self._proc.alive:
            self._refresh_managed_endpoint_locked()
            if self._rcon is None:
                self._rcon = rcon_client.RconConnection(self._host, self._port, self._password, protocol=self._protocol)
            return self._rcon
        attached = self._probe_attached()
        if attached is None:
            raise ToolError("no game running; call launch_game first")
        return self._attached_rcon

    def _game_dir_locked(self):
        """Install dir of the running game (managed or attached)."""
        if self._proc is not None and self._proc.alive and self._game_dir:
            return self._game_dir
        attached = self._probe_attached()
        if attached is not None:
            return attached.game_dir
        raise ToolError("no game running; call launch_game first")

    # -- tools ---------------------------------------------------------------

    def launch_game(self, extra_args, game_dir, timeout_s):
        timeout_s = min(max(timeout_s or BANNER_TIMEOUT_S, 1), LAUNCH_TIMEOUT_CAP_S)
        with self._lock:
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
                protocol = proc.rcon_protocol
                endpoint = read_endpoint_file(gd, expected_pid=proc.pid)
                if protocol == "goldsrc-udp" and (endpoint is None or endpoint.protocol != protocol):
                    raise RconStartError("Native UDP banner has no fresh matching endpoint metadata")
                if endpoint is not None:
                    host, port, protocol = endpoint.host, endpoint.port, endpoint.protocol
                password = plugin_rcon_password(gd)
                connection = rcon_client.RconConnection(host, port, password, protocol=protocol)
                try:
                    connection.open()
                except Exception:
                    connection.close()
                    raise
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
            self._protocol = protocol
            self._endpoint = endpoint
            self._password = password
            if self._rcon is not None:
                self._rcon.close()
            self._rcon = connection
            return self._status_locked()

    def game_status(self):
        with self._lock:
            if self._proc is not None and self._proc.alive:
                self._refresh_managed_endpoint_locked()
            return self._status_locked()

    def run_command(self, command):
        cmd = (command or "").strip()
        if not cmd:
            raise ToolError("empty command")
        with self._lock:
            conn = self._connection_locked()
        try:
            out = conn.command(cmd)
        except ValueError as e:
            raise ToolError(str(e))
        except Exception as e:
            with self._lock:
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

    def _trap_command(self, command):
        """Run a cli.trapkey / cli.trapmouse command; plugin-reported failures become ToolErrors."""
        prefix = command.split(" ", 1)[0]
        out = self.run_command(command)
        own = [ln for ln in out.splitlines() if ln.startswith((prefix + ":", "usage: " + prefix))]
        if any(ln.startswith((prefix + ": error:", "usage:")) for ln in own):
            raise ToolError("\n".join(own))
        # Without console capture the reply is empty even though the event went in.
        return "\n".join(own) if own else out

    def _trap_sequence(self, press, release, action, hold_ms):
        """press / release / tap (press, hold, release) through _trap_command."""
        if action not in INPUT_ACTIONS:
            raise ToolError(f"action must be one of {', '.join(INPUT_ACTIONS)}")
        replies = []
        if action in ("press", "tap"):
            replies.append(self._trap_command(press))
        if action == "tap":
            time.sleep(min(max(hold_ms, 0), MAX_HOLD_MS) / 1000.0)
        if action in ("release", "tap"):
            replies.append(self._trap_command(release))
        return "\n".join(replies)

    def send_key(self, key, action, hold_ms):
        key = (key or "").strip()
        # One console token: whitespace, ';' and '"' would split or quote the command.
        if not key or any(c.isspace() or c in ';"' for c in key):
            raise ToolError("key must be one engine key name or character, e.g. w, SPACE, ENTER, MOUSE1, SEMICOLON")
        return self._trap_sequence(f"cli.trapkey {key} 1", f"cli.trapkey {key} 0", action, hold_ms)

    def send_mouse(self, buttons, action, hold_ms):
        buttons = int(buttons)
        if not 0 < buttons <= MOUSE_BUTTONS_MASK:
            raise ToolError(f"buttons must be a mask in 1..{MOUSE_BUTTONS_MASK} (1=left 2=right 4=middle 8=mouse4 16=mouse5)")
        # buttons is the held mask after the event, so release reports none held.
        return self._trap_sequence(f"cli.trapmouse {buttons} 1", "cli.trapmouse 0 0", action, hold_ms)

    def read_console(self, max_lines, cursor):
        with self._lock:
            proc = self._proc
        if proc is None:
            return ConsoleWindow(lines=[], next_cursor=cursor or 0,
                                 note="no game launched by this server (attached games expose no console stream)")
        if cursor is None:
            cursor = proc.next_cursor
        sl = proc.lines_since(cursor, max_lines)
        return ConsoleWindow(lines=sl.lines, next_cursor=sl.next_cursor, dropped=sl.dropped)

    def snapshot(self, max_edge):
        with self._lock:
            conn = self._connection_locked()
            game_dir = self._game_dir_locked()
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
        with self._lock:
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

    # -- usermsg tools -------------------------------------------------------

    def usermsg_status(self):
        out = self.run_command("cli.usermsg")
        status = parse_usermsg_status(out)
        if status is None:
            raise ToolError(f"unexpected cli.usermsg output: {out[:200]}")
        return status

    def usermsg_messages(self):
        with self._lock:
            game_dir = self._game_dir_locked()
        return load_usermsg_schema(game_dir)

    def usermsg_events(self, since_seq, limit, name, channel=None):
        limit = min(max(int(limit or 50), 1), 200)
        cmd = "cli.usermsg events limit %d" % limit
        if since_seq is not None:
            cmd += " since %d" % max(int(since_seq), 0)
        if channel:
            cmd += " channel " + channel.strip().replace("\n", " ")
        if name:
            cmd += " name " + name.strip().replace("\n", " ")
        out = self.run_command(cmd)
        events, newest, more = parse_usermsg_events(out)
        note = None
        if not events:
            note = "no matching events" if "no matching events" in out else out[:200]
        return UserMsgEvents(events=events, newest_seq=newest, more_after=more, note=note)

    def usermsg_set_display(self, enabled):
        out = self.run_command("cli.usermsg %s" % ("on" if enabled else "off"))
        return out.strip() or ("display on" if enabled else "display off")

    def usermsg_reload_schema(self, sync_from_repo):
        with self._lock:
            game_dir = self._game_dir_locked()
        synced = None
        if sync_from_repo:
            # <repo>/configs/usermsgs; this file is <repo>/mcp/halflifecli/manager.py
            repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
            repo_schemas = os.path.join(repo_root, "configs", "usermsgs")
            if os.path.isdir(repo_schemas):
                synced = sync_usermsg_schemas(game_dir, repo_schemas)
        self.run_command("cli.usermsg reload")
        status = self.usermsg_status()
        parts = []
        if synced is not None:
            parts.append(f"synced {synced} schema file(s) from the repo")
        parts.append(f"schema={status.schema_file} messages={status.messages} "
                     f"hooks: {status.wrapped} wrapped, {status.self_registered} self-registered, "
                     f"{status.pending} pending")
        return "; ".join(parts)

    def shutdown(self):
        """Stop the game this server launched; never touches attached games."""
        with self._lock:
            proc = self._proc
            conn = self._rcon
        try:
            if proc is not None and proc.alive:
                rcon_quit = (lambda: conn.command("quit")) if conn is not None else None
                log.info("stopping managed game: %s", proc.stop(quit_command=rcon_quit).summary)
        finally:
            if conn is not None:
                conn.close()
            if self._attached_rcon is not None:
                self._attached_rcon.close()
