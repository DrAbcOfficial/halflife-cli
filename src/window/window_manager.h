#pragma once

// Hides the game's render window while keeping it alive (rendering continues,
// "snapshot" keeps working). Default strategy is moving the window off-screen
// instead of ShowWindow(SW_HIDE): some engines pause rendering or presentation
// on truly hidden windows, which would break screenshots.
namespace WindowHide
{
	void ApplyConfiguredMode();          // called from HUD_Frame (window exists by then)
	void SetMode(int mode);              // 0 = off, 1 = off-screen, 2 = SW_HIDE
	int GetMode();
	void Restore();                      // best effort restore on shutdown
}
