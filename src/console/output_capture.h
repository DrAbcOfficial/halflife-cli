#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Captures engine console output through the VGUI2Extension plugin's
// GameConsole callbacks (the IGameConsole "GameConsole003" Printf/DPrintf
// path the on-screen vgui console itself uses). VGUI2Extension.dll is a hard
// dependency — there is no fallback engine hooking. Con_DPrintf output only
// reaches the vgui console when the engine routes it there, so keep
// `developer` >= 1 (the plugin sets developer 1 by default) to see it.
namespace OutputCapture
{
	// Registers the GameConsole callbacks with VGUI2Extension. Returns true
	// when registered; false means VGUI2Extension.dll (or its interface) is
	// missing and output capture is unavailable.
	bool Install();

	void Shutdown();

	// Monotonic watermark of captured lines. NextSeq() - 1 is the newest line.
	uint64_t NextSeq();

	// Lines captured in the half-open range [begin, end). Empty when end <= begin.
	std::vector<std::string> GetLines(uint64_t begin, uint64_t end);

	// Live consumer called for every completed line (console mirror).
	// The callback runs under the capture lock; keep it fast.
	using LineSink = void (*)(const std::string& line);
	void SetSink(LineSink sink);

	// Are we registered with VGUI2Extension?
	bool Available();
}
