#include "usermsg/usermsg_monitor.h"

#include "config/config.h"
#include "core/plugins.h"
#include "usermsg/event_log.h"
#include "usermsg/hook_policy.h"
#include "usermsg/usermsg_decoder.h"
#include "usermsg/usermsg_schema.h"
#include "util/text.h"

#include <metahook.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// Hooks the user-message entries the client game DLL registers with the
// engine, decodes payloads via UserMsgDecoder, records every message into its
// schema-assigned EventLog channel (functional group), and answers the
// "cli.usermsg" console command.

namespace
{
	struct HookEntry
	{
		const UserMsgDef* def = nullptr;
		pfnUserMsgHook original = nullptr;  // client DLL's hook; NULL = entry created by us (display-only)
		bool known = false;                 // an engine entry carries our dispatcher
	};

	// cli.usermsg events/list budget: the RCON response body caps at 4096
	// bytes and other console output rides along, so stop well before that.
	constexpr size_t kReplyByteBudget = 3300;

	// Wildcard channel: "display_channels = all" echoes every channel, and
	// "cli.usermsg events channel all" (the default) reads the merged ring.
	constexpr const char* kAllChannels = "all";

	EventLog g_eventLog(512);       // per-channel ring capacity
	std::set<std::string> g_displayChannels;
	UserMsgSchema g_schema;
	std::string g_schemaFile;
	std::vector<HookEntry> g_entries;
	bool g_display = true;          // cli.usermsg on|off (master gate; recording is unaffected)
	bool g_pendingReload = false;
	bool g_reported = false;        // hook-state summary printed once per (re)load
	bool g_schemaMissing = false;   // root schema file absent; monitoring disabled until reload
	size_t g_maxString = 64;

	// g_entries parallels g_schema.messages; the map lookup answers both.
	HookEntry* FindEntry(const char* name)
	{
		auto it = g_schema.index.find(text::Lowercase(name));
		if (it == g_schema.index.end())
			return nullptr;
		return g_entries.data() + it->second;
	}

	// ---------------------------------------------------------------- hooks

	int UserMsg_Dispatch(const char* pszName, int iSize, void* pbuf);
	void HandleMessage(const UserMsgDef& def, int iSize, void* pbuf);

	// Single dispatcher for every monitored message: the engine passes the
	// registered name as the first argument, so one function serves all.
	int UserMsg_Dispatch(const char* pszName, int iSize, void* pbuf)
	{
		HookEntry* e = FindEntry(pszName);
		if (!e)
			return 0;  // not ours; should not happen

		// Recording is unconditional (the EventLog is the traffic recorder);
		// the console echo is gated on the master toggle and the channel list.
		if (iSize >= 0 && pbuf)
			HandleMessage(*e->def, iSize, pbuf);

		if (e->original)
			return e->original(pszName, iSize, pbuf);
		return 1;
	}

		// Repair client registrations while preserving later plugin wrappers.
		// HookUserMsg replaces only the head; it cannot inspect the chain.
		void TryHookAll(bool& changed)
		{
			for (HookEntry& e : g_entries)
			{
				auto entry = g_pMetaHookAPI->FindUserMsgHook(e.def->name.c_str());
				pfnUserMsgHook head = entry ? entry->function : nullptr;
				const uintptr_t address = reinterpret_cast<uintptr_t>(head);
				const uintptr_t client = reinterpret_cast<uintptr_t>(g_pMetaHookAPI->GetClientBase());
				const bool clientCallback = client && address >= client &&
					address - client < g_pMetaHookAPI->GetClientSize();
				if (!UserMsgHooks::ShouldInstall(e.known, head, &UserMsg_Dispatch, e.original, clientCallback))
					continue;
				pfnUserMsgHook current = g_pMetaHookAPI->HookUserMsg(e.def->name.c_str(), &UserMsg_Dispatch);
			if (current == &UserMsg_Dispatch)
			{
				// Our dispatcher is already installed. Whether the entry
				// originated from us or from the client DLL is now fused;
				// keep the last known original.
				if (!e.known)
				{
					e.known = true;
					changed = true;
				}
			}
			else if (current)
			{
				if (!e.known || e.original != current)
				{
					e.original = current;
					e.known = true;
					changed = true;
				}
			}
			else
			{
				// No engine entry (yet): register a display-only hook. If the
				// client DLL registers the same name later, the next round
				// upgrades us to wrap it.
				gEngfuncs.pfnHookUserMsg((char*)e.def->name.c_str(), &UserMsg_Dispatch);
				if (!e.known || e.original != nullptr)
				{
					e.original = nullptr;
					e.known = true;
					changed = true;
				}
			}
		}
	}

	// ------------------------------------------------------- record + echo

	void HandleMessage(const UserMsgDef& def, int iSize, void* pbuf)
	{
		std::string line = UserMsgDecoder::Format(g_schema, def, iSize, pbuf, g_maxString);
		g_eventLog.Record(def.channel.c_str(), line);
		if (g_display && (g_displayChannels.count(kAllChannels) || g_displayChannels.count(def.channel)))
			gEngfuncs.Con_Printf("%s\n", line.c_str());
	}

	// --------------------------------------------------------------- schema

	std::string DefaultSchemaFile()
	{
		const char* dir = g_pMetaHookAPI->GetGameDirectory();
		if (!dir || !*dir)
			return "valve.toml";
		std::string name(dir);
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos)
			name.erase(0, slash + 1);
		name += ".toml";
		return name;
	}

	void ReportState()
	{
		size_t wrapped = 0, own = 0, pending = 0;
		for (const HookEntry& e : g_entries)
		{
			if (!e.known)
				++pending;
			else if (e.original)
				++wrapped;
			else
				++own;
		}
		char b[256];
		_snprintf_s(b, sizeof(b), _TRUNCATE,
			"cli.usermsg: schema=%s coord_size=%d messages=%u display=%s hooks: %u wrapped, %u self-registered, %u pending",
			g_schemaFile.c_str(), g_schema.coord_size, (unsigned)g_schema.messages.size(),
			g_display ? "on" : "off", (unsigned)wrapped, (unsigned)own, (unsigned)pending);
		gEngfuncs.Con_Printf("%s\n", b);
	}

	void LoadSchema()
	{
		g_maxString = CLI_Config().usermsg_max_string > 0 ? (size_t)CLI_Config().usermsg_max_string : 64;

		g_displayChannels.clear();
		for (const std::string& ch : text::SplitCsv(CLI_Config().usermsg_display_channels))
			g_displayChannels.insert(text::Lowercase(ch));

		g_schemaFile = CLI_Config().usermsg_file;
		if (g_schemaFile.empty())
			g_schemaFile = DefaultSchemaFile();

		// The engine's usermsg entries still carry our dispatcher from the
		// previous schema, and the wrapped originals live only in g_entries;
		// carry them across the rebuild (and hand back the hooks of messages
		// dropped from the schema), or the game DLL would stop receiving them.
			std::map<std::string, HookEntry> oldOriginals;
		std::vector<std::string> oldNames;
		for (const HookEntry& e : g_entries)
		{
			oldNames.push_back(e.def->name);
				oldOriginals[text::Lowercase(e.def->name.c_str())] = e;
		}

		UserMsgSchema::LoadResult result = g_schema.Load(g_schemaFile);

		g_entries.clear();
		if (result == UserMsgSchema::LoadResult::Missing)
		{
			// No schema for this mod: report once and switch the monitor off
			// (nothing is hooked or recorded). A later reload that finds the
			// file re-enables it.
			g_schemaMissing = true;
		}
		else
		{
			g_schemaMissing = false;
			for (const UserMsgDef& def : g_schema.messages)
			{
				HookEntry e;
				e.def = &def;
				auto it = oldOriginals.find(text::Lowercase(def.name.c_str()));
				if (it != oldOriginals.end())
				{
						e.original = it->second.original;
						e.known = it->second.known;
				}
				g_entries.push_back(e);
			}
		}
		// Hand back the hooks of messages no longer monitored — on both
		// paths (a reload that shrank the schema, and the missing-schema
		// shutdown), or the game DLL would stop receiving them.
		for (const std::string& name : oldNames)
		{
			if (g_schema.index.count(text::Lowercase(name.c_str())))
				continue;
			auto it = oldOriginals.find(text::Lowercase(name.c_str()));
				if (it != oldOriginals.end() && it->second.original)
					g_pMetaHookAPI->HookUserMsg(name.c_str(), it->second.original);
		}
		g_reported = false;

		if (result == UserMsgSchema::LoadResult::Missing)
		{
			gEngfuncs.Con_Printf("halflife-cli: usermsg monitoring disabled "
				"(no schema for this mod; install one into metahook/configs/halflifecli/"
				"usermsgs/ and run \"cli.usermsg reload\")\n");
			return;
		}

		char b[160];
		_snprintf_s(b, sizeof(b), _TRUNCATE, "halflife-cli: usermsg schema \"%s\" loaded (%u messages)",
			g_schemaFile.c_str(), (unsigned)g_schema.messages.size());
		gEngfuncs.Con_Printf("%s\n", b);
	}
}

namespace UserMsgMonitor
{
	void Init()
	{
		if (!CLI_Config().usermsg_enabled)
			return;
		g_display = true;
		LoadSchema();
	}

	void OnHudInit()
	{
		if (g_entries.empty())
			return;
		bool changed = false;
		TryHookAll(changed);
		if (changed && !g_reported)
		{
			g_reported = true;
			ReportState();
		}
	}

	void OnHudVidInit()
	{
		OnHudInit();
	}

	void Frame()
	{
		if (g_entries.empty())
			return;

		if (g_pendingReload)
		{
			g_pendingReload = false;
			LoadSchema();
			bool changed = false;
			TryHookAll(changed);
			ReportState();
			return;
		}

		// Engine entries can be recreated on map changes; re-assert the wrap
		// periodically (cheap: a few dozen list walks every few seconds).
		static unsigned frameCount = 0;
		if (++frameCount % (66 * 5) != 0)  // roughly every 5 s
			return;

		bool changed = false;
		TryHookAll(changed);
		if (changed)
		{
			g_reported = true;
			ReportState();
		}
	}

	void Shutdown()
	{
		g_entries.clear();
		g_eventLog.Clear();
		g_schema.messages.clear();
		g_schema.index.clear();
		g_reported = false;
		g_schemaMissing = false;
	}

	// Case-insensitive match of the message-name portion of an event line
	// ("[usermsg] Name size=...").
	bool EventNameMatches(const std::string& line, const char* filter)
	{
		if (!*filter)
			return true;
		static const char kPrefix[] = "[usermsg] ";
		constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
		if (line.compare(0, kPrefixLen, kPrefix) != 0)
			return false;
		size_t end = line.find(' ', kPrefixLen);
		if (end == std::string::npos)
			end = line.size();
		size_t len = end - kPrefixLen;
		return strlen(filter) == len && !_strnicmp(line.c_str() + kPrefixLen, filter, len);
	}

	void CmdUserMsgEvents()
	{
		uint64_t since = 0;
		bool haveSince = false;
		size_t limit = 20;
		const char* nameFilter = "";
		std::string channel = kAllChannels;

		for (int i = 2; i + 1 < gEngfuncs.Cmd_Argc(); i += 2)
		{
			const char* key = gEngfuncs.Cmd_Argv(i);
			if (!_stricmp(key, "since"))
			{
				since = _strtoui64(gEngfuncs.Cmd_Argv(i + 1), nullptr, 10);
				haveSince = true;
			}
			else if (!_stricmp(key, "limit"))
				limit = (size_t)atol(gEngfuncs.Cmd_Argv(i + 1));
			else if (!_stricmp(key, "name"))
				nameFilter = gEngfuncs.Cmd_Argv(i + 1);
			else if (!_stricmp(key, "channel"))
				channel = text::Lowercase(gEngfuncs.Cmd_Argv(i + 1));
		}
		if (limit < 1)
			limit = 1;
		if (limit > 200)
			limit = 200;
		// No explicit cursor: show the tail of the ring.
		if (!haveSince && g_eventLog.NewestSeq() > limit)
			since = g_eventLog.NewestSeq() - limit - 1;

		char b[96];
		_snprintf_s(b, sizeof(b), _TRUNCATE, "cli.usermsg: events channel=%s newest=%llu name=%s",
			channel.c_str(), (unsigned long long)g_eventLog.NewestSeq(), *nameFilter ? nameFilter : "*");
		gEngfuncs.Con_Printf("%s\n", b);

		size_t shown = 0, more = 0;
		size_t bytes = 0;
		uint64_t lastShown = 0;
		auto visit = [&](uint64_t seq, const std::string& line)
		{
			if (!EventNameMatches(line, nameFilter))
				return;
			if (shown >= limit)
			{
				++more;
				return;
			}
			char nb[32];
			_snprintf_s(nb, sizeof(nb), _TRUNCATE, "#%llu ", (unsigned long long)seq);
			std::string out = nb;
			out += line;
			if (shown > 0 && bytes + out.size() + 1 > kReplyByteBudget)
			{
				++more;
				return;
			}
			bytes += out.size() + 1;
			gEngfuncs.Con_Printf("%s\n", out.c_str());
			++shown;
			lastShown = seq;
		};
		if (!_stricmp(channel.c_str(), kAllChannels))
			g_eventLog.ForEachAll(since, visit);
		else
			g_eventLog.ForEach(channel, since, visit);
		if (!shown)
			gEngfuncs.Con_Printf("cli.usermsg: no matching events\n");
		else if (more)
		{
			_snprintf_s(b, sizeof(b), _TRUNCATE, "cli.usermsg: %u more after #%llu (raise since)",
				(unsigned)more, (unsigned long long)lastShown);
			gEngfuncs.Con_Printf("%s\n", b);
		}
	}

	void CmdUserMsgList()
	{
		size_t bytes = 0, shown = 0;
		for (const HookEntry& e : g_entries)
		{
			std::string line = !e.known ? "pending " : (e.original ? "wrapped " : "self    ");
			line += e.def->name;
			line += " [";
			line += e.def->channel;
			line += ']';
			if (e.def->raw)
				line += " (raw)";
			else
			{
				char nb[32];
				_snprintf_s(nb, sizeof(nb), _TRUNCATE, " (%u fields)", (unsigned)e.def->fields.size());
				line += nb;
			}
			if (bytes + line.size() + 1 > kReplyByteBudget)
			{
				char b[96];
				_snprintf_s(b, sizeof(b), _TRUNCATE, "cli.usermsg: list truncated at %u of %u messages",
					(unsigned)shown, (unsigned)g_entries.size());
				gEngfuncs.Con_Printf("%s\n", b);
				return;
			}
			bytes += line.size() + 1;
			gEngfuncs.Con_Printf("%s\n", line.c_str());
			++shown;
		}
	}

	void CmdUserMsg()
	{
		int argc = gEngfuncs.Cmd_Argc();
		const char* arg1 = argc >= 2 ? gEngfuncs.Cmd_Argv(1) : "";

		if (!_stricmp(arg1, "on") || !_stricmp(arg1, "off"))
		{
			g_display = !_stricmp(arg1, "on");
			gEngfuncs.Con_Printf("cli.usermsg: display %s (recording continues)\n", g_display ? "on" : "off");
			return;
		}
		if (!_stricmp(arg1, "reload"))
		{
			if (g_entries.empty())
				Init();
			else
				g_pendingReload = true;
			gEngfuncs.Con_Printf("cli.usermsg: reload queued\n");
			return;
		}
		if (g_entries.empty())
		{
			if (!CLI_Config().usermsg_enabled)
				gEngfuncs.Con_Printf("cli.usermsg: no schema loaded ([usermsg] disabled in halflifecli.toml?)\n");
			else if (g_schemaMissing)
				gEngfuncs.Con_Printf("cli.usermsg: monitoring disabled - schema \"%s\" not found "
					"(install it into metahook/configs/halflifecli/usermsgs/, then run \"cli.usermsg reload\")\n",
					g_schemaFile.c_str());
			else
				gEngfuncs.Con_Printf("cli.usermsg: no schema loaded\n");
			return;
		}
		if (!_stricmp(arg1, "events"))
		{
			CmdUserMsgEvents();
			return;
		}
		if (!_stricmp(arg1, "list"))
		{
			CmdUserMsgList();
			return;
		}
		if (!_stricmp(arg1, "pending"))
		{
			size_t n = 0;
			for (const HookEntry& e : g_entries)
			{
				if (!e.known)
				{
					gEngfuncs.Con_Printf("cli.usermsg: pending  %s\n", e.def->name.c_str());
					++n;
				}
			}
			if (!n)
				gEngfuncs.Con_Printf("cli.usermsg: no pending messages\n");
			return;
		}
		if (*arg1)
		{
			// cli.usermsg <name>: show one message's field layout.
			HookEntry* e = FindEntry(arg1);
			if (!e)
			{
				gEngfuncs.Con_Printf("cli.usermsg: \"%s\" is not in the schema\n", arg1);
				return;
			}
			const UserMsgDef& def = *e->def;
			char b[128];
			_snprintf_s(b, sizeof(b), _TRUNCATE, "cli.usermsg: %s [%s]%s hooks=%s%s%s\n", def.name.c_str(),
				def.channel.c_str(),
				def.raw ? " (raw)" : "",
				!e->known ? "pending" : (e->original ? "wrapped" : "self-registered"),
				def.note.empty() ? "" : " - ", def.note.c_str());
			gEngfuncs.Con_Printf("%s", b);
			std::string line;
			for (const UserMsgField& f : def.fields)
			{
				line = "  ";
				line += f.name.empty() ? "(anon)" : f.name;
				line += " : ";
				line += f.type;
				if (f.countRest)
					line += " *";
				else if (f.countFromField)
					line += " [" + f.countField + "]";
				else if (f.count > 1)
					line += " [" + std::to_string(f.count) + "]";
				if (f.hasWhen)
				{
					line += " when ";
					line += f.whenField;
					line += f.whenNotEqual ? " != " : " == ";
					line += std::to_string((long long)f.whenValue);
				}
				if (!f.note.empty())
					line += "  # " + f.note;
				gEngfuncs.Con_Printf("%s\n", line.c_str());
			}
			return;
		}

		ReportState();
		gEngfuncs.Con_Printf("cli.usermsg: usage: on|off|reload|list|pending|events [channel C] [since N] [limit N] [name X]|<name>\n");
	}
}
