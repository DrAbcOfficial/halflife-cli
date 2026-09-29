#include "usermsg_schema.h"
#include "plugins.h"

#include <metahook.h>
#include <toml++/toml.hpp>

#include <set>

namespace
{
	constexpr const char* kSchemaDir = "metahook/configs/usermsgs/";

	bool ReadTomlFile(const std::string& file, toml::table& out, std::string& err)
	{
		const std::string path = std::string(kSchemaDir) + file;
		FileHandle_t fp = FILESYSTEM_ANY_OPEN(path.c_str(), "rb");
		if (!fp)
		{
			err = "file not found";
			return false;
		}

		int size = FILESYSTEM_ANY_SIZE(fp);
		std::string text;
		if (size > 0 && size < (1 << 20))
		{
			text.resize(size);
			FILESYSTEM_ANY_READ(&text[0], size, fp);
		}
		FILESYSTEM_ANY_CLOSE(fp);

		// Windows editors commonly leave a UTF-8 BOM; the TOML spec forbids it.
		if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
			text.erase(0, 3);

		try
		{
			out = toml::parse(text);
		}
		catch (const toml::parse_error& e)
		{
			err = "parse error on line " + std::to_string((unsigned)e.source().begin.line) + ": " + std::string(e.description());
			return false;
		}
		return true;
	}

	bool ParseCount(const toml::table& t, UserMsgField& f, std::string& err)
	{
		const toml::node* count = t["count"].node();
		if (count)
		{
			if (auto v = count->value<int64_t>())
			{
				if (*v < 1 || *v > 4096)
				{
					err = "count must be 1..4096";
					return false;
				}
				f.count = (int)*v;
				return true;
			}
			if (auto v = count->value<std::string_view>())
			{
				if (*v == "*")
				{
					f.countRest = true;
					return true;
				}
				f.countFromField = true;
				f.countField.assign(*v);
				return true;
			}
			err = "count must be an integer, a field name, or \"*\"";
			return false;
		}
		return true;
	}

	bool ParseWhen(const toml::table& t, UserMsgField& f, std::string& err)
	{
		const toml::table* when = t["when"].as_table();
		if (!when)
		{
			if (t["when"].node())
			{
				err = "when must be an inline table { field = ..., eq|ne = ... }";
				return false;
			}
			return true;
		}

		auto field = (*when)["field"].value<std::string_view>();
		if (!field)
		{
			err = "when is missing \"field\"";
			return false;
		}
		f.hasWhen = true;
		f.whenField.assign(*field);

		auto eq = (*when)["eq"].value<double>();
		auto ne = (*when)["ne"].value<double>();
		if (eq.has_value() == ne.has_value())
		{
			err = "when needs exactly one of \"eq\" or \"ne\"";
			return false;
		}
		f.whenNotEqual = ne.has_value();
		f.whenValue = f.whenNotEqual ? *ne : *eq;
		return true;
	}

	bool ParseField(const toml::table& t, UserMsgField& f, std::string& err)
	{
		if (auto v = t["name"].value<std::string_view>())
			f.name.assign(*v);
		if (auto v = t["type"].value<std::string_view>())
			f.type.assign(*v);
		if (auto v = t["note"].value<std::string_view>())
			f.note.assign(*v);

		if (f.type.empty())
		{
			err = "field is missing \"type\"";
			return false;
		}

		static const char* kTypes[] = { "byte", "char", "short", "word", "long", "float",
			"coord", "angle", "angle16", "string", "vec3", "group" };
		bool known = false;
		for (const char* k : kTypes)
		{
			if (f.type == k)
			{
				known = true;
				break;
			}
		}
		if (!known)
		{
			err = "unknown field type \"" + f.type + "\"";
			return false;
		}

		if (!ParseCount(t, f, err) || !ParseWhen(t, f, err))
			return false;

		if (f.type == "group")
		{
			const toml::array* fields = t["fields"].as_array();
			if (!fields)
			{
				err = "group field is missing \"fields\"";
				return false;
			}
			for (const toml::node& el : *fields)
			{
				const toml::table* ft = el.as_table();
				if (!ft)
				{
					err = "group \"fields\" entries must be inline tables";
					return false;
				}
				UserMsgField child;
				if (!ParseField(*ft, child, err))
					return false;
				f.children.push_back(std::move(child));
			}
		}
		else if (t["fields"].node())
		{
			err = "only \"group\" fields may have \"fields\"";
			return false;
		}
		return true;
	}

	bool ParseMessage(const toml::table& t, UserMsgDef& def, std::string& err)
	{
		auto name = t["name"].value<std::string_view>();
		if (!name || name->empty())
		{
			err = "[[usermsg]] is missing \"name\"";
			return false;
		}
		def.name.assign(*name);
		if (auto v = t["note"].value<std::string_view>())
			def.note.assign(*v);
		if (auto v = t["raw"].value<bool>())
			def.raw = *v;

		if (const toml::array* fields = t["fields"].as_array())
		{
			for (const toml::node& el : *fields)
			{
				const toml::table* ft = el.as_table();
				if (!ft)
				{
					err = "message \"" + def.name + "\": \"fields\" entries must be inline tables";
					return false;
				}
				UserMsgField field;
				if (!ParseField(*ft, field, err))
				{
					err = "message \"" + def.name + "\": " + err;
					return false;
				}
				def.fields.push_back(std::move(field));
			}
		}
		return true;
	}

	std::string Lowercase(const std::string& s)
	{
		std::string out = s;
		for (size_t i = 0; i < out.size(); ++i)
			out[i] = (char)tolower((unsigned char)out[i]);
		return out;
	}

	// Loads one file, following "extends" first (base entries land in the
	// schema before the child's, so a child replaces by name). Returns false
	// only when the ROOT file fails; a broken base is reported and also fails
	// the load, because a partially inherited schema would misparse the wire.
	bool LoadInternal(const std::string& file, std::set<std::string>& visited, bool root, UserMsgSchema& schema)
	{
		if (!visited.insert(Lowercase(file)).second)
		{
			gEngfuncs.Con_Printf("halflife-cli: usermsg schema cycle detected at \"%s\"\n", file.c_str());
			return false;
		}

		toml::table tbl;
		std::string err;
		if (!ReadTomlFile(file, tbl, err))
		{
			gEngfuncs.Con_Printf("halflife-cli: usermsg schema \"%s%s%s\" %s\n",
				kSchemaDir, file.c_str(), root ? "" : " (extends)", err.c_str());
			return false;
		}

		if (auto extends = tbl["extends"].value<std::string_view>())
		{
			if (!extends->empty() && !LoadInternal(std::string(*extends), visited, false, schema))
				return false;
		}

		if (const toml::table* primitives = tbl["primitives"].as_table())
		{
			if (auto v = (*primitives)["coord_size"].value<int64_t>())
			{
				if (*v == 2 || *v == 4)
					schema.coord_size = (int)*v;
				else
					gEngfuncs.Con_Printf("halflife-cli: usermsg schema \"%s\": primitives.coord_size must be 2 or 4\n", file.c_str());
			}
		}

		if (const toml::array* msgs = tbl["usermsg"].as_array())
		{
			for (const toml::node& el : *msgs)
			{
				const toml::table* t = el.as_table();
				if (!t)
					continue;
				UserMsgDef def;
				if (!ParseMessage(*t, def, err))
				{
					gEngfuncs.Con_Printf("halflife-cli: usermsg schema \"%s\": %s\n", file.c_str(), err.c_str());
					return false;
				}
				auto it = schema.index.find(Lowercase(def.name));
				if (it != schema.index.end())
					schema.messages[it->second] = std::move(def);  // child overrides the base message
				else
				{
					schema.index.emplace(Lowercase(def.name), schema.messages.size());
					schema.messages.push_back(std::move(def));
				}
			}
		}
		return true;
	}
}

bool UserMsgSchema::Load(const std::string& file)
{
	coord_size = 2;
	messages.clear();
	index.clear();

	std::set<std::string> visited;
	return LoadInternal(file, visited, true, *this);
}
