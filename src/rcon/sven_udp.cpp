#include <winsock2.h>
#include <ws2tcpip.h>
#include "rcon/sven_udp.h"
#include "rcon/sven_policy.h"
#include "core/plugins.h"
#include "console/console_bridge.h"
#include "util/text.h"
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace
{
    constexpr int ServerSocket = 1;
    constexpr int Closed = 3;
    constexpr int RedirectPacket = 2;
    constexpr int CommandSource = 1;
    constexpr size_t OutputCapacity = 1400;
    constexpr size_t TextChunk = 1200;
    constexpr int RequestLimit = 510;
    constexpr int MinMetaHookAPI = 115;
    static_assert(sizeof(SOCKET) == 4 && sizeof(int) == 4);

    // All private functions are x86 cdecl. Do not replace netadr_svengine_t by SDK netadr_t.
    struct Engine
    {
        void (__cdecl *Cbuf_Execute)();
        void (__cdecl *NET_Config)(int);
        int (__cdecl *NET_IsLocalAddress)(netadr_svengine_t);
        void (__cdecl *SVC_ServiceChallenge)();
        void (__cdecl *SV_Rcon)(netadr_svengine_t*);
        void (__cdecl *SV_FlushRedirect)();
        int (__cdecl *NET_GetPacket)(int);
        void (__cdecl *NET_SendPacket)(int, int, const void*, netadr_svengine_t);
        int (__cdecl *SV_FilterPacket)();
        void (__cdecl *SV_SendBan)();
        void (__cdecl *SV_HandleRconPacket)();
        int (__cdecl *SV_CheckChallenge)(netadr_svengine_t*, int);
        int (__cdecl *SV_CheckRconFailure)(netadr_svengine_t*);
        void (__cdecl *SV_AddFailedRcon)(netadr_svengine_t*);
        void (__cdecl *Cmd_ExecuteString)(const char*, int);
        void (__cdecl *SV_BeginRedirect)(int, netadr_svengine_t*);
        void (__cdecl *SV_EndRedirect)();
        SOCKET* sockets;
        netadr_svengine_t* from;
        unsigned char* message;
        unsigned char* server;
        int* initialized;
        int* active;
        int* redirected;
        netadr_svengine_t* redirectTo;
        char* output;
        uint32_t serverActiveOffset, dataOffset, sizeOffset;
        unsigned char* frameCall;
    } g{};
    void* g_hostShutdown = nullptr;
    void* g_netShutdown = nullptr;
    std::vector<hook_t*> g_hooks;
    std::vector<SvenRconPolicy::IP> g_allowed;
    bool g_running = false;
    bool g_installed = false;
    bool g_frame = false;

    void __cdecl BeforeShutdown()
    {
        RconServer::OnEngineShutdown();
        ConsoleBridge::Shutdown();
    }

    // Preserve the entire entry state (including the original return value).
    // These are teardown entries, not opportunities to guess a decompiler's
    // inferred register arguments or to invoke a trampoline after unhooking it.
    __declspec(naked) void HookHostShutdown()
    {
        __asm { pushfd }
        __asm { pushad }
        __asm { call BeforeShutdown }
        __asm { popad }
        __asm { popfd }
        __asm { jmp dword ptr [g_hostShutdown] }
    }
    __declspec(naked) void HookNetShutdown()
    {
        __asm { pushfd }
        __asm { pushad }
        __asm { call BeforeShutdown }
        __asm { popad }
        __asm { popfd }
        __asm { jmp dword ptr [g_netShutdown] }
    }

    bool ServerActive() { return *reinterpret_cast<int*>(g.server + g.serverActiveOffset) != 0; }
    void Poll()
    {
        if (!g_running || !*g.initialized || *g.active == Closed || ServerActive())
            return;
        while (g_running && !ServerActive() && *g.active != Closed && g.NET_GetPacket(ServerSocket))
        {
            if (g.SV_FilterPacket())
            {
                g.SV_SendBan();
                continue;
            }
            int length = *reinterpret_cast<int*>(g.message + g.sizeOffset);
            auto data = *reinterpret_cast<unsigned char**>(g.message + g.dataOffset);
            if (length >= 4 && data && data[0] == 255 && data[1] == 255 && data[2] == 255 && data[3] == 255)
                g.SV_HandleRconPacket();
        }
    }

    __declspec(noinline) void __cdecl HookCbuf()
    {
        const bool mainFrame = _ReturnAddress() == g.frameCall + 5 && !g_frame;
        if (!mainFrame)
        {
            g.Cbuf_Execute();
            return;
        }
        g_frame = true;
        ConsoleBridge::PumpCommands();
        Poll();
        g.Cbuf_Execute();
        RconServer::AfterCommands();
        g_frame = false;
    }

    void __cdecl HookConfig(int multiplayer)
    {
        g.NET_Config(g_running ? 1 : multiplayer);
    }
    int __cdecl HookLocal(netadr_svengine_t address)
    {
        // No change to native semantics when disabled or initialization failed.
        return g_running ? SvenRconPolicy::Local(address) : g.NET_IsLocalAddress(address);
    }
    void __cdecl HookChallenge()
    {
        if (g_running && gEngfuncs.Cmd_Argc() == 2 &&
            !_stricmp(gEngfuncs.Cmd_Argv(1), "rcon") && !SvenRconPolicy::Allowed(*g.from, g_allowed))
            return;
        g.SVC_ServiceChallenge();
    }

    void __cdecl HookFlush()
    {
        if (!g_running || *g.redirected != RedirectPacket)
        {
            g.SV_FlushRedirect();
            return;
        }
        const netadr_svengine_t destination = *g.redirectTo;
        const int mode = *g.redirected;
        *g.redirected = 0; // NET_SendPacket diagnostics must not recurse
        size_t remaining = strnlen_s(g.output, OutputCapacity - 1);
        const char* text = g.output;
        do
        {
            unsigned char packet[TextChunk + 7] = {255, 255, 255, 255, 'l'};
            size_t count = (std::min)(remaining, TextChunk);
            memcpy(packet + 5, text, count); // two zero terminators, including empty output
            g.NET_SendPacket(ServerSocket, static_cast<int>(count + 7), packet, destination);
            text += count;
            remaining -= count;
        } while (remaining);
        g.output[0] = 0;
        *g.redirected = mode;
    }

    void __cdecl HookRcon(netadr_svengine_t* from)
    {
        if (!g_running)
        {
            g.SV_Rcon(from);
            return;
        }
        if (!from || !SvenRconPolicy::Allowed(*from, g_allowed))
            return;

        // CheckChallenge may overwrite net_message. Preserve both the complete
        // address and request before entering any native validation function.
        netadr_svengine_t address = *from;
        const int length = *reinterpret_cast<int*>(g.message + g.sizeOffset) - 4;
        const char* data = *reinterpret_cast<char**>(g.message + g.dataOffset);
        if (!data || length <= 0 || length > RequestLimit)
            return;
        char request[RequestLimit + 1]{};
        memcpy(request, data + 4, length);
        request[length] = 0;
        const int argc = gEngfuncs.Cmd_Argc();
        const int challenge = static_cast<int>(strtoul(gEngfuncs.Cmd_Argv(1), nullptr, 10));
        const std::string supplied = gEngfuncs.Cmd_Argv(2);
        const std::string password = gEngfuncs.pfnGetCvarString("rcon_password");

        int invalid = 0;
        if (argc < 4 || (password.empty() && !SvenRconPolicy::EmptyPasswordAllowed(g_running, address, g_allowed)))
            invalid = 1;
        else
        {
            if (gEngfuncs.pfnGetCvarFloat("sv_rcon_banpenalty") < 0)
                gEngfuncs.Cvar_SetValue("sv_rcon_banpenalty", 0);
            if (g.SV_CheckRconFailure(&address))
            {
                char ban[128];
                _snprintf_s(ban, sizeof(ban), _TRUNCATE, "addip %i %u.%u.%u.%u\n",
                    static_cast<int>(gEngfuncs.pfnGetCvarFloat("sv_rcon_banpenalty")),
                    address.ip[0], address.ip[1], address.ip[2], address.ip[3]);
                gEngfuncs.pfnClientCmd(ban);
                invalid = 3;
            }
            else if (!g.SV_CheckChallenge(&address, challenge))
                invalid = 2;
            else if (supplied != password)
            {
                g.SV_AddFailedRcon(&address);
                invalid = 1;
            }
        }
        // Preserve a prior redirect across nested command execution.
        const int priorMode = *g.redirected;
        const netadr_svengine_t priorAddress = *g.redirectTo;
        char priorOutput[OutputCapacity];
        memcpy(priorOutput, g.output, sizeof(priorOutput));
        g.SV_BeginRedirect(RedirectPacket, &address);
        if (invalid)
            gEngfuncs.Con_Printf("%s", invalid == 2 ? "Bad challenge.\n" : invalid == 3 ? "Rcon banned.\n" : "Bad rcon_password.\n");
        else
        {
            // Public engine parser, three tokens: rcon, challenge, password.
            char token[RequestLimit + 1];
            const char* command = request;
            for (int i = 0; i < 3 && command; ++i)
                command = gEngfuncs.COM_ParseFile(command, token);
            if (command)
                g.Cmd_ExecuteString(command, CommandSource);
            else
                gEngfuncs.Con_Printf("Empty rcon\n");
        }
        if (!g_running) // quit may already have destroyed engine command/cvar state
            return;
        g.SV_EndRedirect();
        *g.redirected = priorMode;
        *g.redirectTo = priorAddress;
        memcpy(g.output, priorOutput, sizeof(priorOutput));
    }

    template<class T> bool Resolve(const char* name, mh_gamesymbol_kind_t kind, T& target, std::string& error)
    {
        void* address = nullptr;
        const auto status = g_pMetaHookAPI->ResolveGameSymbol(g_pMetaHookAPI->GetEngineBase(), name, kind, &address);
        if (status != MH_GAMESYMBOL_OK || !address)
        {
            error = std::string(name) + " (kind " + std::to_string(kind) + "): " + g_pMetaHookAPI->GetGameSymbolStatusString(status);
            return false;
        }
        target = reinterpret_cast<T>(address);
        return true;
    }
    bool Scalar(const char* name, uint32_t& value, uint32_t expected, std::string& error)
    {
        auto status = g_pMetaHookAPI->QueryGameSymbolScalar(g_pMetaHookAPI->GetEngineBase(), name, &value);
        if (status == MH_GAMESYMBOL_OK && value == expected)
            return true;
        error = std::string(name) + " (scalar): " + g_pMetaHookAPI->GetGameSymbolStatusString(status) + ", unsupported Sven layout";
        return false;
    }
    template<class T, class U> bool Hook(const char* name, T& original, U replacement, std::string& error)
    {
        auto hook = g_pMetaHookAPI->InlineHook(reinterpret_cast<void*>(original), reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
        if (!hook)
        {
            error = std::string(name) + ": inline hook installation failed";
            SvenUdp::Uninstall();
            return false;
        }
        g_hooks.push_back(hook);
        return true;
    }
    bool Endpoint(sockaddr_in& bound)
    {
        if (!g_running || !g.sockets || !g.sockets[ServerSocket] || g.sockets[ServerSocket] == INVALID_SOCKET)
            return false;
        int length = sizeof(bound);
        return getsockname(g.sockets[ServerSocket], reinterpret_cast<sockaddr*>(&bound), &length) == 0 && bound.sin_family == AF_INET;
    }
}

namespace SvenUdp
{
    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        if (g_pInterface->MetaHookAPIVersion < MinMetaHookAPI)
        {
            error = "Sven Native UDP requires MetaHook API 115 or newer";
            return false;
        }
        if (g_dwEngineBuildnum != 8948 && g_dwEngineBuildnum != 10257)
        {
            error = "unsupported Sven build (requires verified 8948/10257 gamedata identity)";
            return false;
        }
#define FN(name) if (!Resolve(#name, MH_GAMESYMBOL_KIND_FUNCTION, g.name, error)) return false
        FN(Cbuf_Execute); FN(NET_Config); FN(NET_IsLocalAddress); FN(SVC_ServiceChallenge);
        FN(SV_Rcon); FN(SV_FlushRedirect); FN(NET_GetPacket); FN(NET_SendPacket);
        FN(SV_FilterPacket); FN(SV_SendBan); FN(SV_HandleRconPacket); FN(SV_CheckChallenge);
        FN(SV_CheckRconFailure); FN(SV_AddFailedRcon); FN(Cmd_ExecuteString);
        FN(SV_BeginRedirect); FN(SV_EndRedirect);
#undef FN
#define GV(name, member) if (!Resolve(name, MH_GAMESYMBOL_KIND_GLOBAL, g.member, error)) return false
        GV("ip_sockets", sockets); GV("net_from", from); GV("net_message", message);
        GV("sv", server); GV("host_initialized", initialized); GV("giActive", active);
        GV("sv_redirected", redirected); GV("sv_redirectto", redirectTo); GV("outputbuf", output);
#undef GV
        if (!Scalar("sv_active_offset", g.serverActiveOffset, 0, error) ||
            !Scalar("sizebuf_t_data_offset", g.dataOffset, 8, error) ||
            !Scalar("sizebuf_t_cursize_offset", g.sizeOffset, 16, error) ||
            !Resolve("_Host_Frame_to_Cbuf_Execute_callsite_0", MH_GAMESYMBOL_KIND_PATCH, g.frameCall, error) ||
            !Resolve("Host_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION, g_hostShutdown, error) ||
            !Resolve("NET_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION, g_netShutdown, error))
            return false;
        // A patch record is identity-bound; also verify its actual call target.
        if (g.frameCall[0] != 0xE8 || g.frameCall + 5 + *reinterpret_cast<int32_t*>(g.frameCall + 1) != reinterpret_cast<unsigned char*>(g.Cbuf_Execute))
        {
            error = "main-frame Cbuf callsite does not target Cbuf_Execute";
            return false;
        }
#define HOOK(name, replacement) if (!Hook(#name, g.name, replacement, error)) return false
        HOOK(Cbuf_Execute, HookCbuf); HOOK(NET_Config, HookConfig);
        HOOK(NET_IsLocalAddress, HookLocal); HOOK(SVC_ServiceChallenge, HookChallenge);
        HOOK(SV_Rcon, HookRcon); HOOK(SV_FlushRedirect, HookFlush);
#undef HOOK
        if (!Hook("Host_Shutdown", g_hostShutdown, HookHostShutdown, error) ||
            !Hook("NET_Shutdown", g_netShutdown, HookNetShutdown, error))
            return false;
        g_installed = true;
        return true;
    }

    RconServer::StartResult Start(const std::string& password, const std::string& allowedIps)
    {
        RconServer::StartResult result;
        if (!g_installed)
        {
            result.error = "Native UDP hooks unavailable";
            return result;
        }
        if (password.find_first_of("\"\r\n") != std::string::npos || password.find('\0') != std::string::npos)
        {
            result.error = "RCON password cannot contain quotes or line breaks";
            return result;
        }
        g_allowed.clear();
        for (const auto& value : text::SplitCsv(allowedIps))
        {
            SvenRconPolicy::IP ip{};
            if (inet_pton(AF_INET, value.c_str(), ip.data()) != 1)
            {
                result.error = "invalid rcon.allowed_ips IPv4 address";
                return result;
            }
            g_allowed.push_back(ip);
        }
        gEngfuncs.Cvar_Set("rcon_password", password.c_str());
        g_running = true;
        g.NET_Config(1); // reuse existing sockets and engine-selected bindings
        result.port = CurrentPort();
        result.ok = result.port != 0;
        if (!result.ok)
        {
            g_running = false;
            result.error = "engine NS_SERVER UDP socket could not be opened";
        }
        return result;
    }
    void Stop() { g_running = false; }
    void Uninstall()
    {
        Stop();
        for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it)
            g_pMetaHookAPI->UnHook(*it);
        g_hooks.clear();
        g_installed = false;
    }
    bool Installed() { return g_installed; }
    bool Running() { return g_running; }
    unsigned short CurrentPort()
    {
        sockaddr_in bound{};
        return Endpoint(bound) ? ntohs(bound.sin_port) : 0;
    }
    std::string CurrentAddress()
    {
        sockaddr_in bound{};
        char address[INET_ADDRSTRLEN]{};
        if (Endpoint(bound))
            inet_ntop(AF_INET, &bound.sin_addr, address, sizeof(address));
        return address;
    }
}
