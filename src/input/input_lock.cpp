#include "input/input_lock.h"

#include "input/engine_input.h"
#include "input/focus_lock.h"
#include "util/import_hook.h"
#include "window/window_manager.h"

#include <metahook.h>

#include <windows.h>

// Mouse/cursor lock, IAT hooks on the client and engine modules.
//
// Which module actually samples the mouse depends on the game build and the
// m_rawinput mode, so every live path is covered:
//
//   client.dll, non-raw (m_rawinput 0): IN_Accumulate reads GetCursorPos and
//     IN_ResetMouse warps back with SetCursorPos every frame. While locked,
//     GetCursorPos returns the position the game last tried to warp to (its
//     own window centre, so the per-frame delta is 0 -> view frozen) and
//     SetCursorPos is swallowed (the real cursor stays wherever it is).
//     The pair must move together: swallowing only the warp lets the delta
//     grow without bound and the view spins.
//
//   client.dll or hw.dll, raw (m_rawinput 1): input is read through
//     SDL_GetRelativeMouseState (both modules import it from SDL2.dll).
//     While locked the original is still called — it drains SDL's internal
//     accumulator, so unlocking does not jump — but reports zero motion.
//     Sven Co-op 5.x samples input in the engine (hw.dll), which is why the
//     engine module is hooked too.
//
//   hw.dll warps: SDL_WarpMouseInWindow (the SDL engine's equivalent of
//     SetCursorPos) is swallowed while locked. While native input owns a
//     virtual UI cursor, a warp moves that cursor instead (EngineInput).
//
// The focus lock keeps the engine active while its window is unfocused, so
// the client would keep sampling and recentring the desktop cursor. While
// the engine is active only because of that lock, these hooks behave as
// locked too (the focus guard).
//
// vgui2/vgui/tier0 are NOT hooked — they legitimately use the cursor for UI
// and debug overlays. SDL exports are cdecl; SDL_Uint32 is SDL's unsigned
// int, declared locally to avoid an SDL header dependency.

namespace
{
	POINT g_lockedPos = { 0, 0 };   // last warp target the game handed us
	bool g_haveLockedPos = false;
	bool g_active = false;

	bool g_hookedGet = false;       // client user32 pair
	bool g_hookedSet = false;
	bool g_hookedRelClient = false; // client SDL relative
	bool g_hookedRelEngine = false; // engine SDL relative

	// Total calls seen through the hooks, lock state aside: which counter
	// grows shows which input path the live game exercises.
	unsigned g_getCalls = 0;
	unsigned g_setCalls = 0;
	unsigned g_relCalls = 0;
	unsigned g_warpCalls = 0;

	// Motion actually delivered by the original SDL call (before the lock
	// zeroes it): nonzero counts mean the physical mouse is reaching the
	// game's input sampling point.
	unsigned g_motionCalls = 0;
	int g_lastDx = 0;
	int g_lastDy = 0;

	typedef BOOL(WINAPI *GetCursorPos_t)(LPPOINT);
	typedef BOOL(WINAPI *SetCursorPos_t)(int, int);
	typedef unsigned int SDL_Uint32;
	typedef SDL_Uint32(__cdecl *SDL_GetRelativeMouseState_t)(int*, int*);
	typedef void(__cdecl *SDL_WarpMouseInWindow_t)(void*, int, int);

	// Always the plain OS/SDL entry points (filled once, before any hook can
	// run), so pass-through never depends on MetaHook having filled the
	// original-pointer output of a hook call.
	GetCursorPos_t g_realGetCursorPos = nullptr;
	SetCursorPos_t g_realSetCursorPos = nullptr;
	SDL_GetRelativeMouseState_t g_realSDLGetRelativeMouseState = nullptr;
	SDL_WarpMouseInWindow_t g_realSDLWarpMouseInWindow = nullptr;

	bool Locked()
	{
		return g_active || FocusLock::ForcingActivation();
	}

	// Window centre through the cached HWND, for the frames before the game
	// has handed us a warp target yet (e.g. lock enabled from the config at
	// startup). The delta the client accumulates is our returned position
	// minus its OWN window_center, so the fallback must mirror the client's
	// computation exactly (GetWindowRect midpoint in screen coordinates) —
	// any constant offset would accumulate per frame.
	bool WindowCentre(POINT& out)
	{
		HWND hwnd = (HWND)WindowManager::GetGameWindow();
		RECT rc;
		if (!hwnd || !GetWindowRect(hwnd, &rc))
			return false;
		out.x = (rc.left + rc.right) / 2;
		out.y = (rc.top + rc.bottom) / 2;
		return true;
	}

	BOOL WINAPI Hooked_GetCursorPos(LPPOINT lpPoint)
	{
		++g_getCalls;
		if (Locked() && lpPoint)
		{
			if (g_haveLockedPos || WindowCentre(g_lockedPos))
			{
				g_haveLockedPos = true;
				*lpPoint = g_lockedPos;
				return TRUE;
			}
		}
		return g_realGetCursorPos(lpPoint);
	}

	BOOL WINAPI Hooked_SetCursorPos(int x, int y)
	{
		++g_setCalls;
		if (Locked())
		{
			g_lockedPos.x = x;
			g_lockedPos.y = y;
			g_haveLockedPos = true;
			return TRUE;
		}
		return g_realSetCursorPos(x, y);
	}

	SDL_Uint32 __cdecl Hooked_SDLGetRelativeMouseState(int* dx, int* dy)
	{
		++g_relCalls;
		// Drain SDL's accumulator even while locked (pass the call through),
		// then report zero motion; the button mask passes through so clicks
		// keep working — the lock is about motion only.
		SDL_Uint32 buttons = g_realSDLGetRelativeMouseState(dx, dy);
		int mx = dx ? *dx : 0;
		int my = dy ? *dy : 0;
		if (mx || my)
		{
			++g_motionCalls;
			g_lastDx = mx;
			g_lastDy = my;
		}
		if (Locked())
		{
			if (dx)
				*dx = 0;
			if (dy)
				*dy = 0;
		}
		return buttons;
	}

	void __cdecl Hooked_SDLWarpMouseInWindow(void* window, int x, int y)
	{
		++g_warpCalls;
		if (EngineInput::WarpVirtualMouse(x, y) || Locked())
			return;
		g_realSDLWarpMouseInWindow(window, x, y);
	}

	// GoldSrc uses the SDL2 ABI, including when sdl2-compat forwards to SDL3.
	// Native SDL3 mouse APIs use float coordinates and cannot use these hooks.
	bool HookSDL(HMODULE module, BlobHandle_t blob, const char* funcName, void* hookFunc)
	{
		return ImportHook::Hook(module, blob, "SDL2.dll", funcName, hookFunc);
	}
}

namespace InputLock
{
	void InstallHooks()
	{
		// Runs inside MetaHook's hook transaction (LoadClient is bracketed by
		// MH_TransactionHookBegin/Commit), which defers the actual IAT write;
		// either way the patch is in place before any client code runs.
		if (!g_realGetCursorPos)
		{
			HMODULE user32 = GetModuleHandleA("user32.dll");
			g_realGetCursorPos = (GetCursorPos_t)GetProcAddress(user32, "GetCursorPos");
			g_realSetCursorPos = (SetCursorPos_t)GetProcAddress(user32, "SetCursorPos");
			HMODULE sdl = GetModuleHandleA("SDL2.dll");
			if (sdl)
			{
				g_realSDLGetRelativeMouseState = (SDL_GetRelativeMouseState_t)GetProcAddress(sdl, "SDL_GetRelativeMouseState");
				g_realSDLWarpMouseInWindow = (SDL_WarpMouseInWindow_t)GetProcAddress(sdl, "SDL_WarpMouseInWindow");
			}
		}

		HMODULE client = g_pMetaHookAPI->GetClientModule();
		BlobHandle_t clientBlob = client ? nullptr : g_pMetaHookAPI->GetBlobClientModule();
		bool hookedGet = ImportHook::Hook(client, clientBlob, "user32.dll", "GetCursorPos", Hooked_GetCursorPos);
		bool hookedSet = ImportHook::Hook(client, clientBlob, "user32.dll", "SetCursorPos", Hooked_SetCursorPos);
		bool hookedRelClient = HookSDL(client, clientBlob, "SDL_GetRelativeMouseState", Hooked_SDLGetRelativeMouseState);
		g_hookedGet |= hookedGet;
		g_hookedSet |= hookedSet;
		g_hookedRelClient |= hookedRelClient;

		// The engine module is loaded long before LoadClient and never
		// reloads, but the same idempotent slot check applies.
		HMODULE engine = g_pMetaHookAPI->GetEngineModule();
		BlobHandle_t engineBlob = engine ? nullptr : g_pMetaHookAPI->GetBlobEngineModule();
		g_hookedRelEngine |= HookSDL(engine, engineBlob, "SDL_GetRelativeMouseState", Hooked_SDLGetRelativeMouseState);
		HookSDL(engine, engineBlob, "SDL_WarpMouseInWindow", Hooked_SDLWarpMouseInWindow);

		// A freshly loaded client has a different window/layout from any
		// recorded warp target, so drop the stale one.
		if (hookedGet || hookedSet || hookedRelClient)
			g_haveLockedPos = false;
	}

	void SetActive(bool active)
	{
		g_active = active;
	}

	bool GetActive()
	{
		return g_active;
	}

	bool HooksInstalled()
	{
		// The lock counts as installed when at least one view-driving path is
		// hooked: the non-raw user32 pair, or any of the raw SDL readers.
		return (g_hookedGet && g_hookedSet) || g_hookedRelClient || g_hookedRelEngine;
	}

	void GetHookStats(unsigned& getCalls, unsigned& setCalls, unsigned& relCalls, unsigned& warpCalls,
		unsigned& motionCalls, int& lastDx, int& lastDy)
	{
		getCalls = g_getCalls;
		setCalls = g_setCalls;
		relCalls = g_relCalls;
		warpCalls = g_warpCalls;
		motionCalls = g_motionCalls;
		lastDx = g_lastDx;
		lastDy = g_lastDy;
	}

	void Shutdown()
	{
		// The hooks pass through while inactive; simply drop the lock so
		// shutdown never leaves it engaged.
		g_active = false;
	}
}
