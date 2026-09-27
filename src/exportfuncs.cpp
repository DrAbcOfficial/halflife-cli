#include "plugins.h"
#include "exportfuncs.h"

// Declared extern in metahook.h; every plugin owns one instance (filled by MetahookInit callback).
mh_interface_t* g_pInterface = NULL;
cl_enginefunc_t gEngfuncs;
metahook_api_t* g_pMetaHookAPI = NULL;
mh_enginesave_t* g_pMetaSave = NULL;

cl_exportfuncs_t gExportfuncs = { 0 };
IFileSystem* g_pFileSystem = NULL;
IFileSystem_HL25* g_pFileSystem_HL25 = NULL;

int g_iEngineType = 0;
DWORD g_dwEngineBuildnum = 0;

void HUD_Frame(double time)
{
	gExportfuncs.HUD_Frame(time);
}
