// Exercise the real capture callback without loading a VGUI DLL into the test.
#include "../src/console/output_capture.cpp"
#include "console/console_bridge.h"
#include "config/config.h"
#include "core/cli_commands.h"
#include "input/engine_input.h"
#include "input/focus_lock.h"
#include "input/input_lock.h"
#include "rcon/rcon_server.h"
#include "usermsg/usermsg_monitor.h"
#include "window/window_manager.h"

#include <cstdio>
#include <stdexcept>

namespace
{
    CliConfig config;
    std::string gameDirectory;
    std::string scenario;
    int configLoads = 0;
    int commands = 0;

    void Check(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(message);
    }

    // Inert MetaHook symbol services: the plugin resolves no game symbols of its
    // own, so nothing is expected to reach these.
    mh_gamesymbol_status_t Resolve(PVOID, const char*, mh_gamesymbol_kind_t, PVOID* address)
    {
        *address = nullptr;
        return MH_GAMESYMBOL_SYMBOL_NOT_FOUND;
    }

    hook_t* Hook(void*, void*, void**)
    {
        return nullptr;
    }

    int ClientCommand(const char* text)
    {
        Check(std::string("echo stdin_probe\n") == text, "unexpected stdin command");
        ++commands;
        return 1;
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

// Unrelated backends are inert. LoadClient may be entered twice (a new map
// re-runs it); the disabled_queue scenario reuses that to check the bridge has
// already transitioned to its configured state.
namespace RconServer {
    void Install() {}
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
    api.UnHook = [](hook_t*) -> BOOL { return TRUE; };
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
        plugin.LoadEngine(&engine);
        Check(0 == configLoads, "early phase must not read config");
        Check(consoleWindow == GetConsoleWindow(), "early phase must not allocate a console");

        if (scenario == "disabled" || scenario == "disabled_queue" || scenario == "backend" || scenario == "stdin")
        {
            cl_exportfuncs_t client{};
            plugin.LoadClient(&client);
            const auto queued = scenario == "disabled_queue" ? ConsoleBridge::SubmitCommand("echo stdin_probe") : 0;
            if (scenario != "backend") plugin.LoadClient(&client);
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

        plugin.ExitGame(0);
        plugin.Shutdown();
        ConsoleBridge::WriteOut("UNLOADED_OUTPUT_MUST_BE_SILENT");
        if (scenario == "restart")
        {
            plugin.LoadEngine(&engine);
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
