#pragma once

#include <metahook.h>

// Import address table hooks shared by the input modules.
namespace ImportHook
{
	// Hooks dllName!funcName in a normally loaded module (module) or a
	// MetaHook blob module (blob). Repeated calls are safe: a slot already
	// pointing at hookFunc is skipped, a slot the loader rebuilt is hooked
	// again. False when the import is absent, already ours or the hook fails.
	// The hook calls the real export itself (resolved with GetProcAddress),
	// so no original pointer is returned.
	bool Hook(HMODULE module, BlobHandle_t blob, const char* dllName, const char* funcName, void* hookFunc);
}
