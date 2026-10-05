#include "console/console_bridge.h"
#include "config/config.h"
#include "console/output_capture.h"
#include "core/plugins.h"

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
				// HWND_TOPMOST is a persistent z-order band, so setting it once
				// here keeps the console above the game until something demotes
				// it. Only for the console we just created — the piped-stdio
				// path must not touch the parent's console window.
				if (CLI_Config().console_topmost)
					SetWindowPos(GetConsoleWindow(), HWND_TOPMOST, 0, 0, 0, 0,
						SWP_NOMOVE | SWP_NOSIZE);
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
		{
			// Cancellation can race the reader entering getline. Retry until it
			// exits, then join before an engine restart can unload this DLL.
			HANDLE reader = static_cast<HANDLE>(g_bridge.stdinThread.native_handle());
			while (WaitForSingleObject(reader, 0) == WAIT_TIMEOUT)
			{
				CancelSynchronousIo(reader);
				WaitForSingleObject(reader, 50);
			}
			g_bridge.stdinThread.join();
		}
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
		// clientcommand path only: pfnClientCmd is a stable cl_enginefunc_t
		// entry feeding the engine command buffer, with typed-console
		// semantics. Resolving Cbuf_AddText from the engine by symbol/pattern
		// is build-dependent and deliberately not used. The explicit newline
		// is needed because svengine's ClientCmd may not terminate the line.
		gEngfuncs.pfnClientCmd((cmd + "\n").c_str());
	}

	void PumpCommands()
	{
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
