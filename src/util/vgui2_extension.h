#pragma once

#include <metahook.h>
#include <IVGUI2Extension.h>

// The VGUI2Extension plugin's callback registry, or null when
// VGUI2Extension.dll is not loaded or does not serve the interface. It is a
// separate MetaHook plugin (listed before us in plugins.lst), so its factory
// is up once all plugin DLLs are loaded.
inline IVGUI2Extension* FindVGUI2Extension()
{
	HMODULE module = GetModuleHandleA("VGUI2Extension.dll");
	if (!module)
		return nullptr;

	CreateInterfaceFn factory = Sys_GetFactory((HINTERFACEMODULE)module);
	if (!factory)
		return nullptr;

	return (IVGUI2Extension*)factory(VGUI2_EXTENSION_INTERFACE_VERSION, nullptr);
}
