#include "config/config.h"

#include "core/plugins.h"
#include "util/toml_file.h"

#include <metahook.h>

namespace
{
	// All plugin data lives in its own subfolder of MetaHook's configs dir.
	const char kConfigPath[] = "metahook/configs/halflifecli/halflifecli.toml";

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

	// Strict TOML boolean: only true/false. Anything else (e.g. 0/1) is
	// reported and ignored, so the compiled-in default stands.
	void SetBool(const toml::table& t, std::string_view key, bool& out)
	{
		const toml::node* node = t[key].node();
		if (!node)
			return;
		if (auto v = node->value_exact<bool>())
			out = *v;
		else
			gEngfuncs.Con_Printf("halflife-cli: %s: \"%.*s\" must be true or false, ignoring\n",
				kConfigPath, (int)key.size(), key.data());
	}
}

static CliConfig g_config;

CliConfig& CLI_Config()
{
	return g_config;
}

// Reads the plugin config through the engine filesystem.
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
		rcon_legacy_binding = rcon->contains("port") || rcon->contains("bind");
		SetInt(*rcon, "port", rcon_port);
		SetString(*rcon, "bind", rcon_bind);
		SetString(*rcon, "password", rcon_password);
		SetString(*rcon, "allowed_ips", rcon_allowed_ips);
	}
	if (const toml::table* cli = tbl["cli"].as_table())
	{
		SetInt(*cli, "hide_window", hide_window);
		SetBool(*cli, "block_input", block_input);
		SetBool(*cli, "input_lock", input_lock);
		SetBool(*cli, "focus_lock", focus_lock);
		SetInt(*cli, "developer", developer);
		SetBool(*cli, "capture", capture);
		SetBool(*cli, "console", console);
		SetBool(*cli, "console_topmost", console_topmost);
		SetBool(*cli, "rcon", rcon);
	}
	if (const toml::table* usermsg = tbl["usermsg"].as_table())
	{
		SetBool(*usermsg, "enabled", usermsg_enabled);
		SetString(*usermsg, "file", usermsg_file);
		SetInt(*usermsg, "max_string", usermsg_max_string);
		SetString(*usermsg, "display_channels", usermsg_display_channels);
	}
	return true;
}
