#include "output_capture.h"
#include "plugins.h"

#include <mutex>
#include <deque>
#include <cstdarg>
#include <cstdio>

typedef void (*Con_Printf_t)(const char *fmt, ...);

static Con_Printf_t g_pfnOrigConPrintf = nullptr;
static Con_Printf_t g_pfnOrigConDPrintf = nullptr;

static hook_t *g_hookConPrintf = nullptr;
static hook_t *g_hookConDPrintf = nullptr;

	struct CaptureState
	{
		std::mutex mutex;
		std::string partial;                       // chunk assembly until '\n'
		std::deque<std::string> lines;             // completed lines, ring
		uint64_t nextSeq = 0;                      // seq of the NEXT line
		uint64_t baseSeq = 0;                      // seq of lines.front()
		OutputCapture::LineSink sink = nullptr;
	};
static CaptureState g_cap;

static const size_t MAX_LINE_LENGTH = 8192;
static const size_t MAX_QUEUED_LINES = 4096;

static void EmitLine(std::string line)
{
	if (line.size() > MAX_LINE_LENGTH)
		line.resize(MAX_LINE_LENGTH);
	if (g_cap.sink)
		g_cap.sink(line);

	g_cap.lines.push_back(std::move(line));
	++g_cap.nextSeq;
	while (g_cap.lines.size() > MAX_QUEUED_LINES)
	{
		g_cap.lines.pop_front();
		++g_cap.baseSeq;
	}
}

// Common path for every hooked print function: split the text into lines,
// keep the trailing partial chunk for the next call.
static void OnText(const char *text)
{
	if (!text || !*text)
		return;

	std::lock_guard<std::mutex> lock(g_cap.mutex);
	const char *p = text;
	while (*p)
	{
		const char *nl = strchr(p, '\n');
		if (!nl)
		{
			if (g_cap.partial.size() < MAX_LINE_LENGTH)
				g_cap.partial.append(p);
			break;
		}
		g_cap.partial.append(p, nl - p);
		// strip a preceding '\r' (CRLF) so mirror output stays clean
		while (!g_cap.partial.empty() && g_cap.partial.back() == '\r')
			g_cap.partial.pop_back();
		EmitLine(g_cap.partial);
		g_cap.partial.clear();
		p = nl + 1;
	}
}

static void Hook_Con_Printf(const char *fmt, ...)
{
	char buf[16384];
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	OnText(buf);
	// The original is variadic and cannot be forwarded a va_list; the text is
	// already fully formatted here, so hand it over as "%s".
	g_pfnOrigConPrintf("%s", buf);
}

static void Hook_Con_DPrintf(const char *fmt, ...)
{
	char buf[16384];
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	OnText(buf);
	g_pfnOrigConDPrintf("%s", buf);
}

// svengine routes command-handler output (echo/version/cmdlist/unknown-command
// ...) through a second print function, not the cl_enginefunc_t Con_Printf
// entry. Located at runtime via the "Unknown command" format string xref.
static Con_Printf_t g_pfnOrigConsolePrint = nullptr;
static hook_t *g_hookConsolePrint = nullptr;

static void Hook_ConsolePrint(const char *fmt, ...)
{
	char buf[16384];
	va_list args;
	va_start(args, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	OnText(buf);
	g_pfnOrigConsolePrint("%s", buf);
}

static bool InstallConsolePrintHook()
{
	PVOID base = g_pMetaHookAPI->GetEngineBase();
	DWORD size = g_pMetaHookAPI->GetEngineSize();
	if (!base || !size)
		return false;

	static const char probeStr[] = "Unknown command: '%c'";
	const BYTE *strAddr = (const BYTE *)g_pMetaHookAPI->SearchPattern(
		base, size, probeStr, sizeof(probeStr) - 1);
	if (!strAddr)
		return false;

	// scan .text for `push imm32 <strAddr>` followed by a near call
	const BYTE imm[4] = {
		(BYTE)((uintptr_t)strAddr >> 0),
		(BYTE)((uintptr_t)strAddr >> 8),
		(BYTE)((uintptr_t)strAddr >> 16),
		(BYTE)((uintptr_t)strAddr >> 24),
	};
	const BYTE mask[4] = { 1, 1, 1, 1 };

	uintptr_t textStart = (uintptr_t)base;
	uintptr_t textEnd = textStart + size;
	uintptr_t offset = textStart;
	while (offset < textEnd - 5)
	{
		BYTE hit[4];
		const BYTE *found = (const BYTE *)g_pMetaHookAPI->SearchPatternMasked(
			(void *)offset, (DWORD)(textEnd - offset), imm, mask, 4);
		if (!found)
			break;

		if (found[-1] == 0x68) // push imm32
		{
			for (const BYTE *p = found + 4; p < found + 4 + 64 && p < (const BYTE *)textEnd - 5; ++p)
			{
				if (*p == 0xE8)
				{
					int32_t rel = *(const int32_t *)(p + 1);
					uintptr_t target = (uintptr_t)(p + 5) + rel;
					if (target > textStart && target < textEnd)
					{
						g_hookConsolePrint = g_pMetaHookAPI->InlineHook(
							(PVOID)target, Hook_ConsolePrint, (void **)&g_pfnOrigConsolePrint);
						return g_hookConsolePrint != nullptr;
					}
				}
			}
		}
		offset = (uintptr_t)found + 1;
	}
	return false;
}

namespace OutputCapture
{
	bool Install()
	{
		if (g_hookConsolePrint || g_hookConPrintf)
			return true;

		// svengine routes all console text (command output, engine prints, and
		// the cl_enginefunc_t Con_Printf entry itself, which forwards here)
		// through one print function. It is located at runtime via the
		// "Unknown command" format string xref: the svencoop gamedata catalog
		// carries no Con_* symbols, and hooking the table entry as well would
		// capture its output twice (the entry forwards into this function).
		bool ok = InstallConsolePrintHook();

		if (!ok)
		{
			// Fallback for engines where the xref scan finds nothing: hook the
			// cl_enginefunc_t Con_Printf entry directly.
			PVOID pPrintf = (PVOID)gEngfuncs.Con_Printf;
			if (pPrintf)
				g_hookConPrintf = g_pMetaHookAPI->InlineHook(pPrintf, Hook_Con_Printf, (void **)&g_pfnOrigConPrintf);
		}

		// Con_DPrintf: developer>0 style output is captured regardless of the
		// cvar because the suppression lives in the original function, behind
		// our hook.
		PVOID pDPrintf = (PVOID)gEngfuncs.Con_DPrintf;
		if (pDPrintf)
			g_hookConDPrintf = g_pMetaHookAPI->InlineHook(pDPrintf, Hook_Con_DPrintf, (void **)&g_pfnOrigConDPrintf);

		return g_hookConsolePrint != nullptr || g_hookConPrintf != nullptr;
	}

	void Shutdown()
	{
		if (g_hookConPrintf) { g_pMetaHookAPI->UnHook(g_hookConPrintf); g_hookConPrintf = nullptr; }
		if (g_hookConDPrintf) { g_pMetaHookAPI->UnHook(g_hookConDPrintf); g_hookConDPrintf = nullptr; }
		if (g_hookConsolePrint) { g_pMetaHookAPI->UnHook(g_hookConsolePrint); g_hookConsolePrint = nullptr; }
		g_pfnOrigConPrintf = nullptr;
		g_pfnOrigConDPrintf = nullptr;
		g_pfnOrigConsolePrint = nullptr;
	}

	uint64_t NextSeq()
	{
		std::lock_guard<std::mutex> lock(g_cap.mutex);
		return g_cap.nextSeq;
	}

	std::vector<std::string> GetLines(uint64_t begin, uint64_t end)
	{
		std::lock_guard<std::mutex> lock(g_cap.mutex);
		std::vector<std::string> out;
		if (end <= begin)
			return out;
		for (uint64_t seq = (begin > g_cap.baseSeq ? begin : g_cap.baseSeq);
			seq < end && seq < g_cap.nextSeq; ++seq)
		{
			size_t idx = (size_t)(seq - g_cap.baseSeq);
			if (idx < g_cap.lines.size())
				out.push_back(g_cap.lines[idx]);
		}
		return out;
	}

	void SetSink(LineSink sink)
	{
		std::lock_guard<std::mutex> lock(g_cap.mutex);
		g_cap.sink = sink;
	}

	bool Available()
	{
		return g_pfnOrigConsolePrint != nullptr || g_pfnOrigConPrintf != nullptr;
	}
}
