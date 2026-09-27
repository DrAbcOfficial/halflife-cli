#include "output_capture.h"
#include "plugins.h"

#include <mutex>
#include <deque>
#include <cstdarg>
#include <cstdio>

typedef void (*Con_Printf_t)(const char *fmt, ...);

static Con_Printf_t g_pfnOrigConPrintf = nullptr;
static Con_Printf_t g_pfnOrigConDPrintf = nullptr;
static Con_Printf_t g_pfnOrigConWarning = nullptr;

static hook_t *g_hookConPrintf = nullptr;
static hook_t *g_hookConDPrintf = nullptr;
static hook_t *g_hookConWarning = nullptr;

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

static void FormatAndForward(Con_Printf_t original, const char *fmt, va_list args)
{
	char buf[16384];
	int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
	if (n < 0)
		n = sizeof(buf) - 1;
	// The original function is variadic; forward the fully formatted text so
	// the engine prints exactly what it intended.
	original("%s", buf);
}

static void Hook_Con_Printf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	{
		char buf[16384];
		_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		OnText(buf);
	}
	va_end(args);
	FormatAndForward(g_pfnOrigConPrintf, fmt, args);
}

static void Hook_Con_DPrintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	{
		char buf[16384];
		_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		OnText(buf);
	}
	va_end(args);
	FormatAndForward(g_pfnOrigConDPrintf, fmt, args);
}

static void Hook_Con_Warning(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	{
		char buf[16384];
		_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
		OnText(buf);
	}
	va_end(args);
	FormatAndForward(g_pfnOrigConWarning, fmt, args);
}

namespace OutputCapture
{
	bool Install()
	{
		if (g_pfnOrigConPrintf)
			return true;

		PVOID addr = nullptr;
		auto resolve = [&](const char *name) -> PVOID
		{
			PVOID p = nullptr;
			if (g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetEngineBase(), name,
				MH_GAMESYMBOL_KIND_FUNCTION, &p) == MH_GAMESYMBOL_OK && p)
				return p;
			return nullptr;
		};

		PVOID pPrintf = resolve("Con_Printf");
		if (!pPrintf)
			return false;

		g_hookConPrintf = g_pMetaHookAPI->InlineHook(pPrintf, Hook_Con_Printf, (void **)&g_pfnOrigConPrintf);

		// Optional captures: DPrintf (developer>0 output) and Warning may be
		// absent from the gamedata catalog depending on the engine generation.
		PVOID pDPrintf = resolve("Con_DPrintf");
		if (pDPrintf)
			g_hookConDPrintf = g_pMetaHookAPI->InlineHook(pDPrintf, Hook_Con_DPrintf, (void **)&g_pfnOrigConDPrintf);

		PVOID pWarning = resolve("Con_Warning");
		if (pWarning)
			g_hookConWarning = g_pMetaHookAPI->InlineHook(pWarning, Hook_Con_Warning, (void **)&g_pfnOrigConWarning);

		return g_hookConPrintf != nullptr;
	}

	void Shutdown()
	{
		if (g_hookConPrintf) { g_pMetaHookAPI->UnHook(g_hookConPrintf); g_hookConPrintf = nullptr; }
		if (g_hookConDPrintf) { g_pMetaHookAPI->UnHook(g_hookConDPrintf); g_hookConDPrintf = nullptr; }
		if (g_hookConWarning) { g_pMetaHookAPI->UnHook(g_hookConWarning); g_hookConWarning = nullptr; }
		g_pfnOrigConPrintf = nullptr;
		g_pfnOrigConDPrintf = nullptr;
		g_pfnOrigConWarning = nullptr;
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
		return g_pfnOrigConPrintf != nullptr;
	}
}
