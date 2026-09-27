#include "console_bridge.h"
#include "config.h"
#include "output_capture.h"
#include "plugins.h"

#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

namespace
{
	struct QueuedCommand
	{
		uint64_t token;
		std::string text;
	};

	struct PendingResponse
	{
		uint64_t token;
		uint64_t beginSeq;
		uint64_t completeOnFrame;
	};

	struct BridgeState
	{
		std::mutex queueMutex;
		std::deque<QueuedCommand> queue;

		std::mutex respMutex;
		std::condition_variable respCv;
		std::map<uint64_t, std::string> completed;
		std::vector<PendingResponse> pending;

		std::thread stdinThread;
		std::atomic<uint64_t> nextToken{ 1 };
		std::atomic<bool> running{ false };
		bool hasRealConsole = false;
		uint64_t frameCount = 0;
	};
	BridgeState g_bridge;

	// pfnClientCmd feeds the engine command buffer (it is ClientCmd ->
	// Cbuf_AddText internally), so typed-console semantics are preserved even
	// without symbol resolution. The svencoop gamedata catalog does not carry
	// Cbuf_AddText, so ResolveGameSymbol is only a best-effort upgrade.
	typedef void (*Cbuf_AddText_t)(const char *text);
	Cbuf_AddText_t g_pfnCbufAddText = nullptr;
	bool g_cbufTried = false;

	void WriteOutLocked(const std::string& text)
	{
		if (!g_bridge.hasRealConsole)
			return;
		fputs(text.c_str(), stdout);
		if (!text.empty() && text.back() != '\n')
			fputc('\n', stdout);
		fflush(stdout);
	}

	void CaptureSink(const std::string& line)
	{
		WriteOutLocked(line);
	}

	void StdinThreadMain()
	{
		std::string line;
		while (g_bridge.running)
		{
			if (!std::getline(std::cin, line))
			{
				// stdin closed (automation harness exited): stop reading.
				break;
			}
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if (!line.empty())
				ConsoleBridge::SubmitCommand(line);
		}
	}
}

namespace ConsoleBridge
{
	void WriteOut(const std::string& text)
	{
		WriteOutLocked(text);
	}

	void Init()
	{
		// LoadClient runs again on every map change; init only once.
		if (g_bridge.running.exchange(true))
			return;

		// If the process already has usable std handles (launched from a
		// terminal or with piped stdio by a test harness) use them as-is;
		// otherwise create the CLI console window.
		HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
		bool outOk = hOut != NULL && hOut != INVALID_HANDLE_VALUE &&
			GetFileType(hOut) != FILE_TYPE_UNKNOWN;
		bool inOk = hIn != NULL && hIn != INVALID_HANDLE_VALUE &&
			GetFileType(hIn) != FILE_TYPE_UNKNOWN;

		if (!outOk || !inOk)
		{
			if (AllocConsole())
			{
				FILE *f = nullptr;
				freopen_s(&f, "CONOUT$", "wb", stdout);
				freopen_s(&f, "CONIN$", "rb", stdin);
				freopen_s(&f, "CONOUT$", "wb", stderr);
				SetConsoleTitleA("halflife-cli");
				outOk = inOk = true;
			}
		}

		// NOTE: no setvbuf here — calling setvbuf on the GUI process's stdout
		// fast-fails (0xC0000409); WriteOutLocked flushes per line instead.

		// stdout may be writable while stdin is not (RCON-only mode); the
		// reader thread only starts when stdin is usable.
		g_bridge.hasRealConsole = outOk;

		OutputCapture::SetSink(CaptureSink);

		if (inOk)
			g_bridge.stdinThread = std::thread(StdinThreadMain);
	}

	void Shutdown()
	{
		if (!g_bridge.running.exchange(false))
			return;
		if (g_bridge.stdinThread.joinable())
			g_bridge.stdinThread.detach(); // blocked on getline; process exit will tear it down
		{
			std::lock_guard<std::mutex> lock(g_bridge.respMutex);
			g_bridge.pending.clear();
			g_bridge.completed.clear();
			g_bridge.respCv.notify_all();
		}
	}

	uint64_t SubmitCommand(const std::string& cmd)
	{
		uint64_t token = g_bridge.nextToken++;
		std::lock_guard<std::mutex> lock(g_bridge.queueMutex);
		g_bridge.queue.push_back({ token, cmd });
		return token;
	}

	bool WaitForResponse(uint64_t token, std::string& out, int timeoutMs)
	{
		std::unique_lock<std::mutex> lock(g_bridge.respMutex);
		bool ok = g_bridge.respCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]
		{
			return g_bridge.completed.count(token) != 0;
		});
		if (!ok)
			return false;
		out = g_bridge.completed[token];
		g_bridge.completed.erase(token);
		return true;
	}

	static void ExecuteCommand(const std::string& cmd)
	{
		if (g_pfnCbufAddText)
		{
			g_pfnCbufAddText((cmd + "\n").c_str());
		}
		else
		{
			// explicit newline: svengine's ClientCmd may not terminate the line
			gEngfuncs.pfnClientCmd((cmd + "\n").c_str());
		}
	}

	void PumpCommands()
	{
		// resolve Cbuf_AddText once (best effort)
		if (!g_pfnCbufAddText && !g_cbufTried)
		{
			g_cbufTried = true;
			PVOID p = nullptr;
			if (g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetEngineBase(), "Cbuf_AddText",
				MH_GAMESYMBOL_KIND_FUNCTION, &p) == MH_GAMESYMBOL_OK && p)
				g_pfnCbufAddText = (Cbuf_AddText_t)p;
		}

		// finalize responses from earlier frames
		{
			std::lock_guard<std::mutex> respLock(g_bridge.respMutex);
			uint64_t now = g_bridge.frameCount;
			std::vector<PendingResponse> still;
			for (auto& p : g_bridge.pending)
			{
				if (p.completeOnFrame <= now)
				{
					uint64_t end = OutputCapture::NextSeq();
					std::vector<std::string> lines = OutputCapture::GetLines(p.beginSeq, end);
					std::string text;
					for (size_t i = 0; i < lines.size(); ++i)
					{
						if (i) text += '\n';
						text += lines[i];
					}
					if (text.empty())
					{
						char tbuf[160];
						_snprintf_s(tbuf, sizeof(tbuf), _TRUNCATE,
							"pump:token=%llu begin=%llu end=%llu EMPTY",
							(unsigned long long)p.token,
							(unsigned long long)p.beginSeq, (unsigned long long)end);
									}
					g_bridge.completed[p.token] = std::move(text);
				}
				else
				{
					still.push_back(p);
				}
			}
			g_bridge.pending.swap(still);
			if (!g_bridge.completed.empty())
				g_bridge.respCv.notify_all();
		}

		// drain the command queue
		std::deque<QueuedCommand> batch;
		{
			std::lock_guard<std::mutex> lock(g_bridge.queueMutex);
			batch.swap(g_bridge.queue);
		}
		for (auto& qc : batch)
		{
			uint64_t beginSeq = OutputCapture::NextSeq();
			ExecuteCommand(qc.text);
			std::lock_guard<std::mutex> respLock(g_bridge.respMutex);
			// complete on the NEXT frame so output printed during this frame's
			// Cbuf_Execute (which runs after HUD_Frame) is included
			g_bridge.pending.push_back({ qc.token, beginSeq, g_bridge.frameCount + 1 });
		}

		++g_bridge.frameCount;
	}
}
