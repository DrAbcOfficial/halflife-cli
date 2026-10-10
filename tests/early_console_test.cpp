// Exercise the real capture callback without loading a VGUI DLL into the test.
#include "../src/console/output_capture.cpp"
#include "console/console_bridge.h"
#include "console/sys_error.h"
#include "config/config.h"
#include "core/cli_commands.h"
#include "input/engine_input.h"
#include "input/focus_lock.h"
#include "input/input_lock.h"
#include "rcon/rcon_server.h"
#include "usermsg/usermsg_monitor.h"
#include "window/window_manager.h"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace
{
    using FatalFn = void(__cdecl*)(const char*, ...);
    FatalFn fatal = nullptr;
    CliConfig config;
    std::string gameDirectory;
    std::string scenario;
    int configLoads = 0;
    int hookAttempts = 0;
    int originals = 0;
    int commands = 0;
    bool failResolve = false;
    bool failHook = false;

    void Check(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(message);
    }

    void __cdecl OriginalFatal(const char* fmt, ...)
    {
        char text[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        Check(std::string("fatal 100% value=7") == text, "original fatal parameters changed");
        ++originals;
    }

    mh_gamesymbol_status_t Resolve(PVOID, const char* name, mh_gamesymbol_kind_t kind, PVOID* address)
    {
        Check(std::string("Sys_Error") == name && MH_GAMESYMBOL_KIND_FUNCTION == kind, "unexpected symbol");
        *address = failResolve ? nullptr : reinterpret_cast<void*>(OriginalFatal);
        return failResolve ? MH_GAMESYMBOL_SYMBOL_NOT_FOUND : MH_GAMESYMBOL_OK;
    }

    hook_t* Hook(void*, void* replacement, void** original)
    {
        ++hookAttempts;
        if (failHook) return nullptr;
        fatal = reinterpret_cast<FatalFn>(replacement);
        *original = reinterpret_cast<void*>(OriginalFatal);
        return reinterpret_cast<hook_t*>(1);
    }

    class Text : public IVGUI2Extension_String
    {
        std::string value;
    public:
        explicit Text(const char* text) : value(text) {}
        const char* c_str() const override { return value.c_str(); }
        const char* data() const override { return value.data(); }
        size_t length() const override { return value.length(); }
        size_t capacity() const override { return value.capacity(); }
        void resize(size_t n) override { value.resize(n); }
        void assign(const char* s) override { value.assign(s); }
        void assign2(const char* s, size_t n) override { value.assign(s, n); }
        void clear() override { value.clear(); }
    };

    void Print(const char* text)
    {
        Text value(text);
        VGUI2Extension_CallbackContext context{};
        s_GameConsoleCallbacks.Printf(&value, &context);
    }

    int ClientCommand(const char* text)
    {
        Check(std::string("echo stdin_probe\n") == text, "unexpected stdin command");
        ++commands;
        return 1;
    }

    void RaiseFatal()
    {
        Check(nullptr != fatal, "Sys_Error must be hooked before LoadClient");
        fatal("fatal 100%% value=%d", 7);
    }
}

CliConfig& CLI_Config() { return config; }
bool CliConfig::Load()
{
    ++configLoads;
    console = scenario != "disabled" && scenario != "disabled_queue";
    capture = false;
    return true;
}

// Unrelated backends are inert. The RCON stub can fail during client startup
// to verify the bridge has already transitioned to its configured state.
namespace RconServer {
    void Install() { if (scenario == "backend") RaiseFatal(); }
    void RegisterCommands() {} void OnClientReady() {} void AfterCommands() {}
    void OnEngineShutdown() {} void Shutdown() {} bool UsesMainFrame() { return false; }
}
namespace CliCommands { void RegisterAll() {} }
namespace VGUI2 { void Frame() {} void Shutdown() {} }
namespace UserMsgMonitor { void Init() {} void Shutdown() {} void OnHudInit() {} void OnHudVidInit() {} void Frame() {} }
namespace InputLock { void InstallHooks() {} void SetActive(bool) {} void Shutdown() {} }
namespace FocusLock { void Install() {} void SetActive(bool) {} void OnExitGame() {} void Shutdown() {} }
namespace EngineInput { void Install() {} void SetBlockInput(bool) {} void OnExitGame() {} void Shutdown() {} }
namespace WindowManager { void SetMode(int) {} void ApplyConfiguredMode() {} void Restore() {} }

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    scenario = argv[1];
    gameDirectory = argv[2];
    metahook_api_t api{};
    api.GetGameDirectory = []() { return gameDirectory.c_str(); };
    api.GetEngineBase = []() -> PVOID { return nullptr; };
    api.GetEngineType = []() { return 0; };
    api.GetEngineBuildnum = []() -> DWORD { return 0; };
    api.GetEngineTypeName = []() { return "test"; };
    api.ResolveGameSymbol = Resolve;
    api.GetGameSymbolStatusString = [](mh_gamesymbol_status_t) { return "test resolution failure"; };
    api.InlineHook = Hook;
    api.UnHook = [](hook_t*) -> BOOL { fatal = nullptr; return TRUE; };
    mh_interface_t interfaces{};
    mh_enginesave_t saved{};
    cl_enginefunc_t engine{};
    engine.pfnClientCmd = ClientCommand;
    IPluginsV4 plugin;
    plugin.Init(&api, &interfaces, &saved);
    HANDLE savedOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE consoleWindow = GetConsoleWindow();
    try
    {
        if (scenario == "no_stdout") SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
        if (scenario == "no_stdin") SetStdHandle(STD_INPUT_HANDLE, nullptr);
        failResolve = scenario == "retry_resolve";
        failHook = scenario == "retry_hook";
        plugin.LoadEngine(&engine);
        Check(0 == configLoads, "early phase must not read config");
        Check(consoleWindow == GetConsoleWindow(), "early phase must not allocate a console");

        if (scenario == "retry_resolve" || scenario == "retry_hook")
        {
            Check(!SysError::Hooked(), "injected installation failure ignored");
            Check(*SysError::HookError(), "installation failure must be reported");
            failResolve = failHook = false;
        }
        else
            Check(SysError::Hooked(), "Sys_Error must be hooked before LoadClient");

        if (scenario == "disabled" || scenario == "disabled_queue" || scenario == "backend" || scenario == "stdin" ||
            scenario == "flush" || scenario == "retry_resolve" || scenario == "retry_hook")
        {
            cl_exportfuncs_t client{};
            plugin.LoadClient(&client);
            Check(SysError::Hooked(), "late phase must retry hook installation");
            const int installedAttempts = hookAttempts;
            const auto queued = scenario == "disabled_queue" ? ConsoleBridge::SubmitCommand("echo stdin_probe") : 0;
            if (scenario != "backend") plugin.LoadClient(&client);
            Check(installedAttempts == hookAttempts, "duplicate hook installed");
            if (scenario == "disabled_queue")
            {
                ConsoleBridge::PumpCommands();
                ConsoleBridge::PumpCommands();
                std::string response;
                Check(1 == commands && ConsoleBridge::WaitForResponse(queued, response, 10),
                    "disabling console must preserve RCON commands and responses");
            }
        }

        if (scenario == "stdin")
        {
            const auto deadline = GetTickCount64() + 3000;
            while (!commands && GetTickCount64() < deadline)
            {
                ConsoleBridge::PumpCommands();
                Sleep(10);
            }
            Check(1 == commands, "late init did not start exactly one stdin reader");
        }
        if (scenario == "flush")
        {
            const auto begin = OutputCapture::NextSeq();
            Print("complete\npartial");
            RaiseFatal();
            RaiseFatal();
            const auto lines = OutputCapture::GetLines(begin, OutputCapture::NextSeq());
            Check(std::vector<std::string>({"complete", "partial"}) == lines,
                "fatal flush must emit partial exactly once");
        }
        else if (scenario != "backend") RaiseFatal();

        Check((scenario == "flush" ? 2 : 1) == originals, "original fatal not called");
        plugin.ExitGame(0);
        // Fatal output must remain available through engine teardown.
        RaiseFatal();
        plugin.Shutdown();
        Check(!SysError::Hooked(), "unload did not remove hook");
        ConsoleBridge::WriteOut("UNLOADED_OUTPUT_MUST_BE_SILENT");
        if (scenario == "restart")
        {
            plugin.LoadEngine(&engine);
            RaiseFatal();
            plugin.Shutdown();
        }
        SetStdHandle(STD_OUTPUT_HANDLE, savedOut);
        fprintf(stderr, "PASS %s\n", scenario.c_str());
        return 0;
    }
    catch (const std::exception& error)
    {
        SetStdHandle(STD_OUTPUT_HANDLE, savedOut);
        fprintf(stderr, "FAIL %s: %s\n", scenario.c_str(), error.what());
        plugin.Shutdown();
        return 1;
    }
}
