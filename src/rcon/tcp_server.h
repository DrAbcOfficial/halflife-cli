#pragma once
#include "rcon/rcon_server.h"

// Original Source TCP backend, used only by non-Sven engines.
namespace TcpRcon
{
    using StartResult = RconServer::StartResult;
    StartResult Start(const std::string& bindAddr, unsigned short port,
        const std::string& password, const std::string& allowedIps);
    void Shutdown();
    bool Running();
    unsigned short CurrentPort();
}
