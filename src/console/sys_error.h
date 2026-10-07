#pragma once

// Fatal-error export: hooks the engine's Sys_Error and mirrors its message to
// the CLI console and to <mod>/metahook/configs/halflifecli/errors.log before
// the game dies. MetaHook reports its own load failures through the same
// engine function (MH_SysError calls the pfnSys_Error it resolved), so one hook
// covers engine, MetaHook and plugin fatal errors raised after this plugin has
// loaded — errors raised before it load are out of reach.
//
// The original function is always called last: the dialog, the exit path and
// every other behaviour are unchanged.
namespace SysError
{
	// Call from LoadClient, after ConsoleBridge::Init has wired stdout up. The
	// hook is installed even when the console bridge is off; the log file is
	// written either way.
	void Install();

	// Call on unload only. An engine shutdown is itself a path a fatal error
	// takes, so the hook must outlive ExitGame.
	void Shutdown();

	bool Hooked();                 // Sys_Error resolved and hooked
	const char* HookError();       // resolution/hook failure, empty when hooked
}
