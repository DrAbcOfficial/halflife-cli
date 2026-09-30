#include "input/engine_input.h"

#include "window/window_manager.h"

#include <metahook.h>
#include <keydefs.h>

#include <windows.h>

#include <cctype>
#include <atomic>
#include <cstdlib>
#include <string>

namespace
{
	constexpr int MAX_KEYNUM = 255;
	constexpr int METAHOOK_API_GAMEDATA = 109;
	constexpr unsigned SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301;
	constexpr unsigned SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN = 0x401;
	constexpr unsigned SDL_MOUSEBUTTONUP = 0x402, SDL_MOUSEWHEEL = 0x403;
	constexpr unsigned SDL_INPUT_END = 0x500;
	constexpr int MOUSE_BUTTONS_MASK = 31;

	// SDL2's stable event ABI. No SDL import library is needed. The wheel
	// structure grew in later SDL2 versions, but its initial fields did not.
	union SDLEvent
	{
		unsigned type;
		struct { unsigned type, timestamp, windowID; unsigned char state, repeat, padding[2];
			int scancode, sym; unsigned short mod; unsigned unused; } key;
		struct { unsigned type, timestamp, windowID, which; unsigned char button, state, clicks, padding;
			int x, y; } button;
		struct { unsigned type, timestamp, windowID, which; int x, y; unsigned direction;
			float preciseX, preciseY; int mouseX, mouseY; } wheel;
		unsigned char padding[56];
	};
	static_assert(sizeof(SDLEvent) == 56);
	using SDLFilter = int(__cdecl*)(void*, SDLEvent*);
	struct PrivateFuncs
	{
		void(__cdecl* SDL_SetEventFilter)(SDLFilter, void*) = nullptr;
		int(__cdecl* SDL_GetEventFilter)(SDLFilter*, void**) = nullptr;
		int(__cdecl* SDL_PushEvent)(SDLEvent*) = nullptr;
		int(__cdecl* SDL_GetKeyFromScancode)(int) = nullptr;
		int(__fastcall* CGame_WindowProc)(void*, int, HWND, UINT, WPARAM, LPARAM) = nullptr;
	} gPrivateFuncs;
	bool g_sdlFilterInstalled = false;
	SDLFilter g_previousFilter = nullptr;
	void* g_previousFilterData = nullptr;
	hook_t* g_windowHook = nullptr;
	void* g_game = nullptr;
	std::string g_engineHooksError = "not installed yet";
	std::atomic<bool> g_block{ false };
	std::atomic<unsigned> g_keyEvents{ 0 }, g_keyBlocked{ 0 }, g_mouseEvents{ 0 }, g_mouseBlocked{ 0 };
	// PushEvent runs filters synchronously. sdl2-compat converts/copies the
	// event, so identify our push by thread-local scope rather than pointer.
	thread_local bool g_injecting = false;
	int g_mouseButtons = 0;
	UINT g_legacyWheelMessage = 0;

	bool BlockEvent(bool keyboard)
	{
		++(keyboard ? g_keyEvents : g_mouseEvents);
		if (!g_block.load())
			return false;
		++(keyboard ? g_keyBlocked : g_mouseBlocked);
		return true;
	}

	int __cdecl FilterSDLEvent(void*, SDLEvent* event)
	{
		if (!g_injecting && event->type >= SDL_KEYDOWN && event->type < SDL_INPUT_END &&
			BlockEvent(event->type < SDL_MOUSEMOTION))
			return 0;
		return g_previousFilter ? g_previousFilter(g_previousFilterData, event) : 1;
	}

	int __fastcall HookedWindowProc(void* self, int, HWND window, UINT msg, WPARAM wp, LPARAM lp)
	{
		// Capture the real CGame instance; only the function needs gamedata.
		g_game = self;
		bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
		bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
			(g_legacyWheelMessage && msg == g_legacyWheelMessage);
		if ((keyboard || mouse) && BlockEvent(keyboard))
			return 0;
		return gPrivateFuncs.CGame_WindowProc(self, 0, window, msg, wp, lp);
	}

	bool Resolve(const char* name, mh_gamesymbol_kind_t kind, void** address)
	{
		auto status = g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetEngineBase(), name, kind, address);
		if (status == MH_GAMESYMBOL_OK && *address)
			return true;
		g_engineHooksError = std::string(name) + ": " + g_pMetaHookAPI->GetGameSymbolStatusString(status);
		return false;
	}

	void InstallEngineHooks()
	{
		if (g_sdlFilterInstalled || g_windowHook)
			return;
		char windowClass[64]{};
		GetClassNameA((HWND)WindowManager::GetGameWindow(), windowClass, sizeof(windowClass));
		// A legacy game's client/plugin can load SDL2 independently of its engine.
		HMODULE sdl = _stricmp(windowClass, "Valve001") ? GetModuleHandleW(L"SDL2.dll") : nullptr;
		if (sdl)
		{
			gPrivateFuncs.SDL_SetEventFilter = (decltype(gPrivateFuncs.SDL_SetEventFilter))GetProcAddress(sdl, "SDL_SetEventFilter");
			gPrivateFuncs.SDL_GetEventFilter = (decltype(gPrivateFuncs.SDL_GetEventFilter))GetProcAddress(sdl, "SDL_GetEventFilter");
			gPrivateFuncs.SDL_PushEvent = (decltype(gPrivateFuncs.SDL_PushEvent))GetProcAddress(sdl, "SDL_PushEvent");
			gPrivateFuncs.SDL_GetKeyFromScancode = (decltype(gPrivateFuncs.SDL_GetKeyFromScancode))GetProcAddress(sdl, "SDL_GetKeyFromScancode");
			if (!gPrivateFuncs.SDL_SetEventFilter || !gPrivateFuncs.SDL_GetEventFilter ||
				!gPrivateFuncs.SDL_PushEvent || !gPrivateFuncs.SDL_GetKeyFromScancode)
			{
				g_engineHooksError = "SDL2 input exports missing";
				return;
			}
			gPrivateFuncs.SDL_GetEventFilter(&g_previousFilter, &g_previousFilterData);
			gPrivateFuncs.SDL_SetEventFilter(FilterSDLEvent, nullptr);
			g_sdlFilterInstalled = true;
		}
		else
		{
			if (g_pInterface->MetaHookAPIVersion < METAHOOK_API_GAMEDATA)
			{
				g_engineHooksError = "CGame_WindowProc needs MetaHook API 109 gamedata";
				return;
			}
			void* proc = nullptr;
			if (!Resolve("CGame_WindowProc", MH_GAMESYMBOL_KIND_FUNCTION, &proc))
				return;
			g_legacyWheelMessage = RegisterWindowMessageA("MSWHEEL_ROLLMSG");
			g_windowHook = g_pMetaHookAPI->InlineHook(proc, (void*)HookedWindowProc,
				(void**)&gPrivateFuncs.CGame_WindowProc);
			if (!g_windowHook)
			{
				g_engineHooksError = "CGame_WindowProc inline hook failed";
				return;
			}
		}
		g_engineHooksError.clear();
	}

	void ApplyWindowFallback()
	{
		WindowManager::SetInputDisabled(EngineInput::WindowFallbackActive());
	}

	struct KeyMapping { int key, sdlScan, winScan; };
	// Inverse of SDLKeysymToKeyCode / CGame::MapKey. Win32's bit 8 here
	// denotes the extended scan-code flag (bit 24 of a keyboard LPARAM).
	const KeyMapping KEY_MAPPING[] =
	{
		{ K_ENTER, 40, 0x1c }, { K_ESCAPE, 41, 0x01 }, { K_BACKSPACE, 42, 0x0e },
		{ K_TAB, 43, 0x0f }, { K_SPACE, 44, 0x39 }, { '-', 45, 0x0c }, { '=', 46, 0x0d },
		{ '[', 47, 0x1a }, { ']', 48, 0x1b }, { '\\', 49, 0x2b }, { ';', 51, 0x27 },
		{ '\'', 52, 0x28 }, { '`', 53, 0x29 }, { ',', 54, 0x33 }, { '.', 55, 0x34 },
		{ '/', 56, 0x35 }, { K_CAPSLOCK, 57, 0 }, { K_PAUSE, 72, 0x45 },
		{ K_INS, 73, 0x152 }, { K_HOME, 74, 0x147 }, { K_PGUP, 75, 0x149 },
		{ K_DEL, 76, 0x153 }, { K_END, 77, 0x14f }, { K_PGDN, 78, 0x151 },
		{ K_RIGHTARROW, 79, 0x14d }, { K_LEFTARROW, 80, 0x14b },
		{ K_DOWNARROW, 81, 0x150 }, { K_UPARROW, 82, 0x148 },
		{ K_KP_SLASH, 84, 0x135 }, { '*', 0, 0x37 }, { K_KP_MINUS, 86, 0x4a },
		{ K_KP_PLUS, 87, 0x4e }, { K_KP_ENTER, 88, 0x11c },
		{ K_KP_END, 89, 0x4f }, { K_KP_DOWNARROW, 90, 0x50 }, { K_KP_PGDN, 91, 0x51 },
		{ K_KP_LEFTARROW, 92, 0x4b }, { K_KP_5, 93, 0x4c }, { K_KP_RIGHTARROW, 94, 0x4d },
		{ K_KP_HOME, 95, 0x47 }, { K_KP_UPARROW, 96, 0x48 }, { K_KP_PGUP, 97, 0x49 },
		{ K_KP_INS, 98, 0x52 }, { K_KP_DEL, 99, 0x53 },
		{ K_CTRL, 224, 0x1d }, { K_SHIFT, 225, 0x2a }, { K_ALT, 226, 0x38 },
	};

	KeyMapping MapKey(int key)
	{
		if (key >= 'a' && key <= 'z')
		{
			const int scans[] = { 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
				0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
				0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c };
			return { key, 4 + key - 'a', scans[key - 'a'] };
		}
		if (key >= '0' && key <= '9')
			return { key, key == '0' ? 39 : 30 + key - '1', key == '0' ? 0x0b : 0x02 + key - '1' };
		if (key >= K_F1 && key <= K_F12)
			return { key, 58 + key - K_F1, key <= K_F10 ? 0x3b + key - K_F1 : 0x57 + key - K_F11 };
		for (const auto& mapping : KEY_MAPPING)
			if (mapping.key == key)
				return mapping;
		return { key, 0, 0 };
	}

	bool PushEvent(SDLEvent& event)
	{
		bool previous = g_injecting;
		g_injecting = true;
		int result = gPrivateFuncs.SDL_PushEvent(&event);
		g_injecting = previous;
		if (result != 1)
			g_engineHooksError = result == 0 ? "SDL event filtered" : "SDL event queue rejected input";
		return result == 1;
	}

	bool CallWindowProc(UINT msg, WPARAM wp, LPARAM lp)
	{
		auto window = (HWND)WindowManager::GetGameWindow();
		if (!g_game || !window)
		{
			g_engineHooksError = "game instance or game window unavailable";
			return false;
		}
		gPrivateFuncs.CGame_WindowProc(g_game, 0, window, msg, wp, lp);
		return true;
	}

	bool SendButton(int index, bool down)
	{
		const int bit = 1 << index;
		const int mask = down ? g_mouseButtons | bit : g_mouseButtons & ~bit;
		bool ok;
		if (g_sdlFilterInstalled)
		{
			const unsigned char buttons[] = { 1, 3, 2, 4, 5 };
			SDLEvent event{};
			event.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			event.button.button = buttons[index];
			event.button.state = down;
			event.button.clicks = 1;
			ok = PushEvent(event);
		}
		else
		{
			const UINT messages[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN };
			const unsigned flags[] = { MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2 };
			WPARAM wp = 0;
			for (int i = 0; i < 5; ++i)
				if (mask & (1 << i)) wp |= flags[i];
			if (index >= 3) wp |= (index == 3 ? XBUTTON1 : XBUTTON2) << 16;
			ok = CallWindowProc(messages[index] + (down ? 0 : 1), wp, 0);
		}
		if (ok) g_mouseButtons = mask;
		return ok;
	}

	struct KeyName
	{
		const char* name;
		int key;
	};

	// The engine's keynames[] (keys.c), so names match `bind`. Single
	// characters and decimal keynums are handled separately.
	const KeyName KEY_NAMES[] =
	{
		{ "TAB", K_TAB }, { "ENTER", K_ENTER }, { "ESCAPE", K_ESCAPE }, { "SPACE", K_SPACE },
		{ "BACKSPACE", K_BACKSPACE }, { "UPARROW", K_UPARROW }, { "DOWNARROW", K_DOWNARROW },
		{ "LEFTARROW", K_LEFTARROW }, { "RIGHTARROW", K_RIGHTARROW },
		{ "ALT", K_ALT }, { "CTRL", K_CTRL }, { "SHIFT", K_SHIFT },
		{ "F1", K_F1 }, { "F2", K_F2 }, { "F3", K_F3 }, { "F4", K_F4 }, { "F5", K_F5 }, { "F6", K_F6 },
		{ "F7", K_F7 }, { "F8", K_F8 }, { "F9", K_F9 }, { "F10", K_F10 }, { "F11", K_F11 }, { "F12", K_F12 },
		{ "INS", K_INS }, { "DEL", K_DEL }, { "PGDN", K_PGDN }, { "PGUP", K_PGUP }, { "HOME", K_HOME }, { "END", K_END },
		{ "MOUSE1", K_MOUSE1 }, { "MOUSE2", K_MOUSE2 }, { "MOUSE3", K_MOUSE3 }, { "MOUSE4", K_MOUSE4 }, { "MOUSE5", K_MOUSE5 },
		{ "KP_HOME", K_KP_HOME }, { "KP_UPARROW", K_KP_UPARROW }, { "KP_PGUP", K_KP_PGUP },
		{ "KP_LEFTARROW", K_KP_LEFTARROW }, { "KP_5", K_KP_5 }, { "KP_RIGHTARROW", K_KP_RIGHTARROW },
		{ "KP_END", K_KP_END }, { "KP_DOWNARROW", K_KP_DOWNARROW }, { "KP_PGDN", K_KP_PGDN },
		{ "KP_ENTER", K_KP_ENTER }, { "KP_INS", K_KP_INS }, { "KP_DEL", K_KP_DEL },
		{ "KP_SLASH", K_KP_SLASH }, { "KP_MINUS", K_KP_MINUS }, { "KP_PLUS", K_KP_PLUS },
		{ "CAPSLOCK", K_CAPSLOCK }, { "MWHEELUP", K_MWHEELUP }, { "MWHEELDOWN", K_MWHEELDOWN },
		{ "PAUSE", K_PAUSE },
		{ "SEMICOLON", ';' },   // a raw ';' separates console commands
	};
}

namespace EngineInput
{
	void Install()
	{
		InstallEngineHooks();
		ApplyWindowFallback();
	}

	void SetBlockInput(bool block)
	{
		g_block = block;
		ApplyWindowFallback();
	}

	bool GetBlockInput()
	{
		return g_block;
	}

	bool EngineHooksInstalled()
	{
		return g_sdlFilterInstalled || g_windowHook != nullptr;
	}

	const char* EngineHooksError()
	{
		return g_engineHooksError.c_str();
	}

	bool WindowFallbackActive()
	{
		return g_block && !EngineHooksInstalled();
	}

	int KeyFromName(const char* name)
	{
		if (!name || !*name)
			return -1;

		if (!name[1])
		{
			// A letter key reports its lowercase character, which is also
			// what config.cfg binds.
			return tolower((unsigned char)name[0]);
		}

		char* end = nullptr;
		long number = strtol(name, &end, 10);
		if (*end == '\0')
			return (number >= 0 && number <= MAX_KEYNUM) ? (int)number : -1;

		for (const KeyName& entry : KEY_NAMES)
		{
			if (!_stricmp(name, entry.name))
				return entry.key;
		}
		return -1;
	}

	bool SendKey(int key, bool down)
	{
		if (!EngineHooksInstalled())
			return false;
		g_engineHooksError.clear();
		if (key >= K_MOUSE1 && key <= K_MOUSE5)
			return SendButton(key - K_MOUSE1, down);
		if (key == K_MWHEELUP || key == K_MWHEELDOWN)
		{
			// Native wheel events generate both key edges inside CGame.
			if (!down) return true;
			int direction = key == K_MWHEELUP ? 1 : -1;
			if (!g_sdlFilterInstalled)
				return CallWindowProc(WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(direction * WHEEL_DELTA)), 0);
			SDLEvent event{};
			event.wheel.type = SDL_MOUSEWHEEL;
			event.wheel.y = direction;
			event.wheel.preciseY = (float)direction;
			return PushEvent(event);
		}
		auto mapping = MapKey(key);
		if (g_sdlFilterInstalled ? !mapping.sdlScan : !mapping.winScan)
		{
			g_engineHooksError = "key has no native keyboard mapping on this engine";
			return false;
		}
		if (g_sdlFilterInstalled)
		{
			SDLEvent event{};
			event.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
			event.key.state = down;
			event.key.scancode = mapping.sdlScan;
			event.key.sym = gPrivateFuncs.SDL_GetKeyFromScancode(mapping.sdlScan);
			return PushEvent(event);
		}
		unsigned scan = mapping.winScan & 0xff;
		bool extended = (mapping.winScan & 0x100) != 0;
		WPARAM vk = MapVirtualKeyA(scan | (extended ? 0xe000 : 0), MAPVK_VSC_TO_VK_EX);
		LPARAM lp = 1 | (scan << 16) | (extended ? 1 << 24 : 0);
		if (!down) lp |= (LPARAM)0xc0000000u;
		return CallWindowProc(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
	}

	bool SendMouse(int buttons, bool down)
	{
		if (!EngineHooksInstalled())
			return false;
		g_engineHooksError.clear();
		if (buttons < 0 || (buttons & ~MOUSE_BUTTONS_MASK))
		{
			g_engineHooksError = "invalid mouse button mask";
			return false;
		}
		// The held mask is authoritative; native APIs need individual edges.
		// Keep each successful edge if a later SDL push fails, so retrying works.
		(void)down;
		for (int i = 0; i < 5; ++i)
			if ((buttons ^ g_mouseButtons) & (1 << i))
				if (!SendButton(i, (buttons & (1 << i)) != 0)) return false;
		return true;
	}

	void GetStats(unsigned& keyEvents, unsigned& keyBlocked, unsigned& mouseEvents, unsigned& mouseBlocked)
	{
		keyEvents = g_keyEvents;
		keyBlocked = g_keyBlocked;
		mouseEvents = g_mouseEvents;
		mouseBlocked = g_mouseBlocked;
	}

	void OnExitGame()
	{
		if (g_sdlFilterInstalled)
		{
			SDLFilter current = nullptr;
			void* data = nullptr;
			if (gPrivateFuncs.SDL_GetEventFilter(&current, &data) && current == FilterSDLEvent && !data)
				gPrivateFuncs.SDL_SetEventFilter(g_previousFilter, g_previousFilterData);
		}
		g_sdlFilterInstalled = false;
		g_previousFilter = nullptr;
		g_previousFilterData = nullptr;
		if (g_windowHook)
			g_pMetaHookAPI->UnHook(g_windowHook);
		g_windowHook = nullptr;
		gPrivateFuncs = {};
		g_game = nullptr;
		g_mouseButtons = 0;
		g_injecting = false;
		g_engineHooksError = "engine exited";
	}

	void Shutdown()
	{
		OnExitGame();
	}
}
