#include "config.h"
#include "plugins.h"
#include <metahook.h>
#include <string>
#include <cstring>

static CliConfig g_config;

CliConfig& CLI_Config()
{
	return g_config;
}

static std::string Trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// Reads <mod>/metahook/configs/halflifecli.ini through the engine filesystem.
// Missing file is not an error: compiled-in defaults target automation usage.
bool CliConfig::Load()
{
	FileHandle_t fp = FILESYSTEM_ANY_OPEN("metahook/configs/halflifecli.ini", "rb");
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

	std::string section;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos)
			eol = text.size();
		std::string line = Trim(text.substr(pos, eol - pos));
		pos = eol + 1;

		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line[0] == '[' && line.back() == ']')
		{
			section = Trim(line.substr(1, line.size() - 2));
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = Trim(line.substr(0, eq));
		std::string value = Trim(line.substr(eq + 1));

		if (section == "rcon")
		{
			if (key == "port") rcon_port = atoi(value.c_str());
			else if (key == "bind") rcon_bind = value;
			else if (key == "password") rcon_password = value;
			else if (key == "allowed_ips") rcon_allowed_ips = value;
		}
		else if (section == "cli")
		{
			if (key == "hide_window") hide_window = atoi(value.c_str());
			else if (key == "developer") developer = atoi(value.c_str());
			else if (key == "capture") capture = atoi(value.c_str());
			else if (key == "console") console = atoi(value.c_str());
			else if (key == "console_topmost") console_topmost = atoi(value.c_str());
			else if (key == "rcon") rcon = atoi(value.c_str());
		}
	}
	return true;
}
