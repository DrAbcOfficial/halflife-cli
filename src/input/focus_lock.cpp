#include "input/focus_lock.h"

#include "input/input_state.h"
#include "window/window_manager.h"

#include <metahook.h>

#include <windows.h>

#include <string>

namespace
{
	constexpr int METAHOOK_API_GAMEDATA = 109;
	// IGame::IsActiveApp is slot 10 of the engine's IGame interface
	// (published as the address-less IGame_IsActiveApp record), so it is
	// called through the CGame instance's own vtable.
	constexpr int IGAME_IS_ACTIVE_APP_SLOT = 10;

	using AppActivate_t = void(__fastcall*)(void*, int, bool);
	using SleepUntilInput_t = void(__fastcall*)(void*, int, int);
	using IsActiveApp_t = bool(__fastcall*)(void*, int);

	// Originals, through the trampolines.
	AppActivate_t g_appActivate = nullptr;
	SleepUntilInput_t g_sleepUntilInput = nullptr;
	hook_t* g_activateHook = nullptr;
	hook_t* g_sleepHook = nullptr;
	void* g_game = nullptr;                  // the engine's CGame, captured on the first frame
	InputState::FocusState g_focus;
	std::string g_error = "not installed yet";

	bool IsActiveApp(void* game)
	{
		auto vtable = *(IsActiveApp_t**)game;
		return vtable[IGAME_IS_ACTIVE_APP_SLOT](game, 0);
	}

	// The engine also activates on SDL_WINDOWEVENT_SHOWN, so an activation
	// is only real while the window has the keyboard focus. Called on the
	// window's thread, where GetFocus reports it.
	bool WindowFocused()
	{
		void* window = WindowManager::GetGameWindow();
		return !window || GetFocus() == (HWND)window;
	}

	// Run the engine's activation side effects only when the effective state
	// changes: repeated focus events under the lock must not ClearIOStates or
	// deactivate the client mouse.
	void ApplyActivation(void* game)
	{
		if (IsActiveApp(game) != g_focus.Active())
			g_appActivate(game, 0, g_focus.Active());
	}

	void __fastcall Hooked_AppActivate(void* game, int, bool active)
	{
		g_game = game;
		g_focus.SetActualActive(active && WindowFocused());
		ApplyActivation(game);
	}

	// The engine starts active and an unfocused (off-screen) window never
	// reports a focus change, so take the instance and the real focus from
	// the main loop's first frame instead of waiting for AppActivate.
	void __fastcall Hooked_SleepUntilInput(void* game, int, int time)
	{
		if (!g_game)
		{
			g_game = game;
			g_focus.SetActualActive(WindowFocused());
			ApplyActivation(game);
		}
		g_sleepUntilInput(game, 0, time);
	}

	template<class T> bool Hook(const char* name, mh_gamesymbol_kind_t kind, T& original, void* replacement, hook_t*& hook)
	{
		void* address = nullptr;
		auto status = g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetEngineBase(), name, kind, &address);
		if (status != MH_GAMESYMBOL_OK || !address)
		{
			g_error = std::string(name) + ": " + g_pMetaHookAPI->GetGameSymbolStatusString(status);
			return false;
		}
		hook = g_pMetaHookAPI->InlineHook(address, replacement, (void**)&original);
		if (!hook)
		{
			g_error = std::string(name) + " inline hook failed";
			return false;
		}
		return true;
	}
}

namespace FocusLock
{
	void Install()
	{
		if (Available())
			return;
		if (g_pInterface->MetaHookAPIVersion < METAHOOK_API_GAMEDATA)
		{
			g_error = "CGame_AppActivate needs MetaHook API 109 gamedata";
			return;
		}
		if (!Hook("CGame_AppActivate", MH_GAMESYMBOL_KIND_FUNCTION, g_appActivate, (void*)Hooked_AppActivate, g_activateHook) ||
			!Hook("CGame::SleepUntilInput", MH_GAMESYMBOL_KIND_VIRTUAL_FUNCTION, g_sleepUntilInput, (void*)Hooked_SleepUntilInput, g_sleepHook))
		{
			OnExitGame();
			return;
		}
		g_error.clear();
	}

	bool Available()
	{
		return g_activateHook && g_sleepHook;
	}

	const char* Error()
	{
		return g_error.c_str();
	}

	void SetActive(bool locked)
	{
		g_focus.SetLocked(locked);
		// Before the first frame the lock is applied with the real focus there.
		if (g_game && Available())
			ApplyActivation(g_game);
	}

	bool GetActive()
	{
		return g_focus.Locked();
	}

	bool EngineActive(bool& active)
	{
		if (!g_game)
			return false;
		active = IsActiveApp(g_game);
		return true;
	}

	bool ForcingActivation()
	{
		return g_game && Available() && g_focus.Locked() && !g_focus.ActualActive();
	}

	void OnExitGame()
	{
		std::string error = g_error;
		for (hook_t** hook : { &g_activateHook, &g_sleepHook })
		{
			if (*hook)
				g_pMetaHookAPI->UnHook(*hook);
			*hook = nullptr;
		}
		g_appActivate = nullptr;
		g_sleepUntilInput = nullptr;
		g_game = nullptr;
		g_focus = {};
		g_error = error.empty() ? "engine exited" : error;
	}

	void Shutdown()
	{
		OnExitGame();
	}
}
