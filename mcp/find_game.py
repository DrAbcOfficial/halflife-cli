#!/usr/bin/env python3
"""Locate a GoldSrc game install directory for halflife-cli.

Two resolution paths:

* Sven Co-op (the default app, 225840) uses the fast, dependency-free chain:
  --dir argument -> GAME_DIR environment variable -> mcp/game_dir.txt ->
  a filesystem search (Steam registry roots -> steamapps/libraryfolders.vdf
  libraries -> common install layouts), the first candidate containing
  svencoop.exe winning.
* Any other GoldSrc app (Half-Life, Counter-Strike, ...) is resolved by
  MetahookInstallerCLI, which knows Steam's install layout and each app's
  default mod. Pass --appid, or it is used as the final fallback for Sven.

MetahookInstallerCLI is discovered from HALFLIFECLI_INSTALLER_CLI_EXECUTABLE,
then the CMake build tree (the LaunchGame workflow caches it there). It is
never downloaded here; enable HALFLIFECLI_ENABLE_LAUNCH_GAME and configure
once to provision it. Nothing is hardcoded to a specific user's install path.

Prints the resolved directory and exits 0, or exits 1 with guidance.

Usage:
  python mcp/find_game.py                     # Sven Co-op (default)
  python mcp/find_game.py --dir X             # validate one candidate
  python mcp/find_game.py --appid 70          # Half-Life, via InstallerCLI
  python mcp/find_game.py --appid 10          # Counter-Strike
  python mcp/find_game.py --appid 70 --mod cstrike
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

MARKER = "svencoop.exe"
CONFIG_NAME = "game_dir.txt"
MOD_NAME = "Sven Co-op"
DEFAULT_APPID = 225840  # Sven Co-op; other apps go through MetahookInstallerCLI
CLI_ENV_VARIABLE = "HALFLIFECLI_INSTALLER_CLI_EXECUTABLE"
CLI_TIMEOUT_S = 30


def is_game_dir(path):
    return bool(path) and os.path.isfile(os.path.join(path, MARKER))


def read_config(config_path):
    """Read the first line, tolerating UTF-8/UTF-16 BOMs written by PowerShell."""
    try:
        with open(config_path, "rb") as f:
            raw = f.read(4096)
    except OSError:
        return ""
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw.decode("utf-8-sig")
    else:
        try:
            text = raw.decode("mbcs", errors="replace")
        except LookupError:
            text = raw.decode("utf-8", errors="replace")
    lines = text.splitlines()
    return lines[0].strip().strip('"') if lines else ""


def configured_candidates():
    """(source, path) pairs from env and config file."""
    candidates = []
    env = os.environ.get("GAME_DIR", "").strip().strip('"')
    if env:
        candidates.append(("GAME_DIR", env))
    config = os.path.join(os.path.dirname(os.path.abspath(__file__)), CONFIG_NAME)
    if os.path.isfile(config):
        value = read_config(config)
        if value:
            candidates.append((CONFIG_NAME, value))
    return candidates


def registry_value(root_key, value_name):
    try:
        proc = subprocess.run(
            ["reg", "query", root_key, "/v", value_name],
            capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    if proc.returncode != 0:
        return ""
    m = re.search(r"REG_SZ\s+(.+)", proc.stdout, re.I)
    value = m.group(1).strip() if m else ""
    return value.replace("/", "\\")


def steam_roots():
    roots = []
    for key, name in (
        (r"HKCU\Software\Valve\Steam", "SteamPath"),
        (r"HKLM\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (r"HKLM\SOFTWARE\Valve\Steam", "InstallPath"),
    ):
        value = registry_value(key, name)
        if value and value not in roots:
            roots.append(value)
    return roots


def vdf_libraries(steam_root):
    vdf = os.path.join(steam_root, "steamapps", "libraryfolders.vdf")
    libraries = []
    try:
        with open(vdf, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r'\s*"path"\s+"(.*)"', line, re.I)
                if m:
                    libraries.append(m.group(1).replace("\\\\", "\\"))
    except OSError:
        pass
    return libraries


def search_candidates():
    candidates = []
    for root in steam_roots():
        candidates.append(os.path.join(root, "steamapps", "common", MOD_NAME))
        for lib in vdf_libraries(root):
            candidates.append(os.path.join(lib, "steamapps", "common", MOD_NAME))
    candidates.append(os.path.join(
        r"C:\Program Files (x86)\Steam", "steamapps", "common", MOD_NAME))
    for drive in "CDEF":
        candidates.append("%s:\\SteamLibrary\\steamapps\\common\\%s" % (drive, MOD_NAME))
        candidates.append("%s:\\Steam\\steamapps\\common\\%s" % (drive, MOD_NAME))
    return candidates


def dedupe(paths):
    seen, out = set(), []
    for p in paths:
        key = os.path.normcase(os.path.normpath(p))
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def find_installer_cli():
    """Locate MetahookInstallerCLI.exe, or None.

    Discovery order: the explicit environment override, then the CMake build
    tree where the LaunchGame workflow caches the CLI (both the staged Release
    copy and the downloaded installer cache).
    """
    candidates = []
    override = os.environ.get(CLI_ENV_VARIABLE, "").strip().strip('"')
    if override:
        candidates.append(override)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    launch_root = os.path.join(repo_root, "build", "launch-game")
    candidates += sorted(glob.glob(os.path.join(launch_root, "*", "MetahookInstallerCLI.exe")))
    candidates += sorted(glob.glob(os.path.join(launch_root, "installer", "*", "MetahookInstallerCLI.exe")))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def cli_describe_target(cli, appid, gamedir=None, moddir=None):
    """Query MetahookInstallerCLI -describe-target.

    Returns (description_dict, None) on success or (None, reason) on failure.
    The description carries GameDirectory, ModDirectory and LauncherPath.
    """
    args = [cli, "-appid", str(appid)]
    if gamedir:
        args += ["-gamedir", gamedir]
    if moddir:
        args += ["-moddir", moddir]
    args.append("-describe-target")
    try:
        proc = subprocess.run(args, capture_output=True, text=True, timeout=CLI_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, "MetahookInstallerCLI could not run: %s" % error
    if proc.returncode != 0:
        reason = (proc.stderr or proc.stdout or "").strip() or "exit code %d" % proc.returncode
        # The CLI already prefixes its diagnostics with "Error:".
        reason = re.sub(r"^Error:\s*", "", reason, count=1, flags=re.I)
        return None, reason
    try:
        description = json.loads(proc.stdout)
    except ValueError:
        return None, "MetahookInstallerCLI returned unparseable output: %r" % proc.stdout.strip()
    if not isinstance(description, dict) or not description.get("GameDirectory"):
        return None, "MetahookInstallerCLI did not report a GameDirectory"
    return description, None


def resolve_game_dir(explicit=None, appid=None, mod=None):
    """Return (path, source) on success or (None, reason) on failure.

    Sven Co-op (the default app) keeps the fast local chain. Any other app, or
    a Sven lookup that found nothing locally, is resolved through
    MetahookInstallerCLI.

    An explicit request is authoritative: if it names a directory without
    svencoop.exe, fail instead of silently falling back to other sources.
    """
    selected_appid = DEFAULT_APPID if appid is None else int(appid)

    if explicit is not None:
        value = explicit.strip().strip('"')
        if selected_appid == DEFAULT_APPID:
            if is_game_dir(value):
                return os.path.normpath(value), "--dir"
            return None, "explicit path was given but no %s under %r" % (MARKER, value)
        if os.path.isdir(value):
            return os.path.normpath(value), "--dir"
        return None, "explicit path was given but %r is not a directory" % (value,)

    # The env/config chain and the marker search are Sven-specific: their marker
    # is svencoop.exe and their configured paths are Sven installs.
    if selected_appid == DEFAULT_APPID:
        configured = configured_candidates()
        for source, value in configured:
            if is_game_dir(value):
                return os.path.normpath(value), source
        for source, value in configured:
            print("note: %s points to %r, no %s there; searching..." %
                  (source, value, MARKER), file=sys.stderr)
        for cand in dedupe(search_candidates()):
            if is_game_dir(cand):
                return os.path.normpath(cand), "search"

    # Anything unresolved (every non-Sven app, and Sven when nothing local
    # matched) goes through MetahookInstallerCLI, which finds the Steam install.
    cli = find_installer_cli()
    if not cli:
        if selected_appid == DEFAULT_APPID:
            return None, "no directory containing %s was found" % MARKER
        return None, (
            "app %d needs MetahookInstallerCLI, which was not found. Set %s or "
            "configure CMake with -DHALFLIFECLI_ENABLE_LAUNCH_GAME=ON" %
            (selected_appid, CLI_ENV_VARIABLE))
    description, reason = cli_describe_target(cli, selected_appid, moddir=mod)
    if description is None:
        return None, reason
    game_dir = os.path.normpath(description["GameDirectory"])
    print("note: MetahookInstallerCLI resolved app %d -> %s (mod %s)" %
          (selected_appid, game_dir, description.get("ModDirectory", "?")), file=sys.stderr)
    return game_dir, "MetahookInstallerCLI"


def main():
    parser = argparse.ArgumentParser(description="Locate a GoldSrc game install directory.")
    parser.add_argument("--dir", help="candidate game directory to validate")
    parser.add_argument("--appid", type=int, default=DEFAULT_APPID,
                        help="Steam app ID (default %d = Sven Co-op); other apps use "
                             "MetahookInstallerCLI, e.g. 70 Half-Life, 10 Counter-Strike" % DEFAULT_APPID)
    parser.add_argument("--mod", help="mod directory for MetahookInstallerCLI (e.g. cstrike)")
    args = parser.parse_args()

    path, info = resolve_game_dir(args.dir, appid=args.appid, mod=args.mod)
    if path:
        print(path)
        return 0

    print("ERROR: %s" % info, file=sys.stderr)
    print("Configure it externally, then retry:", file=sys.stderr)
    print('  set GAME_DIR=C:\\path\\to\\Sven Co-op', file=sys.stderr)
    print('  or write the full path into mcp/game_dir.txt (one line):', file=sys.stderr)
    print('  echo C:\\path\\to\\Sven Co-op>"mcp\\game_dir.txt"', file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
