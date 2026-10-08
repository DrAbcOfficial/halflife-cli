// Standalone Win32 regression harness; uses the game's real SDL2.dll and a
// recording CGame::WindowProc trampoline. No game or OS input is generated.
#include "../src/input/engine_input.cpp"
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

// A 400x300 game window showing an 800x600 video mode: screenshot pixels
// are twice the window's.
static int windowWidth = 400, windowHeight = 300;
static DWORD FakeGetVideoMode(int* width, int* height, int*, bool*)
{
	if (width) *width = 800;
	if (height) *height = 600;
	return VIDEOMODE_OPENGL;
}
static HMODULE FakeGetEngineModule() { return nullptr; }
static BlobHandle_t FakeGetBlobEngineModule() { return nullptr; }
static LoadDllNotificationCallback loadNotification = nullptr;
static int registrations = 0, unregistrations = 0;
static metahook_api_t fakeAPI = [] {
	metahook_api_t api{};
	api.GetVideoMode = FakeGetVideoMode;
	api.GetEngineModule = FakeGetEngineModule;
	api.GetBlobEngineModule = FakeGetBlobEngineModule;
	api.RegisterLoadDllNotificationCallback = [](LoadDllNotificationCallback callback) {
		loadNotification = callback; ++registrations;
	};
	api.UnregisterLoadDllNotificationCallback = [](LoadDllNotificationCallback callback) {
		if (loadNotification == callback) loadNotification = nullptr;
		++unregistrations;
	};
	api.UnHook = [](hook_t*) -> BOOL { return TRUE; };
	return api;
}();
mh_interface_t* g_pInterface = nullptr;
metahook_api_t* g_pMetaHookAPI = &fakeAPI;
namespace WindowManager
{
	void SetInputDisabled(bool) {}
	void* GetGameWindow() { return (void*)1; }
	bool GetClientSize(int& width, int& height) { width = windowWidth; height = windowHeight; return true; }
}
// The engine's imports are absent here; the harness calls the hooks itself.
static bool lateCursorImportsAvailable = false;
struct CursorImport { HMODULE module; const char* name; void* replacement; };
static std::vector<CursorImport> cursorImports;
namespace ImportHook
{
	bool Hook(HMODULE module, BlobHandle_t, const char* dll, const char* name, void* replacement)
	{
		if (!strcmp(dll, "user32.dll")) cursorImports.push_back({ module, name, replacement });
		return lateCursorImportsAvailable && !strcmp(dll, "user32.dll");
	}
}

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)
struct Message { UINT type; WPARAM wp; LPARAM lp; };
static std::vector<Message> messages;
static void* expectedGame = (void*)2;
static int previousCalls = 0;
static int previousWheelY = 0;
static bool inspectLegacyMotion = false;
static int __cdecl PreviousFilter(void* data, SDLEvent* event)
{
	CHECK(data == (void*)3);
	++previousCalls;
	if (event->type == SDL_MOUSEWHEEL)
		previousWheelY = event->wheel.y;
	return event->type != SDL_MOUSEWHEEL;
}
static int __fastcall RecordWindowProc(void* self, int, HWND window, UINT type, WPARAM wp, LPARAM lp)
{
	CHECK(self == expectedGame);
	CHECK(window == (HWND)1);
	messages.push_back({ type, wp, lp });
	if (inspectLegacyMotion && type == WM_MOUSEMOVE)
	{
		POINT cursor{};
		CHECK(Hooked_LegacyGetCursorPos(&cursor));
		CHECK(cursor.x == (short)LOWORD(lp) - 1000 && cursor.y == (short)HIWORD(lp) + 40);
		CHECK(Hooked_LegacySetCursorPos(-800, 190)); // synchronous recentre
	}
	return 123;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 2) return 2;
	std::wstring dllPath = argv[1];
	auto separator = dllPath.find_last_of(L"/\\");
	if (separator != std::wstring::npos)
		SetDllDirectoryW(dllPath.substr(0, separator).c_str());
	HMODULE sdl = LoadLibraryW(argv[1]);
	if (!sdl) { std::printf("Cannot load SDL2.dll: %lu\n", GetLastError()); return 2; }
	auto init = (int(__cdecl*)(unsigned))GetProcAddress(sdl, "SDL_Init");
	auto quit = (void(__cdecl*)())GetProcAddress(sdl, "SDL_Quit");
	auto peep = (int(__cdecl*)(SDLEvent*, int, int, unsigned, unsigned))GetProcAddress(sdl, "SDL_PeepEvents");
	CHECK(init(0x4000 /* SDL_INIT_EVENTS */) == 0);
	EngineInput::Install();
	CHECK(EngineInput::EngineHooksInstalled());
	EngineInput::SetBlockInput(true);
	CHECK(!EngineInput::WindowFallbackActive());

	auto drain = [&]() { SDLEvent events[64]; while (peep(events, 64, 2, 0, 0xffff) > 0) {} };
	auto next = [&]() { SDLEvent event{}; CHECK(peep(&event, 1, 2, 0, 0xffff) == 1); return event; };
	drain();
	SDLEvent physical{};
	physical.key.type = SDL_KEYDOWN;
	physical.key.scancode = 26; // W
	CHECK(gPrivateFuncs.SDL_PushEvent(&physical) == 0);
	SDLEvent empty{};
	CHECK(peep(&empty, 1, 2, 0, 0xffff) == 0);
	CHECK(g_keyBlocked == 1);
	CHECK(EngineInput::SendKey('w', true));
	auto event = next();
	CHECK(event.type == SDL_KEYDOWN && event.key.scancode == 26 && event.key.state == 1);
	CHECK(g_keyBlocked == 1); // injected input bypassed our filter
	CHECK(EngineInput::SendKey('w', false));
	CHECK(next().type == SDL_KEYUP);
	CHECK(!EngineInput::SendKey(K_JOY1, true));
	CHECK(!EngineInput::SendKey(-1, true));
	CHECK(!EngineInput::SendMouse(32, true));
	CHECK(EngineInput::SendMouse(3, true));
	CHECK(next().button.button == 1);
	CHECK(next().button.button == 3);
	CHECK(EngineInput::SendMouse(2, false));
	event = next();
	CHECK(event.type == SDL_MOUSEBUTTONUP && event.button.button == 1);
	CHECK(EngineInput::SendKey(K_MOUSE2, false));
	CHECK(next().button.button == 3);
	CHECK(g_mouseButtons == 0);
	const unsigned physicalMouseEvents = g_mouseEvents;
	for (int key : { K_MWHEELUP, K_MWHEELDOWN, K_MWHEELUP })
	{
		const int direction = key == K_MWHEELUP ? 1 : -1;
		CHECK(EngineInput::SendKey(key, true));
		event = next();
		std::printf("wheel roundtrip: expected=%d type=0x%x y=%d preciseY=%g\n",
			direction, event.type, event.wheel.y, event.wheel.preciseY);
		CHECK(event.type == SDL_MOUSEWHEEL && event.wheel.y == direction);
		CHECK(event.wheel.preciseY == (float)direction);
		CHECK(EngineInput::SendKey(key, false));
		CHECK(peep(&event, 1, 2, 0, 0xffff) == 0); // no duplicate or release event
	}
	CHECK(g_mouseEvents == physicalMouseEvents); // injected wheels bypass block_input
	physical.type = 0x200; // SDL_WINDOWEVENT must pass unchanged
	CHECK(gPrivateFuncs.SDL_PushEvent(&physical) == 1);
	CHECK(next().type == 0x200);
	EngineInput::SetBlockInput(false);
	physical.type = SDL_KEYDOWN;
	CHECK(gPrivateFuncs.SDL_PushEvent(&physical) == 1);
	CHECK(next().type == SDL_KEYDOWN);

	// Virtual UI cursor. Coordinates are 800x600 screenshot pixels on a
	// 400x300 window; the engine dispatches what its SDL_PollEvent /
	// SDL_WaitEventTimeout imports return.
	int mx = -1, my = -1;
	CHECK(!EngineInput::MoveMouse(10, 10, false));
	CHECK(!strcmp(EngineInput::EngineHooksError(), "engine SDL mouse imports not hooked"));
	g_hookedMouseState = g_hookedPollEvent = g_hookedWaitEvent = true;
	drain();
	CHECK(EngineInput::MoveMouse(400, 300, false));
	event = SDLEvent{};
	CHECK(Hooked_SDLPollEvent(&event) == 1);
	std::printf("motion roundtrip: type=0x%x which=0x%x x=%d y=%d xrel=%d yrel=%d\n", event.type,
		event.motion.which, event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel);
	CHECK(event.type == SDL_MOUSEMOTION && IsInjectedMouseMotion(event));
	CHECK(event.motion.x == 200 && event.motion.y == 150);
	CHECK(g_dispatchingInjectedMotion);
	CHECK(EngineInput::WarpVirtualMouse(10, 10)); // the engine's recentre while dispatching
	CHECK(Hooked_SDLPollEvent(&event) == 0);       // ...was swallowed, nothing queued
	CHECK(!g_dispatchingInjectedMotion);
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);
	Hooked_SDLGetMouseState(&mx, &my);
	CHECK(mx == 200 && my == 150);                  // engine queries see window coordinates
	CHECK(EngineInput::MoveMouse(-40, 20, true));
	CHECK(Hooked_SDLPollEvent(&event) == 1 && event.motion.xrel == -20 && event.motion.yrel == 10);
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 360 && my == 320);
	CHECK(EngineInput::MoveMouse(INT_MAX, INT_MIN, true));
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 798 && my == 0); // last representable window pixel
	drain();
	CHECK(Hooked_SDLPollEvent(&event) == 0 && !g_dispatchingInjectedMotion); // the engine's next poll ends dispatch

	// A button carries the virtual position and our device id.
	CHECK(EngineInput::MoveMouse(100, 200, false));
	drain();
	CHECK(EngineInput::SendMouse(1, true));
	event = next();
	CHECK(event.type == SDL_MOUSEBUTTONDOWN && event.button.x == 50 && event.button.y == 100);
	CHECK(EngineInput::SendMouse(0, false));
	drain();

	// A virtual warp outside injected dispatch moves the virtual cursor only.
	CHECK(EngineInput::WarpVirtualMouse(200, 150));
	CHECK(Hooked_SDLPollEvent(&event) == 1 && event.motion.x == 200 && event.motion.y == 150);
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);

	// A queued motion is rebased when the window is resized before dispatch.
	// SDL3 ends each poll cycle at a sentinel event; start a fresh queue.
	drain();
	CHECK(EngineInput::MoveMouse(700, 500, false));
	windowWidth = 800, windowHeight = 600;
	CHECK(Hooked_SDLPollEvent(&event) == 1 && event.motion.x == 700 && event.motion.y == 500);
	CHECK(event.motion.xrel == 300 && event.motion.yrel == 200);
	windowWidth = 400, windowHeight = 300;
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 700 && my == 500);

	// Physical motion queued before the block is dropped; the wait hook then
	// returns the next acceptable event.
	drain();
	SDLEvent motion{};
	motion.motion.type = SDL_MOUSEMOTION;
	motion.motion.x = 5;
	motion.motion.y = 6;
	CHECK(gPrivateFuncs.SDL_PushEvent(&motion) == 1);
	EngineInput::SetBlockInput(true);
	CHECK(gPrivateFuncs.SDL_PushEvent(&motion) == 0); // filtered while blocked
	CHECK(EngineInput::MoveMouse(20, 40, false));
	CHECK(Hooked_SDLWaitEventTimeout(&event, 0) == 1 && IsInjectedMouseMotion(event));
	CHECK(event.motion.x == 10 && event.motion.y == 20);
	CHECK(Hooked_SDLPollEvent(&event) == 0);
	// An injected motion we no longer track (window recreated) is dropped.
	CHECK(EngineInput::MoveMouse(30, 30, false));
	g_pendingMotions.clear();
	CHECK(Hooked_SDLPollEvent(&event) == 0);
	// A rejected push does not move the cursor.
	auto savedMotionPush = gPrivateFuncs.SDL_PushEvent;
	gPrivateFuncs.SDL_PushEvent = [](SDLEvent*) -> int { return -1; };
	CHECK(EngineInput::GetMousePosition(mx, my));
	CHECK(!EngineInput::MoveMouse(500, 500, false));
	int unchangedX = -1, unchangedY = -1;
	CHECK(EngineInput::GetMousePosition(unchangedX, unchangedY) && unchangedX == mx && unchangedY == my);
	CHECK(g_pendingMotions.empty());
	gPrivateFuncs.SDL_PushEvent = savedMotionPush;
	// Allowed physical motion takes the cursor over.
	EngineInput::SetBlockInput(false);
	motion.motion.x = 50;
	motion.motion.y = 60;
	CHECK(gPrivateFuncs.SDL_PushEvent(&motion) == 1);
	CHECK(Hooked_SDLPollEvent(&event) == 1 && !IsInjectedMouseMotion(event) && !g_dispatchingInjectedMotion);
	CHECK(!g_mouse.HasVirtualPosition());
	int realX = -1, realY = -1;
	gRealFuncs.SDL_GetMouseState(&realX, &realY);
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == realX * 2 && my == realY * 2); // follows SDL again
	CHECK(!EngineInput::WarpVirtualMouse(0, 0)); // a physical cursor warps for real

	EngineInput::OnExitGame();
	CHECK(!EngineInput::EngineHooksInstalled());
	CHECK(!EngineInput::MoveMouse(1, 1, false));
	CHECK(!EngineInput::SendKey('w', true));
	auto setFilter = (void(__cdecl*)(SDLFilter, void*))GetProcAddress(sdl, "SDL_SetEventFilter");
	auto getFilter = (int(__cdecl*)(SDLFilter*, void**))GetProcAddress(sdl, "SDL_GetEventFilter");
	setFilter(PreviousFilter, (void*)3);
	EngineInput::Install(); // preserve the existing filter across reinstall
	CHECK(EngineInput::EngineHooksInstalled());
	CHECK(EngineInput::SendKey('a', true));
	CHECK(previousCalls > 0);
	drain();
	CHECK(!EngineInput::SendKey(K_MWHEELUP, true)); // previous filter rejects it
	CHECK(previousWheelY == 1); // the existing filter must also see the correct delta
	CHECK(!EngineInput::SendKey(K_MWHEELDOWN, true));
	CHECK(previousWheelY == -1);
	CHECK(peep(&event, 1, 2, 0, 0xffff) == 0); // rejection must not retry via another API
	CHECK(!g_injecting);
	CHECK(EngineInput::SendMouse(1, true));
	auto savedPush = gPrivateFuncs.SDL_PushEvent;
	gPrivateFuncs.SDL_PushEvent = [](SDLEvent*) -> int { return -1; };
	CHECK(!EngineInput::SendMouse(3, true));
	CHECK(g_mouseButtons == 1); // failure must not advance the held mask
	gPrivateFuncs.SDL_PushEvent = savedPush;
	CHECK(EngineInput::SendMouse(0, false));
	EngineInput::OnExitGame();
	SDLFilter restored = nullptr;
	void* restoredData = nullptr;
	CHECK(getFilter(&restored, &restoredData) && restored == PreviousFilter && restoredData == (void*)3);
	quit();
	FreeLibrary(sdl);
	CHECK(registrations == 0); // SDL must never register the legacy notification

	// Exercise the legacy trampoline path with exactly the CGame ABI.
	g_windowHook = (hook_t*)1;
	g_game = nullptr;
	gPrivateFuncs.CGame_WindowProc = RecordWindowProc;
	EngineInput::SetBlockInput(true);
	CHECK(!EngineInput::SendKey('w', true)); // wait for the first real CGame call
	CHECK(HookedWindowProc(expectedGame, 0, (HWND)1, WM_KEYDOWN, 'W', 0) == 0);
	CHECK(messages.empty());
	CHECK(HookedWindowProc(expectedGame, 0, (HWND)1, WM_MOVE, 0, 0) == 123);
	messages.clear();
	CHECK(EngineInput::SendKey('w', true));
	CHECK(messages.back().type == WM_KEYDOWN && ((messages.back().lp >> 16) & 0xff) == 0x11);
	CHECK(EngineInput::SendKey('w', false));
	CHECK(messages.back().type == WM_KEYUP && ((unsigned)messages.back().lp & 0xc0000000) == 0xc0000000);
	CHECK(EngineInput::SendKey(K_ENTER, true));
	CHECK((messages.back().lp & (1 << 24)) == 0);
	CHECK(EngineInput::SendKey(K_KP_ENTER, true));
	CHECK((messages.back().lp & (1 << 24)) != 0);
	CHECK(EngineInput::SendKey(K_HOME, true));
	CHECK((messages.back().lp & (1 << 24)) != 0);
	CHECK(EngineInput::SendKey(K_KP_HOME, true));
	CHECK((messages.back().lp & (1 << 24)) == 0);
	CHECK(EngineInput::SendMouse(17, true));
	CHECK(messages.back().type == WM_XBUTTONDOWN && HIWORD(messages.back().wp) == XBUTTON2);
	CHECK(LOWORD(messages.back().wp) == (MK_LBUTTON | MK_XBUTTON2));
	CHECK(EngineInput::SendMouse(0, false));
	CHECK(messages.back().type == WM_XBUTTONUP && LOWORD(messages.back().wp) == 0);
	CHECK(EngineInput::SendKey(K_MWHEELDOWN, true));
	CHECK(messages.back().type == WM_MOUSEWHEEL && (short)HIWORD(messages.back().wp) == -WHEEL_DELTA);
	CHECK(!EngineInput::MoveMouse(1, 1, false));
	CHECK(!strcmp(EngineInput::EngineHooksError(), "engine Win32 cursor imports not hooked"));
	// Initial imports plus late UI modules: register once, install only the
	// notified target's cursor pair, and ignore unload/unrelated/client events.
	lateCursorImportsAvailable = true;
	EngineInput::Install();
	EngineInput::Install();
	CHECK(registrations == 1 && loadNotification != nullptr);
	cursorImports.clear();
	mh_load_dll_notification_context_t notification{};
	notification.hModule = (HMODULE)4;
	notification.BaseDllName = L"VGUI2.DLL";
	notification.flags = LOAD_DLL_NOTIFICATION_IS_LOAD | LOAD_DLL_NOTIFICATION_IS_IN_CRIT_REGION;
	loadNotification(&notification);
	CHECK(cursorImports.size() == 2);
	CHECK(cursorImports[0].module == (HMODULE)4 && !strcmp(cursorImports[0].name, "GetCursorPos"));
	CHECK(cursorImports[0].replacement == (void*)Hooked_LegacyGetCursorPos);
	CHECK(cursorImports[1].module == (HMODULE)4 && !strcmp(cursorImports[1].name, "SetCursorPos"));
	CHECK(cursorImports[1].replacement == (void*)Hooked_LegacySetCursorPos);
	notification.flags = LOAD_DLL_NOTIFICATION_IS_UNLOAD;
	loadNotification(&notification);
	notification.flags = LOAD_DLL_NOTIFICATION_IS_LOAD;
	notification.BaseDllName = L"client.dll";
	loadNotification(&notification);
	notification.BaseDllName = nullptr;
	loadNotification(&notification);
	CHECK(cursorImports.size() == 2);
	gLegacyFuncs.GetCursorPos = [](LPPOINT p) -> BOOL { *p = { -970, 90 }; return TRUE; };
	gLegacyFuncs.SetCursorPos = [](int, int) -> BOOL { CHECK(false); return FALSE; };
	gLegacyFuncs.ScreenToClient = [](HWND, LPPOINT p) -> BOOL { p->x += 1000; p->y -= 40; return TRUE; };
	gLegacyFuncs.ClientToScreen = [](HWND, LPPOINT p) -> BOOL { p->x -= 1000; p->y += 40; return TRUE; };
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 60 && my == 100);
	inspectLegacyMotion = true;
	const unsigned legacyEvents = g_mouseEvents, legacyBlocked = g_mouseBlocked;
	CHECK(EngineInput::MoveMouse(400, 300, false));
	CHECK(messages.back().type == WM_MOUSEMOVE && messages.back().lp == MAKELPARAM(200, 150));
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);
	CHECK(Hooked_LegacySetCursorPos(-800, 190)); // later-frame recentre also preserves ownership
	windowWidth = 0; // transient resize/unavailable geometry must not warp the desktop
	CHECK(Hooked_LegacySetCursorPos(-800, 190));
	windowWidth = 400;
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);
	CHECK(EngineInput::SendMouse(17, true));
	CHECK(messages.back().lp == MAKELPARAM(200, 150));
	CHECK(EngineInput::MoveMouse(-40, 20, true));
	CHECK(messages.back().wp == (MK_LBUTTON | MK_XBUTTON2));
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 360 && my == 320);
	CHECK(EngineInput::SendMouse(0, false));
	CHECK(messages.back().lp == MAKELPARAM(180, 160));
	CHECK(EngineInput::MoveMouse(INT_MAX, INT_MIN, true));
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 798 && my == 0);
	CHECK(g_mouseEvents == legacyEvents && g_mouseBlocked == legacyBlocked);
	CHECK(!EngineInput::WindowFallbackActive());
	CHECK(EngineInput::MoveMouse(400, 300, false));
	g_game = nullptr;
	CHECK(!EngineInput::MoveMouse(100, 100, false));
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);
	g_game = expectedGame;
	windowWidth = 800; windowHeight = 600;
	POINT cursor{};
	CHECK(Hooked_LegacyGetCursorPos(&cursor) && cursor.x == -600 && cursor.y == 340);
	windowWidth = 400; windowHeight = 300;
	CHECK(HookedWindowProc(expectedGame, 0, (HWND)1, WM_MOUSEMOVE, 0, MAKELPARAM(10, 20)) == 0);
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 400 && my == 300);
	EngineInput::SetBlockInput(false);
	inspectLegacyMotion = false;
	CHECK(HookedWindowProc(expectedGame, 0, (HWND)1, WM_MOUSEMOVE, 0, MAKELPARAM(10, 20)) == 123);
	CHECK(!g_mouse.HasVirtualPosition());
	CHECK(EngineInput::GetMousePosition(mx, my) && mx == 60 && my == 100);
	// Read/conversion failures must not claim a successful injection.
	auto screenToClient = gLegacyFuncs.ScreenToClient;
	gLegacyFuncs.ScreenToClient = [](HWND, LPPOINT) -> BOOL { return FALSE; };
	auto messageCount = messages.size();
	CHECK(!EngineInput::MoveMouse(100, 100, false));
	CHECK(messages.size() == messageCount && !g_mouse.HasVirtualPosition());
	gLegacyFuncs.ScreenToClient = screenToClient;
	windowWidth = SHRT_MAX + 2;
	CHECK(!EngineInput::MoveMouse(INT_MAX, 100, false));
	CHECK(messages.size() == messageCount);
	windowWidth = 400;
	EngineInput::OnExitGame();
	CHECK(loadNotification == nullptr && unregistrations == 1);
	CHECK(Hooked_LegacyGetCursorPos(&cursor) && cursor.x == -970 && cursor.y == 90);
	EngineInput::OnExitGame();
	CHECK(unregistrations == 1);
	g_windowHook = (hook_t*)1;
	EngineInput::Install();
	CHECK(registrations == 2 && loadNotification != nullptr);
	EngineInput::OnExitGame();
	CHECK(unregistrations == 2 && loadNotification == nullptr);
	std::printf("engine_input: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
