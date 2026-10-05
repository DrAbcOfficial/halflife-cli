#pragma once
#include "rcon/rcon_server.h"

namespace SvenUdp
{
    bool Install(std::string& error);
    bool Installed();
    RconServer::StartResult Start(const std::string& password, const std::string& allowedIps);
    void Stop(); // clear keepalive; never closes engine sockets
    void Uninstall();
    bool Running();
    unsigned short CurrentPort();
    std::string CurrentAddress();
}
