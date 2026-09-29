#include "console/output_capture.h"
#include "core/plugins.h"

#include <IVGUI2Extension.h>

#include <mutex>
#include <deque>

// Console output is captured through the VGUI2Extension plugin's GameConsole
// callbacks: VGUI2Extension VFTHooks the IGameConsole interface (version
// "GameConsole003", served by GameUI.dll) and dispatches every Printf/DPrintf
// the engine sends to the vgui console to registered observers. Riding that
// documented interface keeps the hook stable across engine builds — no
// byte-pattern scanning of engine code is involved. VGUI2Extension.dll is a
// hard dependency; without it capture is unavailable (commands still run).
//
// Callbacks are invoked in two passes (pre/post) for each print and in
// descending GetAltitude() order. We capture the pre pass only and never set
// CallbackContext->Result, so the chain continues and the real console keeps
// behaving normally. A high altitude puts us ahead of lower-altitude
// observers (e.g. ABCEnchance registers 0 and SUPERCEDEs the real print).

namespace
{
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

	// Split the streamed text into lines, keeping the trailing partial chunk
	// for the next call (prints rarely arrive newline-terminated as a whole).
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

	// Run before lower-altitude observers so a downstream SUPERCEDE (e.g.
	// ABCEnchance replacing the console) cannot starve our capture.
	class CGameConsoleCaptureCallbacks : public IVGUI2Extension_GameConsoleCallbacks
	{
	public:
		int GetAltitude() const override { return 1000; }

		void Activate(VGUI2Extension_CallbackContext *) override {}
		void Initialize(VGUI2Extension_CallbackContext *) override {}
		void Hide(VGUI2Extension_CallbackContext *) override {}
		void Clear(VGUI2Extension_CallbackContext *) override {}
		void IsConsoleVisible(VGUI2Extension_CallbackContext *) override {}

		void Printf(IVGUI2Extension_String *str, VGUI2Extension_CallbackContext *ctx) override
		{
			if (!ctx->IsPost)
				OnText(str->c_str());
		}

		void DPrintf(IVGUI2Extension_String *str, VGUI2Extension_CallbackContext *ctx) override
		{
			if (!ctx->IsPost)
				OnText(str->c_str());
		}

		void SetParent(vgui::VPANEL, VGUI2Extension_CallbackContext *) override {}
	};

	static CGameConsoleCaptureCallbacks s_GameConsoleCallbacks;

	static IVGUI2Extension *g_pVGUI2Extension = nullptr;
}

namespace OutputCapture
{
	bool Install()
	{
		// LoadClient re-runs on every map change; register only once.
		if (g_pVGUI2Extension)
			return true;

		HMODULE hVGUI2Extension = GetModuleHandleA("VGUI2Extension.dll");
		if (!hVGUI2Extension)
			return false;

		CreateInterfaceFn factory = Sys_GetFactory((HINTERFACEMODULE)hVGUI2Extension);
		if (!factory)
			return false;

		g_pVGUI2Extension = (IVGUI2Extension *)factory(VGUI2_EXTENSION_INTERFACE_VERSION, nullptr);
		if (!g_pVGUI2Extension)
			return false;

		g_pVGUI2Extension->RegisterGameConsoleCallbacks(&s_GameConsoleCallbacks);
		return true;
	}

	void Shutdown()
	{
		if (g_pVGUI2Extension)
		{
			g_pVGUI2Extension->UnregisterGameConsoleCallbacks(&s_GameConsoleCallbacks);
			g_pVGUI2Extension = nullptr;
		}
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
		return g_pVGUI2Extension != nullptr;
	}
}
