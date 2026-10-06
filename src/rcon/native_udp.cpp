#include <winsock2.h>
#include <ws2tcpip.h>
#include "rcon/native_udp.h"
#include "rcon/rcon_policy.h"
#include "core/plugins.h"
#include "console/console_bridge.h"
#include "util/text.h"
#include <intrin.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <type_traits>

// Teardown thunks in shutdown_thunks.asm save the full entry state, call
// NativeUdpBeforeShutdown, restore and jump to the saved original entry.
extern "C"
{
    void* g_nativeUdpHostShutdown = nullptr;
    void* g_nativeUdpNetShutdown = nullptr;
    void NativeUdpHookHostShutdown();
    void NativeUdpHookNetShutdown();

    void __cdecl NativeUdpBeforeShutdown()
    {
        RconServer::OnEngineShutdown();
        ConsoleBridge::Shutdown();
    }
}

namespace
{
    constexpr int ServerSocket = 1;
    constexpr int Closed = 3;
    constexpr int RedirectNone = 0;
    constexpr int RedirectPacket = 2;
    constexpr int CommandSource = 1;
    constexpr size_t OutputCapacity = 1400;
    constexpr size_t TextChunk = 1200;
    constexpr int RequestLimit = 510;
    constexpr size_t TokenCapacity = 1024; // engine com_token
    constexpr int MinMetaHookAPI = 115;
    constexpr char PacketDispatcher[] = "cli._rconpacket";
    static_assert(sizeof(SOCKET) == 4 && sizeof(int) == 4);

    // Builds whose helper ABI (x86 cdecl) and address layout were verified for
    // every catalog record (GoldSrc_VibeSignatures #326). The catalog is the
    // identity gate; this list keeps an unverified build from reaching it.
    constexpr DWORD SvenBuilds[] = { 8948, 10257 };
    constexpr DWORD GoldSrcBuilds[] = { 3248, 3266, 3329, 3647, 4554, 5936, 6153, 8684, 10210 };

    template<size_t N> bool Contains(const DWORD (&builds)[N], DWORD build)
    {
        return std::find(std::begin(builds), std::end(builds), build) != std::end(builds);
    }
    bool IsSven() { return g_iEngineType == ENGINE_SVENGINE; }

    // Address-independent private functions; all are x86 cdecl.
    struct Engine
    {
        void (__cdecl *Cbuf_Execute)();
        void (__cdecl *NET_Config)(int);
        void (__cdecl *SVC_ServiceChallenge)();
        void (__cdecl *SV_FlushRedirect)();
        int (__cdecl *NET_GetPacket)(int);
        int (__cdecl *SV_FilterPacket)();
        void (__cdecl *SV_SendBan)();
        void (__cdecl *SV_HandleRconPacket)(); // native or the dispatcher fallback
        void (__cdecl *Cmd_ExecuteString)(const char*, int);
        void (__cdecl *SV_EndRedirect)();      // native or the fallback
        SOCKET* sockets;
        unsigned char* message;
        unsigned char* server;
        int* initialized;
        int* active;
        int* redirected;
        char* output;
        uint32_t serverActiveOffset, dataOffset, sizeOffset;
        unsigned char* frameCall;
    } g{};
    std::vector<hook_t*> g_hooks;
    std::vector<RconPolicy::IP> g_allowed;
    RconPolicy::FailureTracker g_failures;
    xcommand_t g_resetRcon = nullptr;
    void (*g_dispatcher)() = nullptr; // registered when SV_HandleRconPacket is inline-only
    RconPolicy::Verb g_dispatch = RconPolicy::Verb::None;
    bool g_running = false;
    bool g_installed = false;
    bool g_frame = false;

    int MessageSize() { return *reinterpret_cast<int*>(g.message + g.sizeOffset); }
    unsigned char* MessageData() { return *reinterpret_cast<unsigned char**>(g.message + g.dataOffset); }
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
            const int length = MessageSize();
            auto data = MessageData();
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

    void ResetRcon()
    {
        g_failures.Clear();
        if (g_resetRcon)
            g_resetRcon();
    }

    double Now()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Typed entries and hooks for one native address layout.
    template<class Address> struct Native
    {
        static inline int (__cdecl *NET_IsLocalAddress)(Address) = nullptr;
        static inline void (__cdecl *SV_Rcon)(Address*) = nullptr;
        static inline void (__cdecl *NET_SendPacket)(int, int, const void*, Address) = nullptr;
        static inline int (__cdecl *SV_CheckChallenge)(Address*, int) = nullptr;
        static inline int (__cdecl *SV_CheckRconFailure)(Address*) = nullptr; // native or the tracker
        static inline void (__cdecl *SV_AddFailedRcon)(Address*) = nullptr;   // paired with the check
        static inline void (__cdecl *SV_BeginRedirect)(int, Address*) = nullptr;
        static inline Address* from = nullptr;
        static inline Address* redirectTo = nullptr;

        static int __cdecl HookLocal(Address address)
        {
            // No change to native semantics when disabled or initialization failed.
            return g_running ? RconPolicy::Local(address) : NET_IsLocalAddress(address);
        }

        static void __cdecl HookChallenge()
        {
            if (g_running && gEngfuncs.Cmd_Argc() == 2 &&
                !_stricmp(gEngfuncs.Cmd_Argv(1), "rcon") && !RconPolicy::Allowed(*from, g_allowed))
                return;
            g.SVC_ServiceChallenge();
        }

        static void __cdecl HookFlush()
        {
            if (!g_running || *g.redirected != RedirectPacket)
            {
                g.SV_FlushRedirect();
                return;
            }
            const Address destination = *redirectTo;
            const int mode = *g.redirected;
            *g.redirected = RedirectNone; // NET_SendPacket diagnostics must not recurse
            size_t remaining = strnlen_s(g.output, OutputCapacity - 1);
            const char* text = g.output;
            do
            {
                unsigned char packet[TextChunk + 7] = {255, 255, 255, 255, 'l'};
                size_t count = (std::min)(remaining, TextChunk);
                memcpy(packet + 5, text, count); // two zero terminators, including empty output
                NET_SendPacket(ServerSocket, static_cast<int>(count + 7), packet, destination);
                text += count;
                remaining -= count;
            } while (remaining);
            g.output[0] = 0;
            *g.redirected = mode;
        }

        static void __cdecl HookRcon(Address* source)
        {
            if (!g_running)
            {
                SV_Rcon(source);
                return;
            }
            if (!source || !RconPolicy::Allowed(*source, g_allowed))
                return;

            // CheckChallenge may overwrite net_message. Preserve both the complete
            // address and request before entering any native validation function.
            Address address = *source;
            const int length = MessageSize() - 4;
            const char* data = reinterpret_cast<const char*>(MessageData());
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
            if (argc < 4 || (password.empty() && !RconPolicy::EmptyPasswordAllowed(g_running, address, g_allowed)))
                invalid = 1;
            else
            {
                if (gEngfuncs.pfnGetCvarFloat("sv_rcon_banpenalty") < 0)
                    gEngfuncs.Cvar_SetValue("sv_rcon_banpenalty", 0);
                if (SV_CheckRconFailure(&address))
                {
                    char ban[128];
                    _snprintf_s(ban, sizeof(ban), _TRUNCATE, "addip %i %u.%u.%u.%u\n",
                        static_cast<int>(gEngfuncs.pfnGetCvarFloat("sv_rcon_banpenalty")),
                        address.ip[0], address.ip[1], address.ip[2], address.ip[3]);
                    gEngfuncs.pfnClientCmd(ban);
                    invalid = 3;
                }
                else if (!SV_CheckChallenge(&address, challenge))
                    invalid = 2;
                else if (supplied != password)
                {
                    SV_AddFailedRcon(&address);
                    invalid = 1;
                }
            }
            // Preserve a prior redirect across nested command execution.
            const int priorMode = *g.redirected;
            const Address priorAddress = *redirectTo;
            char priorOutput[OutputCapacity];
            memcpy(priorOutput, g.output, sizeof(priorOutput));
            SV_BeginRedirect(RedirectPacket, &address);
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
            *redirectTo = priorAddress;
            memcpy(g.output, priorOutput, sizeof(priorOutput));
        }

        // Fallbacks for helpers that exist only inline (HL 10210 Windows), built
        // from the published redirect state and the verified entry points.
        static void __cdecl BeginRedirect(int mode, Address* to)
        {
            *g.redirected = mode;
            *redirectTo = *to;
            g.output[0] = 0;
        }

        static void __cdecl EndRedirect()
        {
            HookFlush(); // the native End reaches Flush through its hooked entry
            *g.redirected = RedirectNone;
        }

        static int __cdecl CheckRconFailure(Address* address)
        {
            return g_failures.Rejected(RconPolicy::PeerOf(*address));
        }

        static void __cdecl AddFailedRcon(Address* address)
        {
            const auto limits = RconPolicy::NormalizeFailureLimits(gEngfuncs.pfnGetCvarFloat("sv_rcon_minfailures"),
                gEngfuncs.pfnGetCvarFloat("sv_rcon_maxfailures"), gEngfuncs.pfnGetCvarFloat("sv_rcon_minfailuretime"));
            if (g_failures.Add(RconPolicy::PeerOf(*address), Now(), limits))
                gEngfuncs.Con_Printf("User %u.%u.%u.%u:%u will be banned for rcon hacking\n",
                    address->ip[0], address->ip[1], address->ip[2], address->ip[3], ntohs(address->port));
        }

        // The native menu parser tokenizes the packet line and calls the hooked
        // challenge/rcon entries. Without it, the engine's own Cmd_ExecuteString
        // tokenizes the line with the verb replaced by the dispatcher command, so
        // Cmd_Argc/Cmd_Argv see exactly the native arguments.
        static void __cdecl HandleRconPacket()
        {
            // The line (password included) must only reach our own command: an
            // unknown command would be forwarded to a connected remote server.
            const cmd_function_t* dispatcher = g_pMetaHookAPI->FindCmd(PacketDispatcher);
            if (!dispatcher || dispatcher->function != g_dispatcher)
                return;
            std::string line = RconPolicy::PacketLine(MessageData(), MessageSize());
            char token[TokenCapacity];
            const char* arguments = gEngfuncs.COM_ParseFile(line.data(), token);
            const auto verb = arguments ? RconPolicy::Classify(token) : RconPolicy::Verb::None;
            if (verb == RconPolicy::Verb::None)
                return;
            std::string command = RconPolicy::DispatchLine(PacketDispatcher, arguments);
            g_dispatch = verb;
            g.Cmd_ExecuteString(command.c_str(), CommandSource);
            g_dispatch = RconPolicy::Verb::None;
        }

        // Console invocations (and an rcon command naming it) find no pending packet.
        static void Dispatch()
        {
            const auto verb = g_dispatch;
            g_dispatch = RconPolicy::Verb::None;
            if (verb == RconPolicy::Verb::Challenge)
                HookChallenge();
            else if (verb == RconPolicy::Verb::Rcon)
                HookRcon(from);
        }
    };

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
    // A helper the catalog omits for this hw.dll (exempted as inline-only) takes
    // its fallback; any other lookup failure is still an error.
    template<class T, class U> bool Optional(const char* name, T& target, U fallback, std::string& error)
    {
        const auto status = g_pMetaHookAPI->IsGameSymbolAvailable(g_pMetaHookAPI->GetEngineBase(), name);
        if (status == MH_GAMESYMBOL_SYMBOL_NOT_FOUND)
        {
            target = fallback;
            return true;
        }
        return Resolve(name, MH_GAMESYMBOL_KIND_FUNCTION, target, error);
    }
    bool Scalar(const char* name, uint32_t& value, uint32_t expected, std::string& error)
    {
        auto status = g_pMetaHookAPI->QueryGameSymbolScalar(g_pMetaHookAPI->GetEngineBase(), name, &value);
        if (status == MH_GAMESYMBOL_OK && value == expected)
            return true;
        error = std::string(name) + " (scalar): " + g_pMetaHookAPI->GetGameSymbolStatusString(status) + ", unsupported layout";
        return false;
    }
    template<class T, class U> bool Hook(const char* name, T& original, U replacement, std::string& error)
    {
        auto hook = g_pMetaHookAPI->InlineHook(reinterpret_cast<void*>(original), reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
        if (!hook)
        {
            error = std::string(name) + ": inline hook installation failed";
            NativeUdp::Uninstall();
            return false;
        }
        g_hooks.push_back(hook);
        return true;
    }

    template<class Address> bool InstallFor(std::string& error)
    {
        using N = Native<Address>;
#define FN(name, target) if (!Resolve(#name, MH_GAMESYMBOL_KIND_FUNCTION, target, error)) return false
        FN(Cbuf_Execute, g.Cbuf_Execute); FN(NET_Config, g.NET_Config); FN(SVC_ServiceChallenge, g.SVC_ServiceChallenge);
        FN(SV_FlushRedirect, g.SV_FlushRedirect); FN(NET_GetPacket, g.NET_GetPacket); FN(SV_FilterPacket, g.SV_FilterPacket);
        FN(SV_SendBan, g.SV_SendBan); FN(Cmd_ExecuteString, g.Cmd_ExecuteString);
        FN(NET_IsLocalAddress, N::NET_IsLocalAddress); FN(SV_Rcon, N::SV_Rcon); FN(NET_SendPacket, N::NET_SendPacket);
        FN(SV_CheckChallenge, N::SV_CheckChallenge);
#undef FN
        // HL 10210 Windows keeps these four helpers only inline. SvEngine always
        // publishes them; its address layout has no fallback.
        if constexpr (std::is_same_v<Address, netadr_t>)
        {
            if (!Optional("SV_HandleRconPacket", g.SV_HandleRconPacket, &N::HandleRconPacket, error) ||
                !Optional("SV_BeginRedirect", N::SV_BeginRedirect, &N::BeginRedirect, error) ||
                !Optional("SV_EndRedirect", g.SV_EndRedirect, &N::EndRedirect, error) ||
                !Optional("SV_CheckRconFailure", N::SV_CheckRconFailure, &N::CheckRconFailure, error))
                return false;
            // The check and the record share one failure table: native or the
            // tracker, which resetrcon then clears along with the native table.
            if (N::SV_CheckRconFailure != &N::CheckRconFailure)
            {
                if (!Resolve("SV_AddFailedRcon", MH_GAMESYMBOL_KIND_FUNCTION, N::SV_AddFailedRcon, error))
                    return false;
            }
            else
            {
                N::SV_AddFailedRcon = &N::AddFailedRcon;
                g_resetRcon = g_pMetaHookAPI->HookCmd("resetrcon", ResetRcon);
            }
            g_dispatcher = g.SV_HandleRconPacket == &N::HandleRconPacket ? &N::Dispatch : nullptr;
        }
        else
        {
#define FN(name, target) if (!Resolve(#name, MH_GAMESYMBOL_KIND_FUNCTION, target, error)) return false
            FN(SV_HandleRconPacket, g.SV_HandleRconPacket); FN(SV_BeginRedirect, N::SV_BeginRedirect);
            FN(SV_EndRedirect, g.SV_EndRedirect); FN(SV_CheckRconFailure, N::SV_CheckRconFailure);
            FN(SV_AddFailedRcon, N::SV_AddFailedRcon);
#undef FN
        }
#define GV(name, target) if (!Resolve(name, MH_GAMESYMBOL_KIND_GLOBAL, target, error)) return false
        GV("ip_sockets", g.sockets); GV("net_from", N::from); GV("net_message", g.message);
        GV("sv", g.server); GV("host_initialized", g.initialized); GV("giActive", g.active);
        GV("sv_redirected", g.redirected); GV("sv_redirectto", N::redirectTo); GV("outputbuf", g.output);
#undef GV
        if (!Scalar("sv_active_offset", g.serverActiveOffset, 0, error) ||
            !Scalar("sizebuf_t_data_offset", g.dataOffset, 8, error) ||
            !Scalar("sizebuf_t_cursize_offset", g.sizeOffset, 16, error) ||
            !Resolve("_Host_Frame_to_Cbuf_Execute_callsite_0", MH_GAMESYMBOL_KIND_PATCH, g.frameCall, error) ||
            !Resolve("Host_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION, g_nativeUdpHostShutdown, error) ||
            !Resolve("NET_Shutdown", MH_GAMESYMBOL_KIND_FUNCTION, g_nativeUdpNetShutdown, error))
            return false;
        // A patch record is identity-bound; also verify its actual call target.
        if (g.frameCall[0] != 0xE8 || g.frameCall + 5 + *reinterpret_cast<int32_t*>(g.frameCall + 1) != reinterpret_cast<unsigned char*>(g.Cbuf_Execute))
        {
            error = "main-frame Cbuf callsite does not target Cbuf_Execute";
            return false;
        }
#define HOOK(name, target, replacement) if (!Hook(#name, target, replacement, error)) return false
        HOOK(Cbuf_Execute, g.Cbuf_Execute, HookCbuf); HOOK(NET_Config, g.NET_Config, HookConfig);
        HOOK(NET_IsLocalAddress, N::NET_IsLocalAddress, &N::HookLocal);
        HOOK(SVC_ServiceChallenge, g.SVC_ServiceChallenge, &N::HookChallenge);
        HOOK(SV_Rcon, N::SV_Rcon, &N::HookRcon); HOOK(SV_FlushRedirect, g.SV_FlushRedirect, &N::HookFlush);
#undef HOOK
        return Hook("Host_Shutdown", g_nativeUdpHostShutdown, NativeUdpHookHostShutdown, error) &&
            Hook("NET_Shutdown", g_nativeUdpNetShutdown, NativeUdpHookNetShutdown, error);
    }

    bool Endpoint(sockaddr_in& bound)
    {
        if (!g_running || !g.sockets || !g.sockets[ServerSocket] || g.sockets[ServerSocket] == INVALID_SOCKET)
            return false;
        int length = sizeof(bound);
        return getsockname(g.sockets[ServerSocket], reinterpret_cast<sockaddr*>(&bound), &length) == 0 && bound.sin_family == AF_INET;
    }
}

namespace NativeUdp
{
    bool Selected(std::string& reason)
    {
        if (IsSven())
            return true;
        if (!Contains(GoldSrcBuilds, g_dwEngineBuildnum))
        {
            reason = "engine build " + std::to_string(g_dwEngineBuildnum) + " has no verified Native UDP adapter";
            return false;
        }
        if (g_pInterface->MetaHookAPIVersion < MinMetaHookAPI)
        {
            reason = "Native UDP requires MetaHook API 115 or newer";
            return false;
        }
        const auto status = g_pMetaHookAPI->IsGameSymbolAvailable(g_pMetaHookAPI->GetEngineBase(), "SV_Rcon");
        if (status != MH_GAMESYMBOL_OK)
        {
            reason = std::string("no Native UDP gamedata for this hw.dll (") + g_pMetaHookAPI->GetGameSymbolStatusString(status) + ")";
            return false;
        }
        return true;
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        if (g_pInterface->MetaHookAPIVersion < MinMetaHookAPI)
        {
            error = "Native UDP requires MetaHook API 115 or newer";
            return false;
        }
        if (IsSven() ? !Contains(SvenBuilds, g_dwEngineBuildnum) : !Contains(GoldSrcBuilds, g_dwEngineBuildnum))
        {
            error = "unsupported engine build " + std::to_string(g_dwEngineBuildnum) + " (no verified Native UDP adapter)";
            return false;
        }
        if (!(IsSven() ? InstallFor<netadr_svengine_t>(error) : InstallFor<netadr_t>(error)))
            return false;
        g_installed = true;
        return true;
    }

    void RegisterCommands()
    {
        if (g_installed && g_dispatcher)
            gEngfuncs.pfnAddCommand(PacketDispatcher, g_dispatcher);
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
            RconPolicy::IP ip{};
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
        g_dispatcher = nullptr;
        g_failures.Clear();
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
