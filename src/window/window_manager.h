#pragma once

// Game-window management.
//
// Hide: the render window is kept alive but moved off-screen by default
// ("snapshot" keeps working); ShowWindow(SW_HIDE) is optional because some
// engines pause rendering or presentation on truly hidden windows, which
// would break screenshots.
//
// BlockInput: when enabled, the game window ignores all mouse and keyboard
// input (EnableWindow). Only the game window's HWND is affected — the CLI
// console is a different window and keeps receiving input, and piped stdin
// is unaffected in every mode.
namespace WindowManager
{
	void ApplyConfiguredMode();          // called from HUD_Frame (window exists by then)
	void SetMode(int mode);              // 0 = off, 1 = off-screen, 2 = SW_HIDE
	int GetMode();
	void SetBlockInput(bool block);      // true = the game window ignores mouse/keyboard
	bool GetBlockInput();
	void Restore();                      // best effort restore on shutdown
}
