#include "console/sys_error.h"

#include "console/console_bridge.h"
#include "console/output_capture.h"

#include <metahook.h>

#include <windows.h>
#include <direct.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{
	constexpr size_t MAX_ERROR_LENGTH = 4096;
	constexpr const char* LOG_FILE_NAME = "errors.log";
	constexpr const char* CONSOLE_PREFIX = "[halflife-cli] sys_error: ";

	// Sys_Error is variadic and cdecl in every GoldSrc engine build.
	using SysErrorFn = void(__cdecl*)(const char* fmt, ...);

	SysErrorFn g_pfnOriginalSysError = nullptr;
	hook_t* g_sysErrorHook = nullptr;
	std::string g_hookError = "not installed yet";

	// The plugin's own data folder, where the config and the port file live.
	std::string DataDirectory()
	{
		const char* gameDir = g_pMetaHookAPI->GetGameDirectory();
		if (!gameDir || !*gameDir)
			return {};
		return std::string(gameDir) + "/metahook/configs/halflifecli";
	}

	// Unbuffered Win32 append, independent of the engine filesystem: this runs
	// on the way to a fatal exit, where the engine may already be tearing its
	// own file system down.
	void AppendLog(const std::string& directory, const std::string& text)
	{
		if (directory.empty())
			return;
		_mkdir(directory.c_str());
		HANDLE file = CreateFileA((directory + "/" + LOG_FILE_NAME).c_str(),
			FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return;
		DWORD written = 0;
		WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
		CloseHandle(file);
	}

	void __cdecl HookedSysError(const char* fmt, ...)
	{
		char message[MAX_ERROR_LENGTH] = {};
		if (fmt)
		{
			va_list args;
			va_start(args, fmt);
			_vsnprintf(message, sizeof(message) - 1, fmt, args);
			va_end(args);
		}

		// Formatted here rather than forwarded: a variadic function cannot pass
		// its va_list on, so the original receives our buffer as data.
		const std::string text(message);
		OutputCapture::Flush();
		ConsoleBridge::WriteOut(CONSOLE_PREFIX + text);
		AppendLog(DataDirectory(), text + "\n");

		// Always hand over to the engine: the dialog, the exit code and the
		// whole shutdown path stay exactly as they were.
		if (g_pfnOriginalSysError)
			g_pfnOriginalSysError("%s", message);
	}
}

namespace SysError
{
	void Install()
	{
		if (g_sysErrorHook)
			return;

		void* proc = nullptr;
		const auto status = g_pMetaHookAPI->ResolveGameSymbol(
			g_pMetaHookAPI->GetEngineBase(), "Sys_Error", MH_GAMESYMBOL_KIND_FUNCTION, &proc);
		if (status != MH_GAMESYMBOL_OK || !proc)
		{
			g_hookError = std::string("Sys_Error: ") + g_pMetaHookAPI->GetGameSymbolStatusString(status);
			return;
		}

		g_sysErrorHook = g_pMetaHookAPI->InlineHook(proc, (void*)HookedSysError, (void**)&g_pfnOriginalSysError);
		if (!g_sysErrorHook)
		{
			g_hookError = "Sys_Error inline hook failed";
			return;
		}
		g_hookError.clear();
	}

	void Shutdown()
	{
		if (g_sysErrorHook)
			g_pMetaHookAPI->UnHook(g_sysErrorHook);
		g_sysErrorHook = nullptr;
		g_pfnOriginalSysError = nullptr;
		g_hookError = "not installed";
	}

	bool Hooked()
	{
		return g_sysErrorHook != nullptr;
	}

	const char* HookError()
	{
		return g_hookError.c_str();
	}
}
