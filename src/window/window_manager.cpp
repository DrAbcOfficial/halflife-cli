#include "window_manager.h"

#include <windows.h>
#include <string>
#include <vector>

namespace
{
	struct FoundWindow
	{
		HWND hwnd = nullptr;
		long area = 0;
	};

	BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
			return TRUE;

		char cls[64] = { 0 };
		GetClassNameA(hwnd, cls, sizeof(cls));
		// Sven Co-op creates its window through SDL (SDL_app / SDL3Window);
		// classic GoldSrc uses Valve001.
		if (_stricmp(cls, "SDL_app") && _stricmp(cls, "SDL3Window") && _stricmp(cls, "Valve001"))
			return TRUE;

		RECT rc;
		if (!GetWindowRect(hwnd, &rc))
			return TRUE;
		long area = (rc.right - rc.left) * (rc.bottom - rc.top);

		FoundWindow *best = (FoundWindow *)lParam;
		if (!best->hwnd || area > best->area)
		{
			best->hwnd = hwnd;
			best->area = area;
		}
		return TRUE;
	}

	HWND FindGameWindow()
	{
		FoundWindow best;
		EnumWindows(EnumProc, (LPARAM)&best);
		return best.hwnd;
	}

	int g_mode = -1;      // -1 = not configured yet
	bool g_blockInput = false;
	HWND g_hwnd = nullptr;
	WINDOWPLACEMENT g_original = { sizeof(WINDOWPLACEMENT) };
	bool g_savedOriginal = false;
	int g_appliedBlock = -1;  // block state currently applied to g_hwnd (-1 = unknown)
}

namespace WindowManager
{
	void SetMode(int mode)
	{
		g_mode = mode;
		ApplyConfiguredMode();
	}

	int GetMode()
	{
		return g_mode;
	}

	void SetBlockInput(bool block)
	{
		g_blockInput = block;
		ApplyConfiguredMode();
	}

	bool GetBlockInput()
	{
		return g_blockInput;
	}

	void* GetGameWindow()
	{
		if (!IsWindow(g_hwnd))
			g_hwnd = FindGameWindow();
		return g_hwnd;
	}

	// Runs every frame; cheap after the first pass. Re-asserts the hide mode
	// and the input-block state because the engine can recreate the window
	// on mode or video restarts.
	void ApplyConfiguredMode()
	{
		if (g_mode <= 0 && !g_blockInput)
			return;

		if (!IsWindow(g_hwnd))
		{
			g_hwnd = FindGameWindow();
			if (!g_hwnd)
				return;
			g_appliedBlock = -1;  // fresh window handle: re-assert the block state
		}
		if (!g_savedOriginal)
		{
			g_original.length = sizeof(WINDOWPLACEMENT);
			if (GetWindowPlacement(g_hwnd, &g_original))
				g_savedOriginal = true;
		}

		if (g_mode == 1)
		{
			// Move off-screen: OS still renders and composites the window, so
			// snapshot / glReadPixels keep working; it just never appears on
			// any monitor.
			SetWindowPos(g_hwnd, nullptr, -32000, -32000, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
		}
		else if (g_mode == 2)
		{
			ShowWindow(g_hwnd, SW_HIDE);
		}

		if ((g_blockInput ? 1 : 0) != g_appliedBlock)
		{
			// A disabled window ignores all mouse and keyboard input aimed at
			// it; other windows (the CLI console) are unaffected.
			EnableWindow(g_hwnd, g_blockInput ? FALSE : TRUE);
			g_appliedBlock = g_blockInput ? 1 : 0;
		}
	}

	void Restore()
	{
		if (g_hwnd && IsWindow(g_hwnd))
		{
			ShowWindow(g_hwnd, SW_SHOW);
			// Never leave the game window disabled after we are gone.
			EnableWindow(g_hwnd, TRUE);
			if (g_savedOriginal)
				SetWindowPlacement(g_hwnd, &g_original);
		}
		g_hwnd = nullptr;
		g_appliedBlock = -1;
	}
}
