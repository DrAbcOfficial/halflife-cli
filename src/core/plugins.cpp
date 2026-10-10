#include "core/plugins.h"
#include "core/exportfuncs.h"
#include "core/cli_commands.h"
#include "config/config.h"
#include "console/console_bridge.h"
#include "console/output_capture.h"
#include "console/sys_error.h"
#include "input/engine_input.h"
#include "input/focus_lock.h"
#include "input/input_lock.h"
#include "rcon/rcon_server.h"
#include "usermsg/usermsg_monitor.h"
#include "window/window_manager.h"
#include "vgui2/vgui2.h"

#include <metahook.h>

#include <cstdio>
#include <direct.h>
#include <string>

// Plugin lifecycle: MetaHook entry points, engine/client export overrides and
// module wiring. The cli.* console commands live in cli_commands.cpp.

void IPluginsV4::Init(metahook_api_t *pAPI, mh_interface_t *pInterface, mh_enginesave_t *pSave)
{
	g_pInterface = pInterface;
	g_pMetaHookAPI = pAPI;
	g_pMetaSave = pSave;
}

void IPluginsV4::Shutdown(void)
{
	VGUI2::Shutdown();
	RconServer::Shutdown();
	SysError::Shutdown();
	ConsoleBridge::Shutdown();
	OutputCapture::Shutdown();
	InputLock::Shutdown();
	FocusLock::Shutdown();
	EngineInput::Shutdown();
	UserMsgMonitor::Shutdown();
}

void IPluginsV4::LoadEngine(cl_enginefunc_t *pEngfuncs)
{
	g_pFileSystem = g_pInterface->FileSystem;
	g_iEngineType = g_pMetaHookAPI->GetEngineType();
	g_dwEngineBuildnum = g_pMetaHookAPI->GetEngineBuildnum();

	memcpy(&gEngfuncs, pEngfuncs, sizeof(gEngfuncs));

	// Phase 1 needs no filesystem search paths or initialized client. MetaHook
	// commits inline hooks after all plugins' LoadEngine callbacks return.
	ConsoleBridge::InitEarlyOutput();
	SysError::Install();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t *pExportFunc)
{
	VGUI2::Shutdown();
	memcpy(&gExportfuncs, pExportFunc, sizeof(gExportfuncs));

	pExportFunc->HUD_Init = HUD_Init;
	pExportFunc->HUD_VidInit = HUD_VidInit;
	pExportFunc->HUD_Frame = HUD_Frame;

	CLI_Config().Load();
	// Phase 2: apply console policy before any optional backend can fail.
	if (CLI_Config().console)
		ConsoleBridge::Init();
	else
		ConsoleBridge::DisableConsole();

	if (CLI_Config().capture)
		OutputCapture::Install();
	else
		OutputCapture::Shutdown();
	// Report capture failure only after ConsoleBridge::Init: stdout is wired
	// up there, earlier WriteOut calls would be dropped.
	if (CLI_Config().capture && !OutputCapture::Available())
	{
		// Commands still run; only the output mirroring is lost.
		ConsoleBridge::WriteOut("[halflife-cli] warning: VGUI2Extension.dll missing or incompatible, console output mirroring disabled");
	}
	// Retry an early installation failure; successful hooks are idempotent.
	SysError::Install();
	if (!SysError::Hooked())
	{
		ConsoleBridge::WriteOut(std::string("[halflife-cli] warning: Sys_Error not hooked (") +
			SysError::HookError() + "), fatal errors will not be mirrored");
	}
	RconServer::Install();
	if (CLI_Config().usermsg_enabled)
		UserMsgMonitor::Init();
	if (CLI_Config().hide_window)
	{
		WindowManager::SetMode(CLI_Config().hide_window);
	}

	// After gExportfuncs is captured: the client module is fully loaded, so
	// its IAT can be patched. Pass-through while the lock is off.
	InputLock::InstallHooks();
	InputLock::SetActive(CLI_Config().input_lock);

	// Native input is initialized by LoadClient. Pass through while the block is off.
	EngineInput::Install();
	EngineInput::SetBlockInput(CLI_Config().block_input);

	// Applied on the engine's next activation if it has not activated yet.
	FocusLock::Install();
	FocusLock::SetActive(CLI_Config().focus_lock);

	ConsoleBridge::WriteOut("halflife-cli " + std::string(GetVersion()) +
		" loaded (engine: " + g_pMetaHookAPI->GetEngineTypeName() + ")");
	RconServer::OnClientReady();
}

void IPluginsV4::ExitGame(int iResult)
{
	VGUI2::Shutdown();
	RconServer::OnEngineShutdown();
	ConsoleBridge::Shutdown(true);
	InputLock::Shutdown();
	FocusLock::OnExitGame();
	EngineInput::OnExitGame();
	WindowManager::Restore();
	UserMsgMonitor::Shutdown();
}

void HUD_Init(void)
{
	gExportfuncs.HUD_Init();

	CliCommands::RegisterAll();
	RconServer::RegisterCommands();

	if (CLI_Config().developer > 0)
		gEngfuncs.Cvar_SetValue("developer", (float)CLI_Config().developer);

	// Wrap whatever the client DLL registered in its own HUD_Init first, so
	// user messages dispatched this session land in our decoder.
	UserMsgMonitor::OnHudInit();
}

int HUD_VidInit(void)
{
	int result = gExportfuncs.HUD_VidInit();
	// The client DLL may (re-)register usermsg hooks here on every map load;
	// re-assert our wrappers after it is done.
	UserMsgMonitor::OnHudVidInit();
	return result;
}

void HUD_Frame(double time)
{
	WindowManager::ApplyConfiguredMode();
	VGUI2::Frame();
	if (!RconServer::UsesMainFrame())
	{
		ConsoleBridge::PumpCommands();
		// A failed native UDP adapter (or Source TCP) has no Cbuf hook. Report its
		// startup result once the game console is initialized on the first frame.
		RconServer::AfterCommands();
	}
	UserMsgMonitor::Frame();

	gExportfuncs.HUD_Frame(time);
}

static const char completeVersion[] =
{
	BUILD_YEAR_CH0, BUILD_YEAR_CH1, BUILD_YEAR_CH2, BUILD_YEAR_CH3,
	'-',
	BUILD_MONTH_CH0, BUILD_MONTH_CH1,
	'-',
	BUILD_DAY_CH0, BUILD_DAY_CH1,
	'T',
	BUILD_HOUR_CH0, BUILD_HOUR_CH1,
	':',
	BUILD_MIN_CH0, BUILD_MIN_CH1,
	':',
	BUILD_SEC_CH0, BUILD_SEC_CH1,
	'\0'
};

const char *IPluginsV4::GetVersion(void)
{
	return completeVersion;
}

EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);
