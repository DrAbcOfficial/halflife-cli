#include "input/input_lock.h"

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
//     SetCursorPos) is swallowed while locked.
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
		if (g_active && lpPoint)
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
		if (g_active)
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
		if (g_active)
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
		if (g_active)
			return;
		g_realSDLWarpMouseInWindow(window, x, y);
	}

	// IAT slot of moduleName!funcName in the PE mapped at base, or null when
	// not imported. Works for normally loaded modules (base = HMODULE) and
	// memory-mapped blob modules alike.
	void** FindImportSlot(void* base, const char* moduleName, const char* funcName)
	{
		auto dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		auto nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
			return nullptr;
		auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress)
			return nullptr;
		for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + dir.VirtualAddress); desc->Name; ++desc)
		{
			const char* name = (const char*)((BYTE*)base + desc->Name);
			if (_stricmp(name, moduleName))
				continue;
			// name thunks and IAT thunks run in parallel
			ULONG_PTR namesRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
			auto names = (IMAGE_THUNK_DATA*)((BYTE*)base + namesRva);
			auto iat = (IMAGE_THUNK_DATA*)((BYTE*)base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				auto byName = (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + names->u1.AddressOfData);
				if (!_stricmp((const char*)byName->Name, funcName))
					return (void**)&iat->u1.Function;
			}
		}
		return nullptr;
	}

	// The client module is freed and reloaded on every map change, and
	// MetaHook never drops IAT hooks for unloaded modules. Re-checking the
	// slot makes repeated InstallHooks safe: a slot still pointing at our
	// hook (same module) is skipped, a slot the loader rebuilt (fresh module,
	// possibly at the same address) is hooked again. Hooking an
	// already-hooked slot would chain the new hook's "original" to itself
	// and recurse.
	bool HookOne(HMODULE module, BlobHandle_t blob, const char* dllName, const char* funcName, void* hookFunc)
	{
		void* base = module ? (void*)module
			: (blob ? g_pMetaHookAPI->GetBlobModuleImageBase(blob) : nullptr);
		void** slot = base ? FindImportSlot(base, dllName, funcName) : nullptr;
		if (!slot || *slot == hookFunc)
			return false;
		if (module)
			return g_pMetaHookAPI->IATHook(module, dllName, funcName, hookFunc, nullptr) != nullptr;
		return g_pMetaHookAPI->BlobIATHook(blob, dllName, funcName, hookFunc, nullptr) != nullptr;
	}

	// Games ship against SDL2 or SDL3; hook whichever DLL name the module
	// actually imports from.
	bool HookSDL(HMODULE module, BlobHandle_t blob, const char* funcName, void* hookFunc)
	{
		return HookOne(module, blob, "SDL2.dll", funcName, hookFunc) ||
			HookOne(module, blob, "SDL3.dll", funcName, hookFunc);
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
			if (!sdl)
				sdl = GetModuleHandleA("SDL3.dll");
			g_realSDLGetRelativeMouseState = (SDL_GetRelativeMouseState_t)GetProcAddress(sdl, "SDL_GetRelativeMouseState");
			g_realSDLWarpMouseInWindow = (SDL_WarpMouseInWindow_t)GetProcAddress(sdl, "SDL_WarpMouseInWindow");
		}

		HMODULE client = g_pMetaHookAPI->GetClientModule();
		BlobHandle_t clientBlob = client ? nullptr : g_pMetaHookAPI->GetBlobClientModule();
		bool hookedGet = HookOne(client, clientBlob, "user32.dll", "GetCursorPos", Hooked_GetCursorPos);
		bool hookedSet = HookOne(client, clientBlob, "user32.dll", "SetCursorPos", Hooked_SetCursorPos);
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
