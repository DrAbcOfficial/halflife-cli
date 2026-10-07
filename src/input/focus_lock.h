#pragma once

// Focus lock: keeps the engine internally active while its window is
// unfocused, hidden or off-screen. Desktop foreground focus is left to the
// OS, and a minimized render target is not restored.
//
// CGame::AppActivate (gamedata CGame_AppActivate) is inline-hooked. The hook
// records the real activation and applies the effective one (lock or real
// focus) only when it changes: repeated focus events would otherwise run
// ClearIOStates and drop held injected input. Turning the lock off restores
// the real state. The engine starts active and an unfocused window may never
// report a focus change, so CGame::SleepUntilInput (gamedata of that name)
// is hooked too: the first frame captures the instance and applies the
// initial state.
namespace FocusLock
{
	// Call from LoadClient; idempotent.
	void Install();
	bool Available();                 // AppActivate and SleepUntilInput hooked
	const char* Error();              // why it is unavailable

	void SetActive(bool locked);      // lock on/off at runtime
	bool GetActive();

	// The engine's IsActiveApp; false until the engine has activated once.
	bool EngineActive(bool& active);

	// Locked while the window is not really focused, i.e. the engine is
	// active only because of the lock. input_lock then freezes the desktop
	// cursor (the focus guard).
	bool ForcingActivation();

	// Drop the lock without applying it and remove the hook before unloading.
	void OnExitGame();
	void Shutdown();
}
