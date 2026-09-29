"""Reads the plugin's halflifecli.toml and RCON port file from a game install.

Both files live under <game>/<mod>/metahook/configs/; paths are built by
game_process. Everything here tolerates absence: a missing or broken file
reads as "no setting", like the plugin's own compiled-in defaults.
"""

import os
import tomllib

from game_process import plugin_config_path, port_file_path


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
