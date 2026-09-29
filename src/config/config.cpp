#include "config/config.h"

#include "core/plugins.h"
#include "util/toml_file.h"

#include <metahook.h>

namespace
{
	const char kConfigPath[] = "metahook/configs/halflifecli.toml";

	void SetInt(const toml::table& t, std::string_view key, int& out)
	{
		if (auto v = t[key].value<int64_t>())
			out = static_cast<int>(*v);
	}

	void SetString(const toml::table& t, std::string_view key, std::string& out)
	{
		if (auto v = t[key].value<std::string_view>())
			out.assign(*v);
	}
}

static CliConfig g_config;

CliConfig& CLI_Config()
{
	return g_config;
}

// Reads metahook/configs/halflifecli.toml through the engine filesystem.
// Missing file is not an error: compiled-in defaults target automation usage.
bool CliConfig::Load()
{
	toml::table tbl;
	std::string err;
	if (!toml_file::Read(kConfigPath, tbl, err))
	{
		if (err != "file not found")
			gEngfuncs.Con_Printf("halflife-cli: %s: %s, using defaults\n", kConfigPath, err.c_str());
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
		SetString(*usermsg, "display_channels", usermsg_display_channels);
	}
	return true;
}
