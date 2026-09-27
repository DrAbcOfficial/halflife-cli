#pragma once

#include <string>

struct CliConfig
{
	// [rcon]
	int rcon_port = 0;                        // 0 = pick a random available port
	std::string rcon_bind = "127.0.0.1";      // localhost only by default
	std::string rcon_password;                // empty = accept any auth (bind address still applies)
	std::string rcon_allowed_ips;             // comma separated; empty = bind address is the only gate

	// [cli]
	int hide_window = 1;                      // 0 = off, 1 = move off-screen (default), 2 = ShowWindow(SW_HIDE)
	int developer = 1;                        // set the "developer" cvar so verbose engine output flows

	bool Load();
};

CliConfig& CLI_Config();
