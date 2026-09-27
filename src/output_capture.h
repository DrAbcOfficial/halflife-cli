#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Captures engine console output by inline-hooking Con_Printf / Con_DPrintf /
// Con_Warning. Con_DPrintf is captured even when the "developer" cvar is 0
// (the suppression check lives inside the original function, behind our hook),
// which is exactly what automated testing needs.
namespace OutputCapture
{
	// Resolves and hooks the engine print functions. Returns true when at least
	// Con_Printf was hooked; false means output capture is unavailable.
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

	// Are we hooked at all?
	bool Available();
}
