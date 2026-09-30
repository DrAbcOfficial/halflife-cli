#pragma once

// Native input before CGame dispatches to gameplay and VGUI:
// SDL2 uses SDL_SetEventFilter / SDL_PushEvent; legacy engines use an inline
// CGame::WindowProc hook / its original trampoline. Mouse motion polled by
// the client is handled separately by input_lock.
namespace EngineInput
{
	// Call from LoadClient, once the native input system is initialized.
	void Install();

	void SetBlockInput(bool block);
	bool GetBlockInput();

	bool EngineHooksInstalled();          // native input backend available
	const char* EngineHooksError();       // backend setup or latest injection failure
	bool WindowFallbackActive();          // block applied by disabling the game window instead

	// Engine keynum for a key name as used by `bind` ("w", "SPACE", "ENTER",
	// "MOUSE1", "MWHEELUP", ...) or a decimal keynum; -1 when unknown.
	int KeyFromName(const char* name);

	// Injects native input without OS input synthesis. buttons is the
	// held-button mask after the event (1=left 2=right 4=middle 8=mouse4
	// 16=mouse5). The mask determines button transitions; down is retained for
	// command compatibility. Wheel key-up is a no-op (a wheel event is a pulse).
	// False on unavailable backend, unmappable key or rejected SDL event.
	bool SendKey(int key, bool down);
	bool SendMouse(int buttons, bool down);

	// Physical events seen by the hooks, and how many the block dropped.
	void GetStats(unsigned& keyEvents, unsigned& keyBlocked, unsigned& mouseEvents, unsigned& mouseBlocked);

	// Restore the previous SDL filter and remove hooks before unloading.
	void OnExitGame();
	void Shutdown();
}
