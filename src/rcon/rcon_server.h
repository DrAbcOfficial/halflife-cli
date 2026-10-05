#pragma once

#include <string>

// Sven uses the engine's native UDP socket. Other engines retain Source TCP.
// A failed Sven capability check never falls back to TCP.
namespace RconServer
{
	struct StartResult
	{
		bool ok = false;
		unsigned short port = 0;      // actual bound port (0 on failure)
		std::string error;
	};

		void Install(); // LoadEngine: resolve every capability before installing hooks
		void OnClientReady();
		void AfterCommands(); // called only from the verified main-frame Cbuf call
		void OnEngineShutdown(); // before Host_Shutdown / NET_Shutdown
		bool UsesMainFrame();

	void Shutdown();
	bool Running();
		unsigned short CurrentPort(); // valid after a successful Start()
		std::string CurrentAddress();
		const char* Protocol();
		bool PasswordSet();
		const std::string& Error();
}
