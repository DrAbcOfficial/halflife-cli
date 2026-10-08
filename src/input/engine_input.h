#pragma once

// Native input before CGame dispatches to gameplay and VGUI:
// SDL2 uses SDL_SetEventFilter / SDL_PushEvent; legacy engines use an inline
// CGame::WindowProc hook / its original trampoline. On SDL2 the engine's
// SDL_GetMouseState / SDL_PollEvent / SDL_WaitEventTimeout imports are hooked
// for the virtual UI cursor. Legacy engine/VGUI user32 cursor imports serve
// the same purpose. Client view sampling remains separate in input_lock.
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

	// UI cursor motion (SDL2 and legacy WindowProc backends). x/y are original
	// screenshot pixels (the video mode size), clamped to the image; relative adds
	// them to the current position. Injected motion owns a virtual cursor
	// that native UI cursor queries see instead of the desktop cursor, until
	// allowed physical motion takes it over. Relative motion moves the UI cursor,
	// not the FPS view.
	bool MoveMouse(int x, int y, bool relative);
	bool GetMousePosition(int& x, int& y);   // current cursor in screenshot pixels
	// An engine warp in window coordinates: true when the virtual cursor took
	// it, so the desktop cursor must not move.
	bool WarpVirtualMouse(int x, int y);

	// Physical events seen by the hooks, and how many the block dropped.
	void GetStats(unsigned& keyEvents, unsigned& keyBlocked, unsigned& mouseEvents, unsigned& mouseBlocked);

	// Restore the previous SDL filter and remove hooks before unloading.
	void OnExitGame();
	void Shutdown();
}
