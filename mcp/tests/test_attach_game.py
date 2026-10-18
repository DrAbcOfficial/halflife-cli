"""Explicit external-game selection without launching or owning its process."""
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from halflifecli.manager import Manager
from halflifecli.plugin_config import Endpoint
from game_process import GameTarget
from mcp.server.mcpserver.exceptions import ToolError


class AttachGameTests(unittest.TestCase):
    def setUp(self):
        self.manager = Manager()
        self.target = GameTarget("external", mod="cstrike")
        self.endpoint = Endpoint("goldsrc-udp", "127.0.0.1", 27015, 123, "456")
        self.connection = Mock()
        self.connection.command.return_value = "ready"
        for name, value in (
            ("resolve_target", (self.target, None)),
            ("read_endpoint_file", self.endpoint),
            ("plugin_rcon_password", "pw"),
            ("rcon_client.RconConnection", self.connection),
        ):
            patcher = patch("halflifecli.manager." + name, return_value=value)
            setattr(self, name.split(".")[-1], patcher.start())
            self.addCleanup(patcher.stop)
        env = patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "0"})
        env.start()
        self.addCleanup(env.stop)
        self.addCleanup(self.manager.shutdown)

    def test_selects_external_target_and_routes_commands(self):
        with patch("halflifecli.manager.GameProcess") as process:
            status = self.manager.attach_game("external", 10, "cstrike")
        process.assert_not_called()
        self.resolve_target.assert_called_once_with("external", 10, "cstrike")
        self.assertEqual(("attached", 123, "external", "cstrike", "goldsrc-udp"),
                         (status.mode, status.pid, status.game_dir, status.mod, status.protocol))
        self.assertEqual("ready", self.manager.run_command("echo ok"))
        self.assertEqual("attached", self.manager.game_status().mode)
        self.manager.shutdown()
        self.connection.command.assert_called_once_with("echo ok")

    def test_failed_selection_preserves_previous_connection(self):
        old_target, old_connection = GameTarget("old"), Mock()
        self.manager._target = old_target
        self.manager._attached_rcon = old_connection
        self.manager._attached_key = ("old",)
        self.connection.open.side_effect = RuntimeError("authentication failed")
        with self.assertRaisesRegex(ToolError, "RCON"):
            self.manager.attach_game(None, 70, "valve")
        self.assertIs(old_target, self.manager._target)
        self.assertIs(old_connection, self.manager._attached_rcon)
        old_connection.close.assert_not_called()
        self.connection.close.assert_called_once()

    def test_successful_switch_closes_old_connection(self):
        old_connection = Mock()
        self.manager._attached_rcon = old_connection
        self.manager.attach_game(None, 10, "cstrike")
        old_connection.close.assert_called_once()

    def test_missing_endpoint_does_not_connect(self):
        self.read_endpoint_file.return_value = None
        with self.assertRaisesRegex(ToolError, "endpoint"):
            self.manager.attach_game(None, 70, None)
        self.RconConnection.assert_not_called()

    def test_resolution_failure(self):
        self.resolve_target.return_value = (None, "missing install")
        with self.assertRaisesRegex(ToolError, "missing install"):
            self.manager.attach_game(None, 70, None)

    def test_live_managed_game_cannot_be_replaced(self):
        process = Mock(alive=True)
        self.manager._proc = process
        with self.assertRaisesRegex(ToolError, "managed game"):
            self.manager.attach_game(None, 70, None)
        self.resolve_target.assert_not_called()
        process.stop.assert_not_called()
        self.manager._proc = None

    def test_exited_managed_game_does_not_mask_attached_status(self):
        self.manager._proc = Mock(alive=False, pid=999)
        with patch("halflifecli.manager.wait_pid_gone", return_value=True):
            self.manager.attach_game(None, 70, None)
        self.assertEqual("attached", self.manager.game_status().mode)

    def test_attach_disabled(self):
        with patch.dict(os.environ, {"HALFLIFE_DISABLE_ATTACH": "1"}):
            with self.assertRaisesRegex(ToolError, "disabled"):
                self.manager.attach_game(None, 70, None)
        self.resolve_target.assert_not_called()

    def test_shutting_down_managed_game_cannot_be_forgotten(self):
        process = Mock(alive=False, pid=999)
        self.manager._proc = process
        with patch("halflifecli.manager.wait_pid_gone", return_value=False):
            with self.assertRaisesRegex(ToolError, "shutting down"):
                self.manager.attach_game(None, 70, None)
        self.assertIs(process, self.manager._proc)
        self.resolve_target.assert_not_called()

    def test_mcp_tool_forwards_target_and_is_registered(self):
        import anyio
        import halflife_mcp

        async def check_registration():
            names = [tool.name for tool in await halflife_mcp.mcp.list_tools()]
            self.assertIn("attach_game", names)

        anyio.run(check_registration)
        with patch.object(halflife_mcp.manager, "attach_game") as attach:
            result = halflife_mcp.attach_game("external", 70, "valve")
        attach.assert_called_once_with("external", 70, "valve")
        self.assertIs(attach.return_value, result)


if __name__ == "__main__":
    unittest.main()
