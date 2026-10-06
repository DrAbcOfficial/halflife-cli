#pragma once

// Game-window management.
//
// Hide: the render window is kept alive but moved off-screen by default
// ("snapshot" keeps working); ShowWindow(SW_HIDE) is optional because some
// engines pause rendering or presentation on truly hidden windows, which
// would break screenshots.
//
// InputDisabled: the game window ignores all mouse and keyboard input
// (EnableWindow). This is block_input's fallback when its engine-level hooks
// are unavailable (see input/engine_input.h). Only the game window's HWND is
// affected — the CLI console is a different window and keeps receiving input,
// and piped stdin is unaffected in every mode.
namespace WindowManager
{
	void ApplyConfiguredMode();          // called from HUD_Frame (window exists by then)
	void SetMode(int mode);              // 0 = off, 1 = off-screen, 2 = SW_HIDE
	int GetMode();
	void SetInputDisabled(bool disabled);  // true = the game window ignores mouse/keyboard
	void* GetGameWindow();               // cached game HWND as void* (keeps <windows.h> out of this header); null when not found
	bool GetClientSize(int& width, int& height);  // game window client area; false when not found
	void Restore();                      // best effort restore on shutdown
}
