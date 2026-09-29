#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The CLI half of the plugin: owns the console (attached or allocated), a
// background thread reading commands from stdin, and the engine-thread command
// pump driven from HUD_Frame. All engine console output captured by
// OutputCapture is mirrored to stdout in real time, like watching the game
// console from a terminal.
namespace ConsoleBridge
{
	void Init();
	void Shutdown();

	// Runs on the engine thread (HUD_Frame): executes queued commands and
	// finalizes pending RCON responses with the output captured since submit.
	void PumpCommands();

	// Queue a console command; token 0 means "no response pairing".
	uint64_t SubmitCommand(const std::string& cmd);

	// Wait up to timeoutMs for the output of a submitted token (engine thread
	// collects it one frame after execution). Returns false on timeout.
	bool WaitForResponse(uint64_t token, std::string& out, int timeoutMs);

	// Thread-safe stdout write (also the OutputCapture sink target).
	void WriteOut(const std::string& text);
}
