#!/usr/bin/env python3
"""Locate the Sven Co-op install directory for halflife-cli.

Resolution order: --dir argument, GAME_DIR environment variable, then
mcp/game_dir.txt (a single-line machine-local config next to this
script), then a filesystem search: Steam registry roots ->
steamapps/libraryfolders.vdf libraries -> common install layouts. The first
candidate containing svencoop.exe wins. Nothing is hardcoded to a specific
user's install path.

Prints the resolved directory and exits 0, or exits 1 with guidance.

Usage:
  python mcp/find_game.py              # resolve and print
  python mcp/find_game.py --dir X      # validate one candidate
"""

import argparse
import os
import re
import subprocess
import sys

MARKER = "svencoop.exe"
CONFIG_NAME = "game_dir.txt"
MOD_NAME = "Sven Co-op"


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


def resolve_game_dir(explicit=None):
    """Return (path, source) on success or (None, reason) on failure.

    An explicit request is authoritative: if it names a directory without
    svencoop.exe, fail instead of silently falling back to other sources.
    """
    if explicit is not None:
        value = explicit.strip().strip('"')
        if is_game_dir(value):
            return os.path.normpath(value), "--dir"
        return None, "explicit path was given but no %s under %r" % (MARKER, value)
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
    return None, "no directory containing %s was found" % MARKER


def main():
    parser = argparse.ArgumentParser(description="Locate the Sven Co-op install directory.")
    parser.add_argument("--dir", help="candidate game directory to validate")
    args = parser.parse_args()

    path, info = resolve_game_dir(args.dir)
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
