#pragma once

// Mouse/cursor lock for the game window.
//
// block_input (WindowManager) only stops window *messages* reaching the game
// window: the client DLL still polls the cursor itself on the non-raw input
// path (GetCursorPos every frame, then SetCursorPos back to the window
// centre), so a physically moving mouse keeps driving the view and the real
// OS cursor gets warped. While this lock is active, IAT hooks on the client
// module make the game see "cursor always at the window centre" (mouse delta
// 0 -> view frozen) and swallow its SetCursorPos, so the real cursor stays
// wherever the user leaves it.
//
// The client AND engine modules are hooked — which one samples the mouse
// depends on the game build and m_rawinput (Sven Co-op 5.x samples in the
// engine). vgui2/vgui/tier0 are not: they legitimately use the cursor for UI
// and debug overlays. Paths covered: non-raw user32 (GetCursorPos /
// SetCursorPos on the client), raw SDL relative (client or engine), and the
// engine's SDL_WarpMouseInWindow recentring warp.
namespace InputLock
{
	// Installs the IAT hooks on the client module. Idempotent: LoadClient
	// runs again on every map change.
	void InstallHooks();

	void SetActive(bool active);      // lock on/off at runtime
	bool GetActive();

	// Lock ready: the non-raw user32 pair is hooked, or the raw SDL path is.
	bool HooksInstalled();

	// Calls seen through the hooks so far (any lock state): which counter
	// grows shows which input path the live game exercises. motionCalls is
	// the number of SDL calls that carried nonzero motion (before the lock
	// zeroed it), lastDx/lastDy the most recent values.
	void GetHookStats(unsigned& getCalls, unsigned& setCalls, unsigned& relCalls, unsigned& warpCalls,
		unsigned& motionCalls, int& lastDx, int& lastDy);

	// Drops the lock on shutdown. The hooks stay installed: they pass through
	// while inactive, and unhooking could write into a client module the
	// engine has already unloaded (map change / exit ordering).
	void Shutdown();
}
