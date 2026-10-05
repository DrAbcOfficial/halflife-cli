"""Game target resolution: mod-aware paths and per-launcher command lines.

Sven Co-op is the default and keeps the dependency-free chain (its launcher is
<game>/svencoop.exe, its mod is svencoop). Every other app is described by
MetahookInstallerCLI, which also reports the mod directory and the launcher, so
these tests pin both shapes.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from find_game import resolve_target
from game_process import (
    GameTarget,
    build_game_argv,
    plugin_config_dir,
    port_file_path,
    screenshot_dirs,
    screenshots_dir,
)

DESCRIBED = {
    "GameDirectory": r"D:\CS3266",
    "ModDirectory": "cstrike",
    "LauncherPath": r"D:\CS3266\MetaHook_blob.exe",
}
CS_TARGET = GameTarget(
    DESCRIBED["GameDirectory"],
    mod=DESCRIBED["ModDirectory"],
    launcher=DESCRIBED["LauncherPath"],
    launch_args=("-insecure", "-game", "cstrike"),
)


class BuildArgvTests(unittest.TestCase):
    def test_bare_root_keeps_the_sven_command_line(self):
        argv = build_game_argv(r"D:\Sven Co-op")
        self.assertEqual([r"D:\Sven Co-op\svencoop.exe", "-windowed", "-novid"], argv)

    def test_described_launcher_gets_its_launch_args(self):
        argv = build_game_argv(CS_TARGET)
        self.assertEqual(
            [DESCRIBED["LauncherPath"], "-insecure", "-game", "cstrike", "-windowed", "-novid"], argv)

    def test_extra_args_follow_and_do_not_duplicate_the_mandatory_flags(self):
        argv = build_game_argv(r"D:\Sven Co-op", ["-windowed", "-novid", "-console"])
        self.assertEqual([r"D:\Sven Co-op\svencoop.exe", "-windowed", "-novid", "-console"], argv)


class ModPathTests(unittest.TestCase):
    def test_paths_follow_the_target_mod(self):
        self.assertEqual(os.path.join(r"D:\CS3266", "cstrike", "metahook", "configs", "halflifecli"),
                         plugin_config_dir(CS_TARGET))
        self.assertEqual(os.path.join(r"D:\CS3266", "cstrike", "metahook", "configs", "halflifecli",
                                      "halflifecli.port"),
                         port_file_path(CS_TARGET))
        self.assertEqual(os.path.join(r"D:\CS3266", "cstrike", "screenshots"), screenshots_dir(CS_TARGET))

    def test_bare_root_means_the_sven_mod(self):
        root = r"D:\Sven Co-op"
        self.assertEqual(os.path.join(root, "svencoop", "metahook", "configs", "halflifecli"),
                         plugin_config_dir(root))
        self.assertEqual(os.path.join(root, "svencoop", "screenshots"), screenshots_dir(root))

    def test_screenshot_dirs_cover_mod_root_and_screenshots(self):
        # Sven writes into <mod>/screenshots; the shared GoldSrc engine (Half-Life,
        # Counter-Strike, Condition Zero) writes into the mod root.
        self.assertEqual(
            (os.path.join(r"D:\CS3266", "cstrike"),
             os.path.join(r"D:\CS3266", "cstrike", "screenshots")),
            screenshot_dirs(CS_TARGET))
        self.assertEqual(
            (os.path.join(r"D:\Sven Co-op", "svencoop"),
             os.path.join(r"D:\Sven Co-op", "svencoop", "screenshots")),
            screenshot_dirs(r"D:\Sven Co-op"))


class ResolveTargetTests(unittest.TestCase):
    def test_sven_explicit_dir_needs_no_installer_cli(self):
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, "svencoop.exe"), "wb").close()
            with patch("find_game.find_installer_cli") as cli:
                target, reason = resolve_target(d)
            self.assertIsNone(reason)
            cli.assert_not_called()
            self.assertEqual((os.path.normpath(d), "svencoop"), (target.directory, target.mod))
            self.assertEqual((), target.launch_args)
            self.assertEqual(os.path.join(d, "svencoop.exe"), target.launcher_path)

    def test_sven_explicit_dir_without_the_marker_fails(self):
        with tempfile.TemporaryDirectory() as d:
            target, reason = resolve_target(d)
            self.assertIsNone(target)
            self.assertIn("svencoop.exe", reason)

    def test_explicit_non_sven_dir_is_still_described_by_the_cli(self):
        # The mod and the launcher cannot be derived from the directory alone.
        with tempfile.TemporaryDirectory() as d:
            calls = {}

            def describe(cli, appid, gamedir=None, moddir=None):
                calls.update(cli=cli, appid=appid, gamedir=gamedir, moddir=moddir)
                return DESCRIBED, None

            with patch("find_game.find_installer_cli", return_value="cli.exe"), \
                 patch("find_game.cli_describe_target", side_effect=describe):
                target, reason = resolve_target(d, appid=10, mod="cstrike")
            self.assertIsNone(reason)
            self.assertEqual(("cli.exe", 10, d, "cstrike"),
                             (calls["cli"], calls["appid"], calls["gamedir"], calls["moddir"]))
            self.assertEqual((DESCRIBED["GameDirectory"], "cstrike", DESCRIBED["LauncherPath"]),
                             (target.directory, target.mod, target.launcher))
            self.assertEqual(("-insecure", "-game", "cstrike"), target.launch_args)
            self.assertEqual("MetahookInstallerCLI", target.source)

    def test_non_sven_without_the_cli_fails(self):
        with tempfile.TemporaryDirectory() as d:
            with patch("find_game.find_installer_cli", return_value=None):
                target, reason = resolve_target(d, appid=10)
            self.assertIsNone(target)
            self.assertIn("MetahookInstallerCLI", reason)

    def test_description_without_a_mod_directory_is_rejected(self):
        with patch("find_game.find_installer_cli", return_value="cli.exe"), \
             patch("find_game.cli_describe_target", return_value=({"GameDirectory": r"D:\X"}, None)):
            target, reason = resolve_target(appid=70)
        self.assertIsNone(target)
        self.assertIn("ModDirectory", reason)

    def test_cli_failure_reason_is_propagated(self):
        with patch("find_game.find_installer_cli", return_value="cli.exe"), \
             patch("find_game.cli_describe_target", return_value=(None, "invalid install target")):
            target, reason = resolve_target(appid=10)
        self.assertIsNone(target)
        self.assertEqual("invalid install target", reason)


class LaunchGameTargetTests(unittest.TestCase):
    def test_appid_and_mod_reach_the_launch(self):
        from halflifecli.manager import Manager

        seen = {}

        class FakeProcess:
            rcon_protocol = "source-tcp"
            pid = 7
            alive = True
            returncode = None

            def __init__(self, argv, cwd):
                seen["argv"], seen["cwd"] = argv, cwd

            def start(self):
                return self

            def wait_for_banner(self, timeout):
                return "127.0.0.1", 27015

        class FakeConnection:
            def __init__(self, host, port, password, protocol=None):
                seen["protocol"] = protocol

            def open(self):
                pass

            def close(self):
                pass

        manager = Manager()
        with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "1"}), \
             patch("halflifecli.manager.resolve_target", return_value=(CS_TARGET, None)) as resolve, \
             patch("halflifecli.manager.GameProcess", FakeProcess), \
             patch("halflifecli.manager.rcon_client.RconConnection", FakeConnection), \
             patch("halflifecli.manager.plugin_rcon_password", return_value=""), \
             patch("halflifecli.manager.read_endpoint_file", return_value=None):
            status = manager.launch_game([], None, 2, 10, "cstrike")
        resolve.assert_called_once_with(None, 10, "cstrike")
        self.assertEqual([DESCRIBED["LauncherPath"], "-insecure", "-game", "cstrike", "-windowed", "-novid"],
                         seen["argv"])
        self.assertEqual(DESCRIBED["GameDirectory"], seen["cwd"])
        self.assertEqual(("managed", DESCRIBED["GameDirectory"], "cstrike"),
                         (status.mode, status.game_dir, status.mod))


class LaunchFailureTests(unittest.TestCase):
    def test_game_dying_before_the_banner_reports_its_console_output(self):
        from game_process import GameExitedError
        from halflifecli.manager import Manager

        class DyingProcess:
            rcon_protocol = "source-tcp"
            pid = 9
            alive = False
            returncode = 3221226505

            def __init__(self, argv, cwd):
                pass

            def start(self):
                return self

            def wait_for_banner(self, timeout):
                raise GameExitedError("game exited before printing the banner")

            def tail(self, count):
                return ["halflife-cli 2026-10-05T15:19:44 loaded (engine: GoldSrc_Blob)"]

            def stop(self, quit_command=None, timeout=None):
                return None

        manager = Manager()
        with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "1"}), \
             patch("halflifecli.manager.resolve_target", return_value=(CS_TARGET, None)), \
             patch("halflifecli.manager.GameProcess", DyingProcess):
            with self.assertRaises(Exception) as caught:
                manager.launch_game([], None, 2, 10, "cstrike")
        message = str(caught.exception)
        self.assertIn("3221226505", message)
        self.assertIn("GoldSrc_Blob", message)


if __name__ == "__main__":
    unittest.main()
