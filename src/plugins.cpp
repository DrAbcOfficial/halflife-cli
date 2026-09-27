#include "plugins.h"
#include "exportfuncs.h"
#include "config.h"
#include "output_capture.h"
#include "console_bridge.h"
#include "rcon_server.h"
#include "window_manager.h"

#include <string>
#include <cstdio>

static void WritePortFile(unsigned short port)
{
	FileHandle_t fp = FILESYSTEM_ANY_OPEN("metahook/configs/halflifecli.port", "wb");
	if (!fp)
		return;
	char buf[16];
	int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u\n", port);
	FILESYSTEM_ANY_WRITE(buf, len, fp);
	FILESYSTEM_ANY_CLOSE(fp);
}

void IPluginsV4::Init(metahook_api_t *pAPI, mh_interface_t *pInterface, mh_enginesave_t *pSave)
{
	g_pInterface = pInterface;
	g_pMetaHookAPI = pAPI;
	g_pMetaSave = pSave;
}

void IPluginsV4::Shutdown(void)
{
	RconServer::Shutdown();
	ConsoleBridge::Shutdown();
	OutputCapture::Shutdown();
}

void IPluginsV4::LoadEngine(cl_enginefunc_t *pEngfuncs)
{
	g_pFileSystem = g_pInterface->FileSystem;
	g_iEngineType = g_pMetaHookAPI->GetEngineType();
	g_dwEngineBuildnum = g_pMetaHookAPI->GetEngineBuildnum();

	memcpy(&gEngfuncs, pEngfuncs, sizeof(gEngfuncs));

	CLI_Config().Load();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t *pExportFunc)
{
	memcpy(&gExportfuncs, pExportFunc, sizeof(gExportfuncs));

	pExportFunc->HUD_Init = HUD_Init;
	pExportFunc->HUD_Frame = HUD_Frame;

	CLI_Config().Load();
	// Install output capture here rather than LoadEngine: the engine
	// filesystem backing gamedata/config access is not ready in LoadEngine,
	// and symbol resolution needs the engine module fully loaded anyway.
	if (CLI_Config().capture)
	{
		bool ok = OutputCapture::Install();
		if (!ok)
		{
			// Commands still run; only the output mirroring is lost.
			ConsoleBridge::WriteOut("[halflife-cli] warning: Con_Printf capture unavailable, output mirroring disabled");
		}
	}
	if (CLI_Config().console)
	{
		ConsoleBridge::Init();
	}
	if (CLI_Config().hide_window)
	{
		WindowHide::SetMode(CLI_Config().hide_window);
	}

	if (CLI_Config().rcon)
	{
		RconServer::StartResult r = RconServer::Start(
			CLI_Config().rcon_bind,
			(unsigned short)CLI_Config().rcon_port,
			CLI_Config().rcon_password,
			CLI_Config().rcon_allowed_ips);

		char banner[512];
		if (r.ok)
		{
			// Automation discovers the port here, on stdout, and via the port file.
			_snprintf_s(banner, sizeof(banner), _TRUNCATE,
				"halflife-cli: RCON listening on %s:%u (password: %s)",
				CLI_Config().rcon_bind.c_str(), r.port,
				CLI_Config().rcon_password.empty() ? "none" : "set");
			WritePortFile(r.port);
		}
		else
		{
			_snprintf_s(banner, sizeof(banner), _TRUNCATE,
				"halflife-cli: RCON failed to start (%s)", r.error.c_str());
		}
		ConsoleBridge::WriteOut("halflife-cli " + std::string(GetVersion()) +
			" loaded (engine: " + g_pMetaHookAPI->GetEngineTypeName() + ")");
		ConsoleBridge::WriteOut(banner);
		ConsoleBridge::WriteOut("type a console command and press ENTER; 'cli.help' for plugin commands");
		gEngfuncs.Con_Printf("%s\n", banner);
	}
}

void IPluginsV4::ExitGame(int iResult)
{
	RconServer::Shutdown();
	ConsoleBridge::Shutdown();
	WindowHide::Restore();
}

static void Cmd_CliHelp(void)
{
	// routed through the hooked engine Con_Printf: doubles as an execution probe
	gEngfuncs.Con_Printf("cli.help executed (halflife-cli)\n");
	ConsoleBridge::WriteOut("halflife-cli commands:");
	ConsoleBridge::WriteOut("  cli.rconinfo        - show RCON endpoint info");
	ConsoleBridge::WriteOut("  cli.window <0|1|2>  - 0=show 1=off-screen(default) 2=SW_HIDE");
	ConsoleBridge::WriteOut("  cli.help            - this help");
	ConsoleBridge::WriteOut("any other line is executed as a game console command (e.g. 'status', 'snapshot')");
}

static void Cmd_CliRconInfo(void)
{
	char buf[256];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"RCON bind=%s port=%u password=%s",
			CLI_Config().rcon_bind.c_str(), RconServer::CurrentPort(),
		CLI_Config().rcon_password.empty() ? "none" : "set");
	ConsoleBridge::WriteOut(buf);
}

static void Cmd_CliWindow(void)
{
	if (gEngfuncs.Cmd_Argc() < 2)
	{
		char buf[128];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "cli.window mode = %d (0=show 1=offscreen 2=hide)", WindowHide::GetMode());
		ConsoleBridge::WriteOut(buf);
		return;
	}
	WindowHide::SetMode(atoi(gEngfuncs.Cmd_Argv(1)));
	ConsoleBridge::WriteOut("cli.window applied");
}

void HUD_Init(void)
{
	gExportfuncs.HUD_Init();

	gEngfuncs.pfnAddCommand("cli.help", Cmd_CliHelp);
	gEngfuncs.pfnAddCommand("cli.rconinfo", Cmd_CliRconInfo);
	gEngfuncs.pfnAddCommand("cli.window", Cmd_CliWindow);

	if (CLI_Config().developer > 0)
		gEngfuncs.Cvar_SetValue("developer", (float)CLI_Config().developer);
}

static int g_frames = 0;

void HUD_Frame(double time)
{
	if (g_frames < 3)
	{
		char s[32];
		_snprintf_s(s, sizeof(s), _TRUNCATE, "HUD_Frame:%d", g_frames);
	}
	++g_frames;

	WindowHide::ApplyConfiguredMode();
	ConsoleBridge::PumpCommands();

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
