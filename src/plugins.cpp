#include "plugins.h"
#include "exportfuncs.h"
#include "config.h"
#include "output_capture.h"
#include "console_bridge.h"
#include "rcon_server.h"
#include "window_manager.h"
#include "usermsg_monitor.h"

#include <metahook.h>
#include <cvardef.h>

#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>
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
	UserMsgMonitor::Shutdown();
}

void IPluginsV4::LoadEngine(cl_enginefunc_t *pEngfuncs)
{
	g_pFileSystem = g_pInterface->FileSystem;
	g_iEngineType = g_pMetaHookAPI->GetEngineType();
	g_dwEngineBuildnum = g_pMetaHookAPI->GetEngineBuildnum();

	memcpy(&gEngfuncs, pEngfuncs, sizeof(gEngfuncs));

	CLI_Config().Load();

	// Console capture registers with the VGUI2Extension plugin (its factory
	// is up as soon as all plugin DLLs are loaded), so it can happen here in
	// LoadEngine — no engine module scanning needed.
	if (CLI_Config().capture)
		OutputCapture::Install();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t *pExportFunc)
{
	memcpy(&gExportfuncs, pExportFunc, sizeof(gExportfuncs));

	pExportFunc->HUD_Init = HUD_Init;
	pExportFunc->HUD_VidInit = HUD_VidInit;
	pExportFunc->HUD_Frame = HUD_Frame;

	CLI_Config().Load();
	if (CLI_Config().usermsg_enabled)
		UserMsgMonitor::Init();
	if (CLI_Config().console)
	{
		ConsoleBridge::Init();
	}
	// Report capture failure only after ConsoleBridge::Init: stdout is wired
	// up there, earlier WriteOut calls would be dropped.
	if (CLI_Config().capture && !OutputCapture::Available())
	{
		// Commands still run; only the output mirroring is lost.
		ConsoleBridge::WriteOut("[halflife-cli] warning: VGUI2Extension.dll missing or incompatible, console output mirroring disabled");
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
	UserMsgMonitor::Shutdown();
}

static void Cmd_CliUserMsg(void)
{
	UserMsgMonitor::CmdUserMsg();
}

static void Cmd_CliHelp(void)
{
	// routed through the hooked engine Con_Printf: doubles as an execution probe
	gEngfuncs.Con_Printf("cli.help executed (halflife-cli)\n");
	ConsoleBridge::WriteOut("halflife-cli commands:");
	ConsoleBridge::WriteOut("  cli.rconinfo        - show RCON endpoint info");
	ConsoleBridge::WriteOut("  cli.window <0|1|2>  - 0=show 1=off-screen(default) 2=SW_HIDE");
	ConsoleBridge::WriteOut("  cli.find <name>     - check cvar/command existence, suggests similar names");
	ConsoleBridge::WriteOut("  cli.usermsg         - UserMsg monitor: on|off|reload|list|pending|<name>");
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

// Plain Levenshtein distance; cvar/command names are short so O(len*len) is fine.
static size_t CliFind_EditDistance(const std::string& a, const std::string& b)
{
	std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
	for (size_t j = 0; j <= b.size(); ++j)
		prev[j] = j;
	for (size_t i = 1; i <= a.size(); ++i)
	{
		cur[0] = i;
		for (size_t j = 1; j <= b.size(); ++j)
		{
			size_t substitution = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
			cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, substitution });
		}
		std::swap(prev, cur);
	}
	return prev[b.size()];
}

static std::string CliFind_Lowercase(const char *s)
{
	std::string out(s ? s : "");
	for (size_t i = 0; i < out.size(); ++i)
		out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

static const size_t CLI_FIND_MAX_SUGGESTIONS = 10;

// cli.find: reports whether a name exists as a cvar or console command, and
// when it does not, suggests similar names (substring match or small edit
// distance). Output goes through Con_Printf so it lands in the captured
// response for piped-stdin and RCON clients, not just the stdout mirror.
static void Cmd_CliFind(void)
{
	if (gEngfuncs.Cmd_Argc() < 2)
	{
		gEngfuncs.Con_Printf("usage: cli.find <name> - check if a cvar/command exists, or suggest similar names\n");
		return;
	}

	std::string query = CliFind_Lowercase(gEngfuncs.Cmd_Argv(1));

	struct Suggestion
	{
		std::string name;
		size_t score;
	};
	std::vector<Suggestion> suggestions;
	std::set<std::string> suggested;
	bool found = false;

	auto consider = [&](const char *rawname) -> bool
	{
		if (!rawname || !*rawname)
			return false;
		std::string name = CliFind_Lowercase(rawname);
		if (name == query)
			return true;

		size_t distance = CliFind_EditDistance(query, name);
		// names are usually prefixed ("sv_", "cl_"); also score same-length
		// windows so a typo inside the name is not inflated by the prefix
		if (name.size() >= query.size())
		{
			for (size_t off = 0; off + query.size() <= name.size(); ++off)
			{
				size_t hamming = 0;
				for (size_t i = 0; i < query.size(); ++i)
					hamming += (name[off + i] != query[i]);
				distance = std::min(distance, hamming);
			}
		}

		bool substring = name.find(query) != std::string::npos;
		// substring hits stay interesting even at a large edit distance
		if (!substring && distance > std::max<size_t>(2, query.size() / 2))
			return false;
		if (suggested.insert(name).second)
			suggestions.push_back({ rawname, distance });
		return false;
	};

	for (cvar_t *cvar = gEngfuncs.GetFirstCvarPtr(); cvar; cvar = cvar->next)
	{
		if (!consider(cvar->name))
			continue;
		found = true;
		// protected cvars (passwords) are masked, like the engine does
		gEngfuncs.Con_Printf("cli.find: \"%s\" exists (cvar, value \"%s\")\n",
			cvar->name, (cvar->flags & FCVAR_PROTECTED) ? "**" : cvar->string);
	}

	for (unsigned int handle = gEngfuncs.GetFirstCmdFunctionHandle(); handle;
		handle = gEngfuncs.GetNextCmdFunctionHandle(handle))
	{
		if (!consider(gEngfuncs.GetCmdFunctionName(handle)))
			continue;
		found = true;
		gEngfuncs.Con_Printf("cli.find: \"%s\" exists (command)\n", gEngfuncs.GetCmdFunctionName(handle));
	}

	if (found)
		return;

	gEngfuncs.Con_Printf("cli.find: no cvar or command named \"%s\"\n", gEngfuncs.Cmd_Argv(1));
	if (suggestions.empty())
	{
		gEngfuncs.Con_Printf("cli.find: no similar names found\n");
		return;
	}

	std::sort(suggestions.begin(), suggestions.end(), [](const Suggestion& a, const Suggestion& b)
	{
		if (a.score != b.score)
			return a.score < b.score;
		return a.name < b.name;
	});
	std::string line = "cli.find: similar names:";
	size_t shown = std::min(suggestions.size(), CLI_FIND_MAX_SUGGESTIONS);
	for (size_t i = 0; i < shown; ++i)
	{
		line += ' ';
		line += suggestions[i].name;
		if (i + 1 < shown)
			line += ',';
	}
	gEngfuncs.Con_Printf("%s\n", line.c_str());
}

void HUD_Init(void)
{
	gExportfuncs.HUD_Init();

	gEngfuncs.pfnAddCommand("cli.help", Cmd_CliHelp);
	gEngfuncs.pfnAddCommand("cli.rconinfo", Cmd_CliRconInfo);
	gEngfuncs.pfnAddCommand("cli.window", Cmd_CliWindow);
	gEngfuncs.pfnAddCommand("cli.find", Cmd_CliFind);
	gEngfuncs.pfnAddCommand("cli.usermsg", Cmd_CliUserMsg);

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
