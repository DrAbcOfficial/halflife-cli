#include "config.h"
#include "plugins.h"
#include <metahook.h>
#include <toml++/toml.hpp>
#include <string>

static CliConfig g_config;

CliConfig& CLI_Config()
{
	return g_config;
}

static void SetInt(const toml::table& t, std::string_view key, int& out)
{
	if (auto v = t[key].value<int64_t>())
		out = static_cast<int>(*v);
}

static void SetString(const toml::table& t, std::string_view key, std::string& out)
{
	if (auto v = t[key].value<std::string_view>())
		out.assign(*v);
}

// Reads <mod>/metahook/configs/halflifecli.toml through the engine filesystem.
// Missing file is not an error: compiled-in defaults target automation usage.
bool CliConfig::Load()
{
	FileHandle_t fp = FILESYSTEM_ANY_OPEN("metahook/configs/halflifecli.toml", "rb");
	if (!fp)
		return false;

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

	toml::table tbl;
	try
	{
		tbl = toml::parse(text);
	}
	catch (const toml::parse_error& err)
	{
		const std::string desc(err.description());
		gEngfuncs.Con_Printf("halflife-cli: halflifecli.toml parse error (line %u): %s, using defaults\n",
			(unsigned)err.source().begin.line, desc.c_str());
		return false;
	}

	if (const toml::table* rcon = tbl["rcon"].as_table())
	{
		SetInt(*rcon, "port", rcon_port);
		SetString(*rcon, "bind", rcon_bind);
		SetString(*rcon, "password", rcon_password);
		SetString(*rcon, "allowed_ips", rcon_allowed_ips);
	}
	if (const toml::table* cli = tbl["cli"].as_table())
	{
		SetInt(*cli, "hide_window", hide_window);
		SetInt(*cli, "developer", developer);
		SetInt(*cli, "capture", capture);
		SetInt(*cli, "console", console);
		SetInt(*cli, "console_topmost", console_topmost);
		SetInt(*cli, "rcon", rcon);
	}
	if (const toml::table* usermsg = tbl["usermsg"].as_table())
	{
		SetInt(*usermsg, "enabled", usermsg_enabled);
		SetString(*usermsg, "file", usermsg_file);
		SetInt(*usermsg, "max_string", usermsg_max_string);
	}
	return true;
}
