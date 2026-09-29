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
	HWND g_hwnd = nullptr;
	WINDOWPLACEMENT g_original = { sizeof(WINDOWPLACEMENT) };
	bool g_savedOriginal = false;
}

namespace WindowHide
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

	void ApplyConfiguredMode()
	{
		if (g_mode < 0)
			return;
		if (g_mode == 0)
			return;

		if (!IsWindow(g_hwnd))
		{
			g_hwnd = FindGameWindow();
			if (!g_hwnd)
				return;
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
	}

	void Restore()
	{
		if (g_hwnd && IsWindow(g_hwnd))
		{
			ShowWindow(g_hwnd, SW_SHOW);
			if (g_savedOriginal)
				SetWindowPlacement(g_hwnd, &g_original);
		}
		g_hwnd = nullptr;
	}
}
