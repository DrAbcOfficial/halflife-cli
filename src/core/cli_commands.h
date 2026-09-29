#pragma once

// The plugin's "cli.*" console commands: help text, RCON endpoint info,
// window hide-mode control, cvar/command lookup ("cli.find"), and the
// UserMsg monitor front-end ("cli.usermsg").
namespace CliCommands
{
	// Registers the commands with the engine. Call once from HUD_Init, after
	// the client DLL registered its own console commands.
	void RegisterAll();
}
