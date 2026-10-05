#include "rcon/rcon_server.h"
#include "rcon/sven_udp.h"
#include "rcon/tcp_server.h"
#include "core/plugins.h"
#include "config/config.h"
#include "console/console_bridge.h"
#include "console/output_capture.h"
#include <windows.h>
#include <direct.h>
#include <cstdio>
#include <ctime>

namespace
{
    bool g_sven = false;
    bool g_ready = false;
    bool g_attempted = false;
    bool g_stopping = false;
    bool g_prepared = false;
    std::string g_error;
    std::string g_directory;

    unsigned long long ProcessStart()
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return 0;
        return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    bool AtomicFile(const char* name, const std::string& contents)
    {
        if (g_directory.empty())
            return false;
        std::string path = g_directory + "/" + name;
        std::string temporary = path + "." + std::to_string(GetCurrentProcessId()) + ".tmp";
        FILE* file = nullptr;
        if (fopen_s(&file, temporary.c_str(), "wb") || !file)
            return false;
        bool ok = fwrite(contents.data(), 1, contents.size(), file) == contents.size();
        if (fclose(file))
            ok = false;
        if (ok)
            ok = MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok)
            DeleteFileA(temporary.c_str());
        return ok;
    }
    bool Publish(const char* status)
    {
        char record[512];
        _snprintf_s(record, sizeof(record), _TRUNCATE,
            "{\"version\":1,\"status\":\"%s\",\"protocol\":\"%s\",\"bind\":\"%s\",\"port\":%u,"
            "\"pid\":%lu,\"process_start_filetime\":\"%llu\",\"published_at\":%llu}\n",
            status, RconServer::Protocol(), RconServer::CurrentAddress().c_str(), RconServer::CurrentPort(),
            GetCurrentProcessId(), ProcessStart(), static_cast<unsigned long long>(time(nullptr)));
        return AtomicFile("halflifecli.endpoint.json", record);
    }
    void Report(const std::string& message)
    {
        gEngfuncs.Con_Printf("%s\n", message.c_str());
        if (!OutputCapture::Available())
            ConsoleBridge::WriteOut(message);
    }
    void Start()
    {
        if (g_attempted || g_stopping || !g_ready || !CLI_Config().rcon)
            return;
        g_attempted = true;
        const auto& config = CLI_Config();
        RconServer::StartResult result;
        if (g_sven)
        {
            if (!g_error.empty())
                result.error = g_error;
            else
                result = SvenUdp::Start(config.rcon_password, config.rcon_allowed_ips);
            if (config.rcon_legacy_binding)
                Report("halflife-cli: Sven ignores [rcon].bind/port; use engine ip/ip_hostport/hostport/port or -port");
        }
        else
            result = TcpRcon::Start(config.rcon_bind, static_cast<unsigned short>(config.rcon_port), config.rcon_password, config.rcon_allowed_ips);
        if (result.ok)
        {
            // Publish metadata last: clients never infer readiness from port alone.
            if (!AtomicFile("halflifecli.port", std::to_string(result.port) + "\n") || !Publish("ready"))
            {
                result.ok = false;
                result.error = "could not publish RCON endpoint metadata";
                if (g_sven) SvenUdp::Stop(); else TcpRcon::Shutdown();
            }
        }
        if (!result.ok)
        {
            g_error = result.error;
            if (!g_directory.empty())
                DeleteFileA((g_directory + "/halflifecli.port").c_str());
            Publish("failed");
            Report("halflife-cli: RCON failed to start (" + g_error + ")");
            return;
        }
        Report("halflife-cli: RCON listening on " + RconServer::CurrentAddress() + ":" +
            std::to_string(result.port) + " (" + RconServer::Protocol() + ", password: " +
            (RconServer::PasswordSet() ? "set" : "none") + ")");
    }
}

namespace RconServer
{
    void Install()
    {
        if (g_prepared)
            return;
        g_prepared = true;
        g_sven = g_iEngineType == ENGINE_SVENGINE;
        g_ready = g_attempted = g_stopping = false;
        g_error.clear();
        const char* gameDir = g_pMetaHookAPI->GetGameDirectory();
        if (gameDir && *gameDir)
        {
            g_directory = std::string(gameDir) + "/metahook/configs/halflifecli";
            _mkdir(g_directory.c_str());
        }
        Publish(CLI_Config().rcon ? "initializing" : "disabled");
        if (!g_directory.empty())
            DeleteFileA((g_directory + "/halflifecli.port").c_str());
        if (g_sven && CLI_Config().rcon)
            SvenUdp::Install(g_error);
    }
    void OnClientReady()
    {
        g_ready = true;
    }
    void AfterCommands() { Start(); }
    void OnEngineShutdown()
    {
        if (g_stopping)
            return;
        g_stopping = true;
        SvenUdp::Stop();
        if (!g_sven) TcpRcon::Shutdown();
        Publish("stopped");
        if (!g_directory.empty())
            DeleteFileA((g_directory + "/halflifecli.port").c_str());
    }
    void Shutdown()
    {
        OnEngineShutdown();
        SvenUdp::Uninstall();
        g_prepared = false;
    }
    bool UsesMainFrame() { return SvenUdp::Installed(); }
    bool Running() { return g_sven ? SvenUdp::Running() : TcpRcon::Running(); }
    unsigned short CurrentPort() { return Running() ? (g_sven ? SvenUdp::CurrentPort() : TcpRcon::CurrentPort()) : 0; }
    std::string CurrentAddress() { return g_sven ? SvenUdp::CurrentAddress() : CLI_Config().rcon_bind; }
    const char* Protocol() { return g_sven ? "goldsrc-udp" : "source-tcp"; }
    bool PasswordSet()
    {
        return g_sven && Running() ? gEngfuncs.pfnGetCvarString("rcon_password")[0] != 0 : !CLI_Config().rcon_password.empty();
    }
    const std::string& Error() { return g_error; }
}
