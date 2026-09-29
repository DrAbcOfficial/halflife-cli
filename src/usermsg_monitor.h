#pragma once

// Dynamic UserMsg monitor: wraps the user-message hooks the client game DLL
// registers with the engine (g_pMetaHookAPI->HookUserMsg), decodes each
// message's payload per the TOML schema, and prints one line per message so
// CLI clients can watch network traffic in the mirrored console output.
namespace UserMsgMonitor
{
	// Loads the schema selected by [usermsg] in halflifecli.toml (default:
	// "<gamedir>.toml"). Safe to call again; reloads on every map change.
	void Init();

	// Hook (re-)assertion points: run right AFTER the original client DLL
	// HUD_Init / HUD_VidInit so messages registered there get wrapped.
	void OnHudInit();
	void OnHudVidInit();

	// Periodically re-asserts hooks (entries may be recreated per map load)
	// and applies a pending schema reload.
	void Frame();

	void Shutdown();

	// "cli.usermsg" console command handler.
	void CmdUserMsg();
}
