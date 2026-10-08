#include "input/engine_input.h"

#include "input/input_state.h"
#include "util/import_hook.h"
#include "window/window_manager.h"

#include <metahook.h>
#include <keydefs.h>

#include <windows.h>

#include <cctype>
#include <climits>
#include <atomic>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace
{
	constexpr int MAX_KEYNUM = 255;
	constexpr int METAHOOK_API_GAMEDATA = 109;
	constexpr unsigned SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301;
	constexpr unsigned SDL_MOUSEMOTION = 0x400, SDL_MOUSEBUTTONDOWN = 0x401;
	constexpr unsigned SDL_MOUSEBUTTONUP = 0x402, SDL_MOUSEWHEEL = 0x403;
	constexpr unsigned SDL_INPUT_END = 0x500;
	constexpr int MOUSE_BUTTONS_MASK = 31;
	constexpr int MOUSE_BUTTONS_COUNT = 5;
	// SDL button numbers in mask bit order (left, right, middle, mouse4,
	// mouse5), as CGame builds its mouse state.
	const unsigned char SDL_BUTTONS[MOUSE_BUTTONS_COUNT] = { 1, 3, 2, 4, 5 };
	// Our injected mouse events carry this in `which`, so a queued motion is
	// still recognized when it is dispatched; the low bits sequence motions.
	constexpr unsigned INJECTED_MOUSE_ID = 0x48000000, INJECTED_MOUSE_ID_MASK = 0xFF000000;

	// SDL2's stable event ABI. No SDL import library is needed. The wheel
	// structure grew in later SDL2 versions, but its initial fields did not.
	union SDLEvent
	{
		unsigned type;
		struct { unsigned type, timestamp, windowID; unsigned char state, repeat, padding[2];
			int scancode, sym; unsigned short mod; unsigned unused; } key;
		struct { unsigned type, timestamp, windowID, which, state; int x, y, xrel, yrel; } motion;
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
		int(__cdecl* SDL_HasEvent)(unsigned) = nullptr;
		int(__fastcall* CGame_WindowProc)(void*, int, HWND, UINT, WPARAM, LPARAM) = nullptr;
	} gPrivateFuncs;
	// SDL entry points behind the engine import hooks. Filled once and never
	// cleared: the hooked engine imports may still run after OnExitGame.
	struct RealFuncs
	{
		unsigned(__cdecl* SDL_GetMouseState)(int*, int*) = nullptr;
		int(__cdecl* SDL_PollEvent)(SDLEvent*) = nullptr;
		int(__cdecl* SDL_WaitEventTimeout)(SDLEvent*, int) = nullptr;
	} gRealFuncs;
	// These imports belong to the UI/engine, not the client's view sampler.
	// Keep screen coordinates at the user32 boundary and client pixels in MouseState.
	struct LegacyFuncs
	{
		decltype(&::GetCursorPos) GetCursorPos = ::GetCursorPos;
		decltype(&::SetCursorPos) SetCursorPos = ::SetCursorPos;
		decltype(&::ScreenToClient) ScreenToClient = ::ScreenToClient;
		decltype(&::ClientToScreen) ClientToScreen = ::ClientToScreen;
	} gLegacyFuncs;
	bool g_hookedLegacyGet = false, g_hookedLegacySet = false;
	bool g_legacyLoadNotificationRegistered = false;
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

	// Virtual UI cursor: injected motion owns it until allowed physical
	// motion takes over. Pending motions keep the geometry they were queued
	// with, so a resize before dispatch rebases them.
	InputState::MouseState g_mouse;
	std::unordered_map<unsigned, InputState::MouseMotion> g_pendingMotions;
	unsigned g_motionSequence = 0;
	void* g_mouseWindow = nullptr;
	// The engine imports through which it reads, warps and dispatches the
	// cursor. Accumulated like InputLock's flags: a repeated Install finds
	// the slots already ours.
	bool g_hookedMouseState = false, g_hookedPollEvent = false, g_hookedWaitEvent = false;
	// Set while the engine dispatches our injected motion: its recentring
	// warp must not discard the virtual move.
	thread_local bool g_dispatchingInjectedMotion = false;

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

	bool SyncMouseGeometry();

	int __fastcall HookedWindowProc(void* self, int, HWND window, UINT msg, WPARAM wp, LPARAM lp)
	{
		// Capture the real CGame instance; only the function needs gamedata.
		g_game = self;
		bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
		bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
			(g_legacyWheelMessage && msg == g_legacyWheelMessage);
		if ((keyboard || mouse) && BlockEvent(keyboard))
			return 0;
		if (msg == WM_MOUSEMOVE && SyncMouseGeometry())
			g_mouse.ObserveMotion({ (short)LOWORD(lp), (short)HIWORD(lp) }, false, false);
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

	void HookEngineMouseImports();
	void HookLegacyMouseImports();

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
			gPrivateFuncs.SDL_HasEvent = (decltype(gPrivateFuncs.SDL_HasEvent))GetProcAddress(sdl, "SDL_HasEvent");
			gRealFuncs.SDL_GetMouseState = (decltype(gRealFuncs.SDL_GetMouseState))GetProcAddress(sdl, "SDL_GetMouseState");
			gRealFuncs.SDL_PollEvent = (decltype(gRealFuncs.SDL_PollEvent))GetProcAddress(sdl, "SDL_PollEvent");
			gRealFuncs.SDL_WaitEventTimeout = (decltype(gRealFuncs.SDL_WaitEventTimeout))GetProcAddress(sdl, "SDL_WaitEventTimeout");
			if (!gPrivateFuncs.SDL_SetEventFilter || !gPrivateFuncs.SDL_GetEventFilter ||
				!gPrivateFuncs.SDL_PushEvent || !gPrivateFuncs.SDL_GetKeyFromScancode || !gPrivateFuncs.SDL_HasEvent ||
				!gRealFuncs.SDL_GetMouseState || !gRealFuncs.SDL_PollEvent || !gRealFuncs.SDL_WaitEventTimeout)
			{
				g_engineHooksError = "SDL2 input exports missing";
				return;
			}
			gPrivateFuncs.SDL_GetEventFilter(&g_previousFilter, &g_previousFilterData);
			gPrivateFuncs.SDL_SetEventFilter(FilterSDLEvent, nullptr);
			g_sdlFilterInstalled = true;
			HookEngineMouseImports();
		}
		else
		{
			if (g_pInterface->MetaHookAPIVersion < METAHOOK_API_GAMEDATA)
			{
				g_engineHooksError = "CGame_WindowProc needs MetaHook API 109 gamedata";
				return;
			}
			void* proc = nullptr;
			if (!Resolve("CGame::WindowProc", MH_GAMESYMBOL_KIND_FUNCTION, &proc))
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

	unsigned SDLButtonState()
	{
		unsigned state = 0;
		for (int i = 0; i < MOUSE_BUTTONS_COUNT; ++i)
			if (g_mouseButtons & (1 << i))
				state |= 1u << (SDL_BUTTONS[i] - 1);
		return state;
	}

	// Refresh the cursor geometry: the game window's client size and the
	// video mode size (what a screenshot captures). A recreated window drops
	// the virtual cursor and every pending motion.
	bool SyncMouseGeometry()
	{
		void* window = WindowManager::GetGameWindow();
		InputState::MouseGeometry geometry;
		if (!window || !WindowManager::GetClientSize(geometry.windowWidth, geometry.windowHeight))
		{
			g_engineHooksError = "game window unavailable";
			return false;
		}
		if (g_mouseWindow != window)
		{
			g_mouse.Reset();
			g_pendingMotions.clear();
			g_mouseWindow = window;
		}
		g_pMetaHookAPI->GetVideoMode(&geometry.imageWidth, &geometry.imageHeight, nullptr, nullptr);
		if (!g_mouse.SetGeometry(geometry))
		{
			g_engineHooksError = "game image or window has no valid size";
			return false;
		}
		// WM_MOUSEMOVE and button LPARAMs contain signed 16-bit client pixels.
		if (!g_sdlFilterInstalled && (geometry.windowWidth > SHRT_MAX + 1 || geometry.windowHeight > SHRT_MAX + 1))
		{
			g_engineHooksError = "game window exceeds Win32 mouse coordinate range";
			return false;
		}
		if (!g_mouse.HasVirtualPosition())
		{
			int x = 0, y = 0;
			if (g_sdlFilterInstalled)
				gRealFuncs.SDL_GetMouseState(&x, &y);
			else
			{
				POINT point{};
				if (!gLegacyFuncs.GetCursorPos(&point) || !gLegacyFuncs.ScreenToClient((HWND)window, &point))
				{
					g_engineHooksError = "cannot read game cursor position";
					return false;
				}
				x = point.x;
				y = point.y;
			}
			g_mouse.Seed({ x, y });
			if (g_block.load())
				g_mouse.KeepVirtualPosition();
		}
		return true;
	}

	// Each backend needs both dispatch and cursor read/warp interception.
	bool VirtualMouseReady()
	{
		if (!g_sdlFilterInstalled)
		{
			if (g_windowHook && g_hookedLegacyGet && g_hookedLegacySet)
				return true;
			g_engineHooksError = "engine Win32 cursor imports not hooked";
		}
		else if (!g_hookedMouseState || !g_hookedPollEvent || !g_hookedWaitEvent)
			g_engineHooksError = "engine SDL mouse imports not hooked";
		else
			return true;
		return false;
	}

	BOOL WINAPI Hooked_LegacyGetCursorPos(LPPOINT point)
	{
		if (point && g_windowHook && SyncMouseGeometry() && g_mouse.HasVirtualPosition())
		{
			auto position = g_mouse.WindowPosition();
			POINT screen{ position.x, position.y };
			if (!gLegacyFuncs.ClientToScreen((HWND)WindowManager::GetGameWindow(), &screen))
				return FALSE;
			*point = screen;
			return TRUE;
		}
		return gLegacyFuncs.GetCursorPos(point);
	}

	BOOL WINAPI Hooked_LegacySetCursorPos(int x, int y)
	{
		// Legacy engines also recentre outside WindowProc, on subsequent frames.
		// Keep the injected UI position until an allowed physical motion takes over.
		// A transient invalid window size must not let a recentre reach user32.
		if (g_windowHook && (g_mouse.HasVirtualPosition() ||
			(SyncMouseGeometry() && g_mouse.HasVirtualPosition())))
			return TRUE;
		return gLegacyFuncs.SetCursorPos(x, y);
	}

	void HookLegacyUIMouseImports(HMODULE module, BlobHandle_t blob)
	{
		ImportHook::Hook(module, blob, "user32.dll", "GetCursorPos", Hooked_LegacyGetCursorPos);
		ImportHook::Hook(module, blob, "user32.dll", "SetCursorPos", Hooked_LegacySetCursorPos);
	}

	void LegacyDllLoaded(mh_load_dll_notification_context_t* context)
	{
		if (!(context->flags & LOAD_DLL_NOTIFICATION_IS_LOAD) || !context->BaseDllName)
			return;
		for (const wchar_t* name : { L"vgui.dll", L"vgui2.dll", L"GameUI.dll" })
		{
			if (!_wcsicmp(context->BaseDllName, name))
			{
				// Use the notified module directly; do not load DLLs or query game
				// state under the loader lock. MetaHook commits this hook transaction.
				HookLegacyUIMouseImports(context->hModule, context->hBlob);
				return;
			}
		}
	}

	void HookLegacyMouseImports()
	{
		HMODULE engine = g_pMetaHookAPI->GetEngineModule();
		BlobHandle_t blob = engine ? nullptr : g_pMetaHookAPI->GetBlobEngineModule();
		g_hookedLegacyGet |= ImportHook::Hook(engine, blob, "user32.dll", "GetCursorPos", Hooked_LegacyGetCursorPos);
		g_hookedLegacySet |= ImportHook::Hook(engine, blob, "user32.dll", "SetCursorPos", Hooked_LegacySetCursorPos);
		// VGUI can query user32 directly, bypassing the engine's surface API.
		// Client imports stay with InputLock: returning UI pixels there would
		// feed a nonzero delta to IN_Accumulate every frame.
		for (const char* name : { "vgui.dll", "vgui2.dll", "GameUI.dll" })
		{
			HMODULE module = GetModuleHandleA(name);
			if (!module) continue;
			HookLegacyUIMouseImports(module, nullptr);
		}
	}

	bool PushMouseMotion(InputState::MousePosition target, InputState::MousePosition delta)
	{
		// SDL mode switches may flush the queue: once no motion is queued,
		// no pending motion can still arrive.
		if (!gPrivateFuncs.SDL_HasEvent(SDL_MOUSEMOTION))
			g_pendingMotions.clear();
		SDLEvent event{};
		event.motion.type = SDL_MOUSEMOTION;
		do
			event.motion.which = INJECTED_MOUSE_ID | (++g_motionSequence & ~INJECTED_MOUSE_ID_MASK);
		while (g_pendingMotions.count(event.motion.which));
		event.motion.state = SDLButtonState();
		event.motion.x = target.x;
		event.motion.y = target.y;
		event.motion.xrel = delta.x;
		event.motion.yrel = delta.y;
		g_pendingMotions.emplace(event.motion.which, InputState::MouseMotion{ target, delta, g_mouse.Geometry() });
		if (PushEvent(event))
			return true;
		g_pendingMotions.erase(event.motion.which);
		return false;
	}

	bool IsInjectedMouseMotion(const SDLEvent& event)
	{
		return event.type == SDL_MOUSEMOTION && (event.motion.which & INJECTED_MOUSE_ID_MASK) == INJECTED_MOUSE_ID;
	}

	// A motion the engine is about to dispatch: rebase queued injection,
	// let allowed physical motion take the cursor over, and reject blocked
	// physical or obsolete injected motion.
	bool ObserveMouseMotion(SDLEvent& event)
	{
		bool injected = IsInjectedMouseMotion(event);
		if (!SyncMouseGeometry())
			return !injected;
		if (injected)
		{
			auto pending = g_pendingMotions.find(event.motion.which);
			if (pending == g_pendingMotions.end())
				return false;
			auto motion = pending->second.Rebase(g_mouse.Geometry());
			g_pendingMotions.erase(pending);
			event.motion.x = motion.position.x;
			event.motion.y = motion.position.y;
			event.motion.xrel = motion.delta.x;
			event.motion.yrel = motion.delta.y;
		}
		if (!g_mouse.ObserveMotion({ event.motion.x, event.motion.y }, injected, g_block.load()))
			return false;
		if (injected)
		{
			auto position = g_mouse.WindowPosition();
			event.motion.x = position.x;
			event.motion.y = position.y;
		}
		return true;
	}

	// VGUI1/VGUI2 cursor queries and the HUD mouse position all read
	// SDL_GetMouseState in the engine; the engine converts the window
	// position to UI coordinates itself.
	unsigned __cdecl Hooked_SDLGetMouseState(int* x, int* y)
	{
		unsigned buttons = gRealFuncs.SDL_GetMouseState(x, y);
		if (g_sdlFilterInstalled && SyncMouseGeometry() && g_mouse.HasVirtualPosition())
		{
			auto position = g_mouse.WindowPosition();
			if (x) *x = position.x;
			if (y) *y = position.y;
		}
		return buttons;
	}

	// Post-process an event the engine is about to dispatch; false drops it.
	bool AcceptEvent(SDLEvent& event)
	{
		if (!g_sdlFilterInstalled || event.type != SDL_MOUSEMOTION)
			return true;
		if (!ObserveMouseMotion(event))
			return false;
		g_dispatchingInjectedMotion = IsInjectedMouseMotion(event);
		return true;
	}

	// CGame::SleepUntilInput waits for one event, polls the rest and
	// dispatches each inline, so these imports are the dispatch point.
	int __cdecl Hooked_SDLPollEvent(SDLEvent* event)
	{
		g_dispatchingInjectedMotion = false;
		int result;
		do
			result = gRealFuncs.SDL_PollEvent(event);
		while (result > 0 && event && !AcceptEvent(*event));
		return result;
	}

	int __cdecl Hooked_SDLWaitEventTimeout(SDLEvent* event, int timeout)
	{
		g_dispatchingInjectedMotion = false;
		int result = gRealFuncs.SDL_WaitEventTimeout(event, timeout);
		// A dropped event ends the wait; continue with what is already queued.
		if (result > 0 && event && !AcceptEvent(*event))
			return Hooked_SDLPollEvent(event);
		return result;
	}

	// GoldSrc uses the SDL2 ABI, including when sdl2-compat forwards to SDL3.
	void HookEngineMouseImports()
	{
		HMODULE engine = g_pMetaHookAPI->GetEngineModule();
		BlobHandle_t blob = engine ? nullptr : g_pMetaHookAPI->GetBlobEngineModule();
		g_hookedMouseState |= ImportHook::Hook(engine, blob, "SDL2.dll", "SDL_GetMouseState", Hooked_SDLGetMouseState);
		g_hookedPollEvent |= ImportHook::Hook(engine, blob, "SDL2.dll", "SDL_PollEvent", Hooked_SDLPollEvent);
		g_hookedWaitEvent |= ImportHook::Hook(engine, blob, "SDL2.dll", "SDL_WaitEventTimeout", Hooked_SDLWaitEventTimeout);
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

	WPARAM LegacyButtonState(int mask)
	{
		const unsigned flags[] = { MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2 };
		WPARAM state = 0;
		for (int i = 0; i < MOUSE_BUTTONS_COUNT; ++i)
			if (mask & (1 << i)) state |= flags[i];
		return state;
	}

	bool PushLegacyMouseMotion(InputState::MousePosition target, InputState::MousePosition)
	{
		// WindowProc dispatch is synchronous. Queries made inside it must see
		// the target already; restore the state if dispatch cannot start.
		auto previous = g_mouse;
		g_mouse.ObserveMotion(target, true, false);
		if (CallWindowProc(WM_MOUSEMOVE, LegacyButtonState(g_mouseButtons), MAKELPARAM(target.x, target.y)))
			return true;
		g_mouse = previous;
		return false;
	}

	bool SendButton(int index, bool down)
	{
		const int bit = 1 << index;
		const int mask = down ? g_mouseButtons | bit : g_mouseButtons & ~bit;
		bool ok;
		if (g_sdlFilterInstalled)
		{
			SDLEvent event{};
			event.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			event.button.which = INJECTED_MOUSE_ID;
			event.button.button = SDL_BUTTONS[index];
			event.button.state = down;
			event.button.clicks = 1;
			// Best effort: the button still goes in without a window.
			if (SyncMouseGeometry())
			{
				auto position = g_mouse.WindowPosition();
				event.button.x = position.x;
				event.button.y = position.y;
			}
			ok = PushEvent(event);
		}
		else
		{
			const UINT messages[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN, WM_XBUTTONDOWN, WM_XBUTTONDOWN };
			WPARAM wp = LegacyButtonState(mask);
			if (index >= 3) wp |= (index == 3 ? XBUTTON1 : XBUTTON2) << 16;
			LPARAM position = 0;
			if (g_hookedLegacyGet && g_hookedLegacySet && SyncMouseGeometry())
			{
				auto cursor = g_mouse.WindowPosition();
				position = MAKELPARAM(cursor.x, cursor.y);
			}
			ok = CallWindowProc(messages[index] + (down ? 0 : 1), wp, position);
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
		if (g_windowHook)
		{
			// VGUI may load after LoadClient. Subscribe before the initial sweep
			// so its per-frame cursor polling cannot miss the virtual position.
			if (!g_legacyLoadNotificationRegistered)
			{
				g_pMetaHookAPI->RegisterLoadDllNotificationCallback(LegacyDllLoaded);
				g_legacyLoadNotificationRegistered = true;
			}
			HookLegacyMouseImports();
		}
		ApplyWindowFallback();
	}

	void SetBlockInput(bool block)
	{
		// Freeze the last accepted position before a backend samples blocked motion.
		bool cursorBackend = g_sdlFilterInstalled || (g_windowHook && g_hookedLegacyGet && g_hookedLegacySet);
		if (block && !g_block.load() && cursorBackend && SyncMouseGeometry())
			g_mouse.KeepVirtualPosition();
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
		for (int i = 0; i < MOUSE_BUTTONS_COUNT; ++i)
			if ((buttons ^ g_mouseButtons) & (1 << i))
				if (!SendButton(i, (buttons & (1 << i)) != 0)) return false;
		return true;
	}

	bool MoveMouse(int x, int y, bool relative)
	{
		if (!EngineHooksInstalled())
			return false;
		g_engineHooksError.clear();
		return VirtualMouseReady() && SyncMouseGeometry() &&
			g_mouse.Move(x, y, relative, g_sdlFilterInstalled ? PushMouseMotion : PushLegacyMouseMotion);
	}

	bool GetMousePosition(int& x, int& y)
	{
		if (!EngineHooksInstalled() || !VirtualMouseReady() || !SyncMouseGeometry())
			return false;
		auto position = g_mouse.ScreenPosition();
		x = position.x;
		y = position.y;
		return true;
	}

	bool WarpVirtualMouse(int x, int y)
	{
		if (!g_sdlFilterInstalled)
			return false;
		// The engine recentres after motion; injected motion stays where it was put.
		if (g_dispatchingInjectedMotion)
			return true;
		if (!SyncMouseGeometry() || !g_mouse.HasVirtualPosition())
			return false;
		auto target = g_mouse.Geometry().ClampWindow({ x, y });
		auto current = g_mouse.WindowPosition();
		if (target.x != current.x || target.y != current.y)
			g_mouse.MoveWindow(target, PushMouseMotion);
		// Even a rejected virtual warp must not move the user's desktop cursor.
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
		if (g_legacyLoadNotificationRegistered)
		{
			g_pMetaHookAPI->UnregisterLoadDllNotificationCallback(LegacyDllLoaded);
			g_legacyLoadNotificationRegistered = false;
		}
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
		g_mouse.Reset();
		g_pendingMotions.clear();
		g_mouseWindow = nullptr;
		g_dispatchingInjectedMotion = false;
		g_engineHooksError = "engine exited";
	}

	void Shutdown()
	{
		OnExitGame();
	}
}
