#!/usr/bin/env python3
"""Launch and supervise a halflife-cli game process.

Shared by the MCP server (halflife_mcp.py) and the acceptance test. The game
is started with piped stdio so the plugin's console bridge talks to us: every
stdin line is executed as a console command and all captured console output
arrives on stdout. A background thread drains stdout continuously into a
bounded line buffer; the plugin flushes stdout from the engine thread, so an
undrained pipe would eventually stall the game.
"""

import collections
import dataclasses
import itertools
import logging
import os
import re
import socket
import subprocess
import threading
import time

log = logging.getLogger(__name__)

EXECUTABLE_NAME = "svencoop.exe"
MOD_DIR_NAME = "svencoop"
# The plugin keeps its config, port file and usermsg schemas in its own
# subfolder of MetaHook's configs directory (src/core/plugins.cpp writes there).
PLUGIN_DATA_DIR_NAME = "halflifecli"
PLUGIN_CONFIG_NAME = "halflifecli.toml"
PORT_FILE_NAME = "halflifecli.port"
# Off-screen hiding needs windowed mode; -novid skips the intro video.
FORCED_ARGS = ("-windowed", "-novid")
# The plugin prints one of these once RCON startup is decided (src/plugins.cpp).
# Group 3 retains the legacy startup-error contract; protocol is group 4.
BANNER_RE = re.compile(r"halflife-cli: RCON (?:listening on (\S*):(\d+)|failed to start \((.*)\))(?: \((goldsrc-udp|source-tcp),)?")
STDIN_QUIT_LINE = "quit"
LOOPBACK = "127.0.0.1"

BANNER_TIMEOUT_S = 120
QUIT_TIMEOUT_S = 30
KILL_WAIT_S = 5
EXIT_CODE_GRACE_S = 2
CONSOLE_BUFFER_LINES = 5000


class GameExitedError(RuntimeError):
    """The game process ended before the awaited output appeared."""


class RconStartError(RuntimeError):
    """The plugin loaded but reported that its RCON server failed to start."""


@dataclasses.dataclass
class ConsoleSlice:
    lines: list
    next_cursor: int  # pass back as `cursor` to continue after these lines
    dropped: int      # lines between the requested cursor and the oldest buffered line


@dataclasses.dataclass
class StopResult:
    steps: list
    exit_code: object  # int, or None if the process could not be reaped
    killed: bool

    @property
    def summary(self):
        return "; ".join(self.steps + ["exit code %s" % self.exit_code])


@dataclasses.dataclass(frozen=True)
class GameTarget:
    """A resolved game install: where it is, which mod, and how to start it.

    Sven Co-op, the default app, needs only a directory: its launcher is
    <directory>/svencoop.exe and its mod is svencoop. Every other app is
    described by MetahookInstallerCLI, which reports the mod directory and the
    launcher separately (Sven Co-op runs svencoop.exe; a legacy engine runs
    MetaHook_blob.exe with an explicit -game, see resolve_target).

    `launch_args` are the launcher-specific arguments that must precede the
    mandatory windowed/novid ones. Path helpers accept either a GameTarget or
    a bare game root, which keeps the Sven default working unchanged.
    """

    directory: str
    mod: str = MOD_DIR_NAME
    launcher: str = ""
    launch_args: tuple = ()
    source: str = ""

    @property
    def launcher_path(self):
        """The executable to start; empty launcher means the Sven default."""
        return self.launcher or os.path.join(self.directory, EXECUTABLE_NAME)


def _target_parts(target):
    """(game root, mod directory) for a GameTarget or a bare Sven game root."""
    if isinstance(target, GameTarget):
        return target.directory, target.mod
    return target, MOD_DIR_NAME


def mod_dir(target):
    return os.path.join(*_target_parts(target))


def plugin_config_dir(target):
    """The plugin's own config subfolder inside MetaHook's configs dir."""
    return os.path.join(mod_dir(target), "metahook", "configs", PLUGIN_DATA_DIR_NAME)


def plugin_config_path(target):
    return os.path.join(plugin_config_dir(target), PLUGIN_CONFIG_NAME)


def port_file_path(target):
    return os.path.join(plugin_config_dir(target), PORT_FILE_NAME)


def screenshots_dir(target):
    """Sven Co-op's dated screenshot directory, under the mod folder."""
    return os.path.join(mod_dir(target), "screenshots")


def screenshot_dirs(target):
    """Directories the engine `screenshot` command may write into.

    Sven Co-op writes dated files under <mod>/screenshots; the GoldSrc engine
    shared by Half-Life, Counter-Strike and Condition Zero writes HalfLifeNN.tga
    into the mod root itself. Searching both keeps snapshot working across
    engines without asking the target which one it is; a caller records what
    already exists in every directory and picks the file that appears.
    """
    mod = mod_dir(target)
    return (mod, screenshots_dir(target))


# Local-file screenshot formats the engine writes, depending on engine and mod
# config (svencoop's `screenshot` writes .tga; `snapshot` is taken over by
# SteamScreenshots.dll and uploads to Steam instead of writing a file).
IMAGE_EXTENSIONS = (".bmp", ".tga", ".png", ".jpg", ".jpeg")


def rcon_connect_host(bind):
    """Address that reaches the plugin's RCON server for a configured bind.

    Mirrors src/rcon_server.cpp: an empty bind or 0.0.0.0 listens on every
    interface and an unparsable address falls back to loopback, so both are
    reached on 127.0.0.1; a concrete IPv4 address is used as-is.
    """
    try:
        packed = socket.inet_pton(socket.AF_INET, bind)
    except (OSError, ValueError):
        return LOOPBACK
    return LOOPBACK if packed == bytes(4) else bind


def build_game_argv(target, extra_args=()):
    """Game command line: launcher, launcher args, mandatory flags, extras.

    The mandatory windowed/novid flags are never missing: a caller repeating one
    in extra_args drops it rather than duplicating the flag. A bare game root
    means the Sven default launcher with no launcher-specific arguments.
    """
    if isinstance(target, GameTarget):
        launcher, launch_args = target.launcher_path, list(target.launch_args)
    else:
        launcher, launch_args = os.path.join(target, EXECUTABLE_NAME), []
    extra = [arg for arg in extra_args if arg.lower() not in FORCED_ARGS]
    return [launcher, *launch_args, *FORCED_ARGS, *extra]


def _kill_on_close_job(pid):
    """Put `pid` into a Job Object that kills it once our handle closes.

    The game window is hidden off-screen by default, so a game orphaned by a
    crashed or force-killed supervisor would be easy to miss. Returns the job
    handle, which must stay referenced for the lifetime of the game, or None
    when unavailable (non-Windows, or pywin32 missing).
    """
    if os.name != "nt":
        return None
    try:
        import win32api
        import win32con
        import win32job
    except ImportError:
        log.warning("pywin32 not available: the game will outlive this process if it dies")
        return None
    try:
        job = win32job.CreateJobObject(None, "")
        info = win32job.QueryInformationJobObject(job, win32job.JobObjectExtendedLimitInformation)
        info["BasicLimitInformation"]["LimitFlags"] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        win32job.SetInformationJobObject(job, win32job.JobObjectExtendedLimitInformation, info)
        handle = win32api.OpenProcess(win32con.PROCESS_SET_QUOTA | win32con.PROCESS_TERMINATE, False, pid)
        try:
            win32job.AssignProcessToJobObject(job, handle)
        finally:
            win32api.CloseHandle(handle)
        return job
    except win32api.error as e:
        log.warning("could not attach the game to a kill-on-close job: %s", e)
        return None


class GameProcess:
    """One game process with piped stdio and a cursor-addressable console buffer.

    Every stdout line gets a monotonically increasing sequence number; the
    buffer keeps the newest `buffer_lines` of them.
    """

    def __init__(self, argv, cwd, buffer_lines=CONSOLE_BUFFER_LINES):
        self.argv = list(argv)
        self.cwd = cwd
        self._lines = collections.deque(maxlen=buffer_lines)
        self._next_seq = 0
        self._eof = False
        self._cond = threading.Condition()
        self._stdin_lock = threading.Lock()
        self._proc = None
        self._job = None
        self.rcon_protocol = "source-tcp"  # unmarked old banners are TCP only

    def start(self):
        if self._proc is not None:
            raise RuntimeError("game process already started")
        # stdout/stderr must be pipes, never inherited: when this runs inside
        # the MCP server, its own stdout is the protocol channel.
        self._proc = subprocess.Popen(
            self.argv, cwd=self.cwd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self._job = _kill_on_close_job(self._proc.pid)
        threading.Thread(target=self._drain_stdout, name="game-stdout", daemon=True).start()
        return self

    @property
    def pid(self):
        return self._proc.pid if self._proc else None

    @property
    def alive(self):
        return self._proc is not None and self._proc.poll() is None

    @property
    def returncode(self):
        return self._proc.poll() if self._proc else None

    def _drain_stdout(self):
        try:
            for raw in iter(self._proc.stdout.readline, b""):
                line = raw.decode(errors="replace").rstrip("\r\n")
                with self._cond:
                    self._lines.append(line)
                    self._next_seq += 1
                    self._cond.notify_all()
        except (OSError, ValueError):
            pass  # pipe torn down while the process exits
        finally:
            with self._cond:
                self._eof = True
                self._cond.notify_all()

    def _slice_locked(self, cursor, max_lines):
        first = self._next_seq - len(self._lines)
        if cursor is None:
            start = first if max_lines is None else max(first, self._next_seq - max_lines)
            dropped = 0
        else:
            cursor = max(0, cursor)
            start = min(max(cursor, first), self._next_seq)
            dropped = max(0, first - cursor)
        end = self._next_seq if max_lines is None else min(self._next_seq, start + max_lines)
        lines = list(itertools.islice(self._lines, start - first, end - first))
        return ConsoleSlice(lines=lines, next_cursor=end, dropped=dropped)

    def lines_since(self, cursor=None, max_lines=None):
        """Lines from `cursor` on; cursor None means the newest `max_lines`."""
        with self._cond:
            return self._slice_locked(cursor, max_lines)

    def tail(self, count):
        return self.lines_since(None, count).lines

    @property
    def next_cursor(self):
        """Cursor that reads only lines printed after this call."""
        with self._cond:
            return self._next_seq

    def wait_for(self, pattern, timeout, since=0):
        """Block until a console line matches `pattern`; return the match.

        Raises TimeoutError, or GameExitedError once stdout reaches EOF.
        """
        deadline = time.monotonic() + timeout
        cursor = since
        with self._cond:
            while True:
                chunk = self._slice_locked(cursor, None)
                cursor = chunk.next_cursor
                for line in chunk.lines:
                    match = pattern.search(line)
                    if match:
                        return match
                if self._eof:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("no console line matching %r within %ss" % (pattern.pattern, timeout))
                self._cond.wait(remaining)
        raise GameExitedError("game exited (code %s) before printing %r"
                              % (self.wait(EXIT_CODE_GRACE_S), pattern.pattern))

    def wait_for_banner(self, timeout=BANNER_TIMEOUT_S):
        """Wait for the plugin's RCON banner; return the (host, port) to connect to.

        Raises RconStartError when the plugin reports an RCON startup failure.
        """
        match = self.wait_for(BANNER_RE, timeout)
        if match.group(3) is not None:
            raise RconStartError("plugin RCON failed to start: %s" % match.group(3))
        self.rcon_protocol = match.group(4) or "source-tcp"
        return rcon_connect_host(match.group(1)), int(match.group(2))

    def send_stdin(self, line):
        """Queue one console command through the plugin's stdin reader."""
        with self._stdin_lock:
            try:
                self._proc.stdin.write((line + "\n").encode())
                self._proc.stdin.flush()
                return True
            except (OSError, ValueError):
                return False

    def wait(self, timeout):
        """Exit code, or None if still running after `timeout` seconds."""
        try:
            return self._proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        try:
            self._proc.kill()
        except OSError:
            pass  # already gone

    def stop(self, quit_command=None, timeout=QUIT_TIMEOUT_S):
        """Quit cleanly: `quit_command` (e.g. RCON quit), else stdin, kill last."""
        if not self.alive:
            return StopResult(steps=["game already exited"], exit_code=self.returncode, killed=False)
        steps = []
        replied = False
        if quit_command is not None:
            try:
                quit_command()
                steps.append("quit sent via RCON")
                replied = True
            except (OSError, ValueError) as e:
                # A sent UDP quit may have executed. Wait for exit before any
                # explicit stdin fallback; never retransmit it on a timeout.
                steps.append("no RCON reply to quit (%s)" % e)
                if isinstance(e, TimeoutError) and "may have executed" in str(e):
                    replied = True
        if not replied:
            steps.append("quit sent via stdin" if self.send_stdin(STDIN_QUIT_LINE) else "stdin already closed")
        code = self.wait(timeout)
        killed = code is None
        if killed:
            steps.append("no exit within %ss, killed" % timeout)
            self.kill()
            code = self.wait(KILL_WAIT_S)
        return StopResult(steps=steps, exit_code=code, killed=killed)
