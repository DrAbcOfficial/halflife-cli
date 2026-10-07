#include "util/import_hook.h"

#include <windows.h>

namespace
{
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
}

namespace ImportHook
{
	// The client module is freed and reloaded on every map change, and
	// MetaHook never drops IAT hooks for unloaded modules. Re-checking the
	// slot makes repeated hooking safe: a slot still pointing at our hook
	// (same module) is skipped, a slot the loader rebuilt (fresh module,
	// possibly at the same address) is hooked again. Hooking an
	// already-hooked slot would chain the new hook's "original" to itself
	// and recurse.
	bool Hook(HMODULE module, BlobHandle_t blob, const char* dllName, const char* funcName, void* hookFunc)
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
}
