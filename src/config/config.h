#pragma once

#include <string>

struct CliConfig
{
	// Boolean options are strict TOML booleans (true/false); 0/1 and other
	// types are reported and ignored, leaving the default in place.

	// [rcon]
	int rcon_port = 0;                        // 0 = pick a random available port
	std::string rcon_bind = "127.0.0.1";      // localhost only by default
	std::string rcon_password;                // empty = accept any auth (bind address still applies)
	std::string rcon_allowed_ips;             // comma separated; empty = bind address is the only gate
	bool rcon_legacy_binding = false;         // warn about bind/port on native UDP

	// [cli]
	int hide_window = 1;                      // 0 = off, 1 = move off-screen (default), 2 = ShowWindow(SW_HIDE)
	bool block_input = false;                 // true = filter native keyboard/mouse events; cli.trapkey/cli.trapmouse still inject
	bool input_lock = false;                  // true = lock the mouse input (client user32 + engine SDL) so it stops driving the view
	bool focus_lock = true;                   // keep the engine active while its window is unfocused/hidden (engine gamedata)
	int developer = 1;                        // set the "developer" cvar so verbose engine output flows
	bool capture = true;                      // capture console output via VGUI2Extension GameConsole callbacks
	bool console = true;                      // CLI console bridge: stdin reader + stdout mirror
	bool console_topmost = false;             // keep the CLI console window always on top
	bool rcon = true;                         // RCON server

	// [usermsg]
	bool usermsg_enabled = true;              // hook + decode server user messages (configs/halflifecli/usermsgs/)
	std::string usermsg_file;                 // schema file name; empty = "<gamedir>.toml"
	int usermsg_max_string = 64;              // truncate decoded strings longer than this
	// Channels (comma separated) echoed to the console while recorded;
	// "all" is the wildcard, empty = record only. Channels are the schema's
	// functional groups (weapon, status, text, score, screen, world, hud,
	// meta, or the default "usermsg").
	std::string usermsg_display_channels = "all";

	bool Load();
};

CliConfig& CLI_Config();
