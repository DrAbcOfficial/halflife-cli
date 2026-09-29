#pragma once

#include <string>

// Source RCON protocol server (winsock2), informed by the protocol layout used
// across Source-engine games: little-endian [size:int32][id:int32][type:int32]
// [body bytes][NUL][empty-string NUL], where size = 10 + body length.
// Packets: SERVERDATA_AUTH(3) / AUTH_RESPONSE(2) / EXECCOMMAND(2, direction
// disambiguates from AUTH_RESPONSE) / RESPONSE_VALUE(0).
//
// Commands are forwarded to ConsoleBridge and answered with the console output
// captured around execution. Binds to the configured address with an ephemeral
// port by default (0 => random reachable port), so the default posture is
// localhost-only automation access.
namespace RconServer
{
	struct StartResult
	{
		bool ok = false;
		unsigned short port = 0;      // actual bound port (0 on failure)
		std::string error;
	};

	StartResult Start(const std::string& bindAddr, unsigned short port,
		const std::string& password, const std::string& allowedIps);

	void Shutdown();
	bool Running();
	unsigned short CurrentPort(); // valid after a successful Start()
}
