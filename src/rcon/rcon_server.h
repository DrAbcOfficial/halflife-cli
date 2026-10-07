#pragma once

#include <string>

// SvEngine and the verified GoldSrc/CoF builds use the engine's native UDP
// socket; other engines (and a GoldSrc build the catalog does not cover) keep
// Source TCP. Once native UDP is selected, a failed capability check never
// falls back to TCP.
namespace RconServer
{
	struct StartResult
	{
		bool ok = false;
		unsigned short port = 0;      // actual bound port (0 on failure)
		std::string error;
	};

		void Install(); // LoadEngine: resolve every capability before installing hooks
		void RegisterCommands(); // HUD_Init
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
