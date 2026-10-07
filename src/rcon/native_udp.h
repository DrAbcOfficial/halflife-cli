#pragma once
#include "rcon/rcon_server.h"

// Native GoldSrc UDP RCON on the engine's own NS_SERVER socket. SvEngine
// 8948/10257 and the cataloged GoldSrc/CoF builds share one adapter,
// instantiated for their 36- or 20-byte address layout.
namespace NativeUdp
{
    // Whether this engine uses the native adapter. SvEngine always does (an
    // unsupported build then fails Install with a reason); a GoldSrc build does
    // when its build was verified and the catalog covers this hw.dll, otherwise
    // reason explains why it keeps the Source TCP backend.
    bool Selected(std::string& reason);
    bool Install(std::string& error);
    void RegisterCommands(); // HUD_Init: the engine command table is ready
    bool Installed();
    RconServer::StartResult Start(const std::string& password, const std::string& allowedIps);
    void Stop(); // clear keepalive; never closes engine sockets
    void Uninstall();
    bool Running();
    unsigned short CurrentPort();
    std::string CurrentAddress();
}
