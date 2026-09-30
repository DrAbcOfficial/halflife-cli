#include "core/cli_commands.h"

#include "config/config.h"
#include "console/console_bridge.h"
#include "core/plugins.h"
#include "input/engine_input.h"
#include "input/input_lock.h"
#include "rcon/rcon_server.h"
#include "usermsg/usermsg_monitor.h"
#include "util/text.h"
#include "window/window_manager.h"

#include <metahook.h>
#include <cvardef.h>

#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	void Cmd_CliHelp(void)
	{
		// routed through the hooked engine Con_Printf: doubles as an execution probe
		gEngfuncs.Con_Printf("cli.help executed (halflife-cli)\n");
		ConsoleBridge::WriteOut("halflife-cli commands:");
		ConsoleBridge::WriteOut("  cli.rconinfo        - show RCON endpoint info");
		ConsoleBridge::WriteOut("  cli.window <0|1|2>  - 0=show 1=off-screen(default) 2=SW_HIDE");
		ConsoleBridge::WriteOut("  cli.inputlock on|off - lock the mouse cursor so it stops driving the view");
		ConsoleBridge::WriteOut("  cli.blockinput on|off - make the game ignore the physical keyboard and mouse buttons");
		ConsoleBridge::WriteOut("  cli.trapkey <key> <0|1> - send a key event through native engine input");
		ConsoleBridge::WriteOut("  cli.trapmouse <buttons> <0|1> - send a mouse button event through native engine input");
		ConsoleBridge::WriteOut("  cli.find <name>     - check cvar/command existence, suggests similar names");
		ConsoleBridge::WriteOut("  cli.usermsg         - UserMsg monitor: on|off|reload|list|pending|<name>");
		ConsoleBridge::WriteOut("  cli.help            - this help");
		ConsoleBridge::WriteOut("any other line is executed as a game console command (e.g. 'status', 'snapshot')");
	}

	void Cmd_CliRconInfo(void)
	{
		char buf[256];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"RCON bind=%s port=%u password=%s",
			CLI_Config().rcon_bind.c_str(), RconServer::CurrentPort(),
			CLI_Config().rcon_password.empty() ? "none" : "set");
		ConsoleBridge::WriteOut(buf);
	}

	void Cmd_CliWindow(void)
	{
		if (gEngfuncs.Cmd_Argc() < 2)
		{
			char buf[160];
			_snprintf_s(buf, sizeof(buf), _TRUNCATE,
				"cli.window mode = %d (0=show 1=offscreen 2=hide) block_input=%s",
				WindowManager::GetMode(), EngineInput::GetBlockInput() ? "on" : "off");
			ConsoleBridge::WriteOut(buf);
			return;
		}
		WindowManager::SetMode(atoi(gEngfuncs.Cmd_Argv(1)));
		ConsoleBridge::WriteOut("cli.window applied");
	}

	void Cmd_CliInputLock(void)
	{
		if (gEngfuncs.Cmd_Argc() < 2)
		{
			unsigned getCalls = 0, setCalls = 0, relCalls = 0, warpCalls = 0, motionCalls = 0;
			int lastDx = 0, lastDy = 0;
			InputLock::GetHookStats(getCalls, setCalls, relCalls, warpCalls, motionCalls, lastDx, lastDy);
			gEngfuncs.Con_Printf("cli.inputlock: %s (hooks=%s, calls: get=%u set=%u rel=%u warp=%u motion=%u last=(%d,%d))\n",
				InputLock::GetActive() ? "on" : "off",
				InputLock::HooksInstalled() ? "installed" : "missing (client does not import cursor calls)",
				getCalls, setCalls, relCalls, warpCalls, motionCalls, lastDx, lastDy);
			return;
		}
		const char* arg = gEngfuncs.Cmd_Argv(1);
		if (!_stricmp(arg, "on") || !_stricmp(arg, "off"))
		{
			InputLock::SetActive(!_stricmp(arg, "on"));
			gEngfuncs.Con_Printf("cli.inputlock: %s\n", InputLock::GetActive() ? "on" : "off");
		}
		else
		{
			gEngfuncs.Con_Printf("usage: cli.inputlock [on|off] - lock the mouse cursor so it stops driving the view\n");
		}
	}

	void Cmd_CliBlockInput(void)
	{
		if (gEngfuncs.Cmd_Argc() < 2)
		{
			unsigned keyEvents = 0, keyBlocked = 0, mouseEvents = 0, mouseBlocked = 0;
			EngineInput::GetStats(keyEvents, keyBlocked, mouseEvents, mouseBlocked);
			std::string hooks = EngineInput::EngineHooksInstalled() ? "installed"
				: std::string("missing (") + EngineInput::EngineHooksError() + ")";
			gEngfuncs.Con_Printf("cli.blockinput: %s (engine hooks=%s, window fallback=%s, "
				"physical events: key=%u blocked=%u, mouse=%u blocked=%u)\n",
				EngineInput::GetBlockInput() ? "on" : "off", hooks.c_str(),
				EngineInput::WindowFallbackActive() ? "on" : "off",
				keyEvents, keyBlocked, mouseEvents, mouseBlocked);
			return;
		}
		const char* arg = gEngfuncs.Cmd_Argv(1);
		if (!_stricmp(arg, "on") || !_stricmp(arg, "off"))
		{
			EngineInput::SetBlockInput(!_stricmp(arg, "on"));
			gEngfuncs.Con_Printf("cli.blockinput: %s (window fallback=%s)\n",
				EngineInput::GetBlockInput() ? "on" : "off", EngineInput::WindowFallbackActive() ? "on" : "off");
		}
		else
		{
			gEngfuncs.Con_Printf("usage: cli.blockinput [on|off] - make the game ignore the physical keyboard and mouse buttons\n");
		}
	}

	// The <0|1> "down" argument shared by cli.trapkey / cli.trapmouse.
	bool ParseDown(const char* arg, bool& down)
	{
		if (!strcmp(arg, "1") || !strcmp(arg, "0"))
		{
			down = arg[0] == '1';
			return true;
		}
		return false;
	}

	void Cmd_CliTrapKey(void)
	{
		bool down = false;
		if (gEngfuncs.Cmd_Argc() != 3 || !ParseDown(gEngfuncs.Cmd_Argv(2), down))
		{
			gEngfuncs.Con_Printf("usage: cli.trapkey <key> <0|1> - send a key event through native engine input "
				"(key: bind name such as w, SPACE, ENTER, MOUSE1, MWHEELUP, or a keynum)\n");
			return;
		}
		const char* name = gEngfuncs.Cmd_Argv(1);
		int key = EngineInput::KeyFromName(name);
		if (key < 0)
		{
			gEngfuncs.Con_Printf("cli.trapkey: error: unknown key \"%s\"\n", name);
			return;
		}
		if (!EngineInput::SendKey(key, down))
		{
			gEngfuncs.Con_Printf("cli.trapkey: error: input injection failed (%s)\n", EngineInput::EngineHooksError());
			return;
		}
		gEngfuncs.Con_Printf("cli.trapkey: %s (%d) %s\n", name, key, down ? "down" : "up");
	}

	// Mask of the five mouse buttons native engine input carries.
	const int MOUSE_BUTTONS_MASK = 0x1F;

	void Cmd_CliTrapMouse(void)
	{
		bool down = false;
		char* end = nullptr;
		long buttons = gEngfuncs.Cmd_Argc() == 3 ? strtol(gEngfuncs.Cmd_Argv(1), &end, 10) : -1;
		if (gEngfuncs.Cmd_Argc() != 3 || *end != '\0' || buttons < 0 || buttons > MOUSE_BUTTONS_MASK ||
			!ParseDown(gEngfuncs.Cmd_Argv(2), down))
		{
			gEngfuncs.Con_Printf("usage: cli.trapmouse <buttons> <0|1> - send a mouse button event through "
				"native engine input (buttons: held mask after the event, 1=left 2=right 4=middle 8=mouse4 16=mouse5)\n");
			return;
		}
		if (!EngineInput::SendMouse((int)buttons, down))
		{
			gEngfuncs.Con_Printf("cli.trapmouse: error: input injection failed (%s)\n", EngineInput::EngineHooksError());
			return;
		}
		gEngfuncs.Con_Printf("cli.trapmouse: buttons=%ld %s\n", buttons, down ? "down" : "up");
	}

	// Plain Levenshtein distance; cvar/command names are short so O(len*len) is fine.
	size_t EditDistance(const std::string& a, const std::string& b)
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

	const size_t MAX_SUGGESTIONS = 10;

	// cli.find: reports whether a name exists as a cvar or console command, and
	// when it does not, suggests similar names (substring match or small edit
	// distance). Output goes through Con_Printf so it lands in the captured
	// response for piped-stdin and RCON clients, not just the stdout mirror.
	void Cmd_CliFind(void)
	{
		if (gEngfuncs.Cmd_Argc() < 2)
		{
			gEngfuncs.Con_Printf("usage: cli.find <name> - check if a cvar/command exists, or suggest similar names\n");
			return;
		}

		std::string query = text::Lowercase(gEngfuncs.Cmd_Argv(1));

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
			std::string name = text::Lowercase(rawname);
			if (name == query)
				return true;

			size_t distance = EditDistance(query, name);
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
		size_t shown = std::min(suggestions.size(), MAX_SUGGESTIONS);
		for (size_t i = 0; i < shown; ++i)
		{
			line += ' ';
			line += suggestions[i].name;
			if (i + 1 < shown)
				line += ',';
		}
		gEngfuncs.Con_Printf("%s\n", line.c_str());
	}

	void Cmd_CliUserMsg(void)
	{
		UserMsgMonitor::CmdUserMsg();
	}
}

void CliCommands::RegisterAll()
{
	gEngfuncs.pfnAddCommand("cli.help", Cmd_CliHelp);
	gEngfuncs.pfnAddCommand("cli.rconinfo", Cmd_CliRconInfo);
	gEngfuncs.pfnAddCommand("cli.window", Cmd_CliWindow);
	gEngfuncs.pfnAddCommand("cli.inputlock", Cmd_CliInputLock);
	gEngfuncs.pfnAddCommand("cli.blockinput", Cmd_CliBlockInput);
	gEngfuncs.pfnAddCommand("cli.trapkey", Cmd_CliTrapKey);
	gEngfuncs.pfnAddCommand("cli.trapmouse", Cmd_CliTrapMouse);
	gEngfuncs.pfnAddCommand("cli.find", Cmd_CliFind);
	gEngfuncs.pfnAddCommand("cli.usermsg", Cmd_CliUserMsg);
}
