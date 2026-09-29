#pragma once

#include <metahook.h>
#include <Interface/IPlugins.h>

// metahook.h declares these as extern; the plugin owns the definitions (see exportfuncs.cpp).
// Plugin-private globals live here.
extern cl_exportfuncs_t gExportfuncs;
extern IFileSystem* g_pFileSystem;
extern IFileSystem_HL25* g_pFileSystem_HL25;
extern int g_iEngineType;
extern DWORD g_dwEngineBuildnum;
