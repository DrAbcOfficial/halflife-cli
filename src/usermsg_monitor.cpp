#include "usermsg_monitor.h"
#include "usermsg_schema.h"
#include "config.h"
#include "plugins.h"

#include <metahook.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct HookEntry
	{
		const UserMsgDef* def = nullptr;
		pfnUserMsgHook original = nullptr;  // client DLL's hook; NULL = entry created by us (display-only)
		bool known = false;                 // an engine entry carries our dispatcher
	};

	std::vector<HookEntry> g_entries;
	UserMsgSchema g_schema;
	std::string g_schemaFile;
	bool g_display = true;          // cli.usermsg on|off
	bool g_pendingReload = false;
	bool g_reported = false;        // hook-state summary printed once per (re)load
	size_t g_maxString = 64;

	const size_t kMaxItemsShown = 16;

	std::string LowerName(const char* s)
	{
		std::string out(s ? s : "");
		for (size_t i = 0; i < out.size(); ++i)
			out[i] = (char)tolower((unsigned char)out[i]);
		return out;
	}

	// g_entries parallels g_schema.messages; the map lookup answers both.
	HookEntry* FindEntry(const char* name)
	{
		auto it = g_schema.index.find(LowerName(name));
		if (it == g_schema.index.end())
			return nullptr;
		return g_entries.data() + it->second;
	}

	void AppendHex(std::string& out, const unsigned char* p, size_t n)
	{
		char b[8];
		for (size_t i = 0; i < n; ++i)
		{
			_snprintf_s(b, sizeof(b), _TRUNCATE, " %02X", p[i]);
			out += b;
		}
	}

	void AppendNumber(std::string& out, double v, bool integral)
	{
		char b[32];
		if (integral)
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%lld", (long long)v);
		else
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%g", v);
		out += b;
	}

	// ---------------------------------------------------------------- hooks

	int UserMsg_Dispatch(const char* pszName, int iSize, void* pbuf);
	void PrintMessage(const UserMsgDef& def, int iSize, void* pbuf);

	// Single dispatcher for every monitored message: the engine passes the
	// registered name as the first argument, so one function serves all.
	int UserMsg_Dispatch(const char* pszName, int iSize, void* pbuf)
	{
		HookEntry* e = FindEntry(pszName);
		if (!e)
			return 0;  // not ours; should not happen

		if (g_display && iSize >= 0 && pbuf)
			PrintMessage(*e->def, iSize, pbuf);

		if (e->original)
			return e->original(pszName, iSize, pbuf);
		return 1;
	}

	// For every schema message: wrap the engine entry if one exists (ours or
	// the client DLL's), otherwise create a display-only entry. Safe to run
	// any number of times; it converges no matter whether the client DLL has
	// registered the name yet, and repairs entries the engine recreated.
	void TryHookAll(bool& changed)
	{
		for (HookEntry& e : g_entries)
		{
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

	// --------------------------------------------------------------- reader

	struct Reader
	{
		const unsigned char* data = nullptr;
		size_t size = 0;
		size_t pos = 0;
		int coordSize = 2;
		bool ok = true;

		const void* Take(size_t n)
		{
			if (!ok || pos + n > size)
			{
				ok = false;
				return nullptr;
			}
			const void* p = data + pos;
			pos += n;
			return p;
		}
	};

	// Minimum bytes one item of this field consumes; bounds "*" repetition
	// and count-from-field loops.
	size_t FieldMinSize(const UserMsgField& f, int coordSize)
	{
		size_t base;
		if (f.type == "byte" || f.type == "char" || f.type == "angle")
			base = 1;
		else if (f.type == "coord")
			base = (size_t)coordSize;
		else if (f.type == "short" || f.type == "word" || f.type == "angle16")
			base = 2;
		else if (f.type == "long" || f.type == "float")
			base = 4;
		else if (f.type == "string")
			base = 1;
		else if (f.type == "vec3")
			base = 3 * (size_t)coordSize;
		else if (f.type == "group")
		{
			base = 0;
			for (const UserMsgField& c : f.children)
				if (!c.hasWhen)
					base += FieldMinSize(c, coordSize);
		}
		else
			base = 0;
		return base * (f.countRest ? 1 : (size_t)f.count);
	}

	struct Scope
	{
		std::string name;
		double value;
	};

	// Ordered (name, numeric value) pairs for when/count lookups; searched
	// newest-first, so inner scopes shadow outer ones.
	const double* LookupValue(const std::vector<Scope>& scopes, const std::string& name)
	{
		for (size_t i = scopes.size(); i-- > 0;)
		{
			if (scopes[i].name == name)
				return &scopes[i].value;
		}
		return nullptr;
	}

	// Reads one item of the field and appends its rendering; array fields
	// call this repeatedly and wrap the items in brackets.
	bool ReadOne(const UserMsgField& f, Reader& r, std::vector<Scope>& scopes, std::string& out)
	{
		char b[64];
		auto push = [&](double v)
		{
			if (!f.name.empty())
				scopes.push_back({ f.name, v });
		};

		if (f.type == "byte" || f.type == "char")
		{
			const void* p = r.Take(1);
			if (!p) return false;
			int v = f.type == "byte" ? *(const unsigned char*)p : *(const char*)p;
			push((double)v);
			AppendNumber(out, (double)v, true);
		}
		else if (f.type == "short" || f.type == "word" || f.type == "angle16")
		{
			const void* p = r.Take(2);
			if (!p) return false;
			unsigned short u = *(const unsigned short*)p;
			push(f.type == "short" ? (double)*(const short*)p : (double)u);
			if (f.type == "angle16")
			{
				_snprintf_s(b, sizeof(b), _TRUNCATE, "%.2f", (double)u * 360.0 / 65536.0);
				out += b;
			}
			else
				AppendNumber(out, (double)(short)u, true);
		}
		else if (f.type == "long" || f.type == "float")
		{
			const void* p = r.Take(4);
			if (!p) return false;
			double v = f.type == "long" ? (double)*(const long*)p : (double)*(const float*)p;
			push(v);
			AppendNumber(out, v, f.type == "long");
		}
		else if (f.type == "coord" || f.type == "angle")
		{
			const void* p = r.Take(f.type == "coord" ? (size_t)r.coordSize : 1);
			if (!p) return false;
			double v;
			if (f.type == "coord")
				v = (r.coordSize == 4 ? (double)*(const long*)p : (double)*(const short*)p) * 0.125;
			else
				v = (double)*(const char*)p * 360.0 / 256.0;
			push(v);
			_snprintf_s(b, sizeof(b), _TRUNCATE, "%.3f", v);
			out += b;
		}
		else if (f.type == "string")
		{
			const char* start = (const char*)r.data + r.pos;
			size_t avail = r.size - r.pos;
			size_t len = 0;
			while (len < avail && start[len] != '\0')
				++len;
			r.Take(len < avail ? len + 1 : len);  // consume the NUL too, when present
			if (!r.ok) return false;
			push(0.0);  // strings only test as 0 in when-counters
			out += '"';
			bool cut = len > g_maxString;
			size_t shown = cut ? g_maxString : len;
			char eb[8];
			for (size_t i = 0; i < shown; ++i)
			{
				unsigned char c = (unsigned char)start[i];
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20 || c == 0x7F)
					{
						_snprintf_s(eb, sizeof(eb), _TRUNCATE, "\\x%02X", c);
						out += eb;
					}
					else
						out += (char)c;
				}
			}
			out += cut ? "...\"" : "\"";
		}
		else if (f.type == "vec3")
		{
			const void* p = r.Take(3 * (size_t)r.coordSize);
			if (!p) return false;
			double v[3];
			if (r.coordSize == 4)
			{
				const long* l = (const long*)p;
				v[0] = l[0] * 0.125; v[1] = l[1] * 0.125; v[2] = l[2] * 0.125;
			}
			else
			{
				const short* s = (const short*)p;
				v[0] = s[0] * 0.125; v[1] = s[1] * 0.125; v[2] = s[2] * 0.125;
			}
			if (!f.name.empty())
			{
				scopes.push_back({ f.name + ".x", v[0] });
				scopes.push_back({ f.name + ".y", v[1] });
				scopes.push_back({ f.name + ".z", v[2] });
			}
			_snprintf_s(b, sizeof(b), _TRUNCATE, "(%.3f,%.3f,%.3f)", v[0], v[1], v[2]);
			out += b;
		}
		else  // group reaches WalkField, never here
			return false;
		return true;
	}

	void WalkFields(const std::vector<UserMsgField>& fields, Reader& r,
		std::vector<Scope>& scopes, std::string& out);

	void WalkField(const UserMsgField& f, Reader& r, std::vector<Scope>& scopes, std::string& out)
	{
		const size_t base = out.size();

		if (f.hasWhen)
		{
			const double* v = LookupValue(scopes, f.whenField);
			if (!v)
				return;
			bool match = f.whenNotEqual ? (*v != f.whenValue) : (*v == f.whenValue);
			if (!match)
				return;
		}

		// Resolve the repeat count: "*", a named field, or a fixed value.
		bool isArray = f.countRest || f.count > 1 || f.countFromField;
		size_t count = 1;
		size_t minSize = 0;
		if (f.countRest)
		{
			minSize = FieldMinSize(f, r.coordSize);
			count = minSize ? (size_t)-1 : 0;  // bounded by the reader below
		}
		else if (f.countFromField)
		{
			const double* v = LookupValue(scopes, f.countField);
			count = v ? (size_t)(long long)*v : 0;
		}
		else
			count = (size_t)f.count;

		if (!f.name.empty())
		{
			out += ' ';
			out += f.name;
			out += '=';
		}
		if (isArray)
			out += '[';

		size_t shown = 0;
		for (size_t i = 0; i < count; ++i)
		{
			if (minSize && r.size - r.pos < minSize)
				break;
			if (isArray && shown)
				out += ',';
			if (isArray && shown >= kMaxItemsShown)
			{
				out += "...";
				break;
			}
			if (f.type == "group")
			{
				const size_t scopeBase = scopes.size();
				WalkFields(f.children, r, scopes, out);
				scopes.resize(scopeBase);
			}
			else
			{
				std::string item;
				if (!ReadOne(f, r, scopes, item))
					break;
				out += item;
			}
			++shown;
		}

		if (isArray)
			out += ']';
		if (!r.ok)
			out.resize(base);
	}

	void WalkFields(const std::vector<UserMsgField>& fields, Reader& r,
		std::vector<Scope>& scopes, std::string& out)
	{
		for (const UserMsgField& f : fields)
		{
			if (!r.ok)
				break;
			WalkField(f, r, scopes, out);
		}
	}

	// ---------------------------------------------------------------- print

	void PrintMessage(const UserMsgDef& def, int iSize, void* pbuf)
	{
		char b[64];
		_snprintf_s(b, sizeof(b), _TRUNCATE, " size=%d", iSize);
		std::string line = "[usermsg] " + def.name + b;

		Reader r;
		r.data = (const unsigned char*)pbuf;
		r.size = (size_t)iSize;
		r.coordSize = g_schema.coord_size;

		if (def.raw)
		{
			size_t shown = r.size < 32 ? r.size : 32;
			line += " raw";
			AppendHex(line, r.data, shown);
			if (r.size > shown)
				line += " ...";
		}
		else
		{
			std::vector<Scope> scopes;
			WalkFields(def.fields, r, scopes, line);
			if (!r.ok)
				line += " truncated";
			if (r.ok && r.pos < r.size)
			{
				line += " tail";
				size_t shown = (r.size - r.pos) < 16 ? (r.size - r.pos) : 16;
				AppendHex(line, r.data + r.pos, shown);
				if (r.size - r.pos > 16)
					line += " ...";
			}
		}

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

		g_schemaFile = CLI_Config().usermsg_file;
		if (g_schemaFile.empty())
			g_schemaFile = DefaultSchemaFile();

		g_schema.Load(g_schemaFile);

		g_entries.clear();
		for (const UserMsgDef& def : g_schema.messages)
		{
			HookEntry e;
			e.def = &def;
			g_entries.push_back(e);
		}
		g_reported = false;

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
		g_schema.messages.clear();
		g_schema.index.clear();
		g_reported = false;
	}

	void CmdUserMsg()
	{
		int argc = gEngfuncs.Cmd_Argc();
		const char* arg1 = argc >= 2 ? gEngfuncs.Cmd_Argv(1) : "";

		if (!_stricmp(arg1, "on") || !_stricmp(arg1, "off"))
		{
			g_display = !_stricmp(arg1, "on");
			gEngfuncs.Con_Printf("cli.usermsg: display %s\n", g_display ? "on" : "off");
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
			gEngfuncs.Con_Printf("cli.usermsg: no schema loaded ([usermsg] disabled in halflifecli.toml?)\n");
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
		if (*arg1 && _stricmp(arg1, "list"))
		{
			// cli.usermsg <name>: show one message's field layout.
			HookEntry* e = FindEntry(arg1);
			if (!e)
			{
				gEngfuncs.Con_Printf("cli.usermsg: \"%s\" is not in the schema\n", arg1);
				return;
			}
			const UserMsgDef& def = *e->def;
			gEngfuncs.Con_Printf("cli.usermsg: %s%s%s%s\n", def.name.c_str(),
				def.raw ? " (raw)" : "", def.note.empty() ? "" : " - ", def.note.c_str());
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
					line += f.whenNotEqual ? " when " : " when ";
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
		gEngfuncs.Con_Printf("cli.usermsg: usage: on|off|reload|list|pending|<name>\n");
	}
}
