// Standalone Win32 regression harness; uses the game's real SDL2.dll and a
// recording CGame::WindowProc trampoline. No game or OS input is generated.
#include "../src/input/engine_input.cpp"
#include <cstdio>
#include <vector>

mh_interface_t* g_pInterface = nullptr;
metahook_api_t* g_pMetaHookAPI = nullptr;
namespace WindowManager
{
	void SetInputDisabled(bool) {}
	void* GetGameWindow() { return (void*)1; }
}

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)
struct Message { UINT type; WPARAM wp; LPARAM lp; };
static std::vector<Message> messages;
static void* expectedGame = (void*)2;
static int previousCalls = 0;
static int previousWheelY = 0;
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
	EngineInput::OnExitGame();
	CHECK(!EngineInput::EngineHooksInstalled());
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
	g_windowHook = nullptr;
	std::printf("engine_input: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
