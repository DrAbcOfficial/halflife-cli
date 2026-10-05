#pragma once
#include "enginedef.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace SvenRconPolicy
{
    constexpr netadrtype_t Loopback = NA_LOOPBACK;
    constexpr netadrtype_t IPv4 = NA_IP;
    using IP = std::array<uint8_t, 4>;

    inline bool Local(const netadr_svengine_t& address)
    {
        return address.type == Loopback ||
            (address.type == IPv4 && address.ip[0] == 127 && address.ip[1] == 0 &&
                address.ip[2] == 0 && address.ip[3] == 1);
    }

    inline bool Allowed(const netadr_svengine_t& address, const std::vector<IP>& allowed)
    {
        if (allowed.empty())
            return true;
        for (const auto& ip : allowed)
            if ((address.type == IPv4 && std::equal(ip.begin(), ip.end(), address.ip)) ||
                (address.type == Loopback && ip == IP{127, 0, 0, 1}))
                return true;
        return false;
    }

    inline bool EmptyPasswordAllowed(bool running, const netadr_svengine_t& address, const std::vector<IP>& allowed)
    {
        return running && Local(address) && Allowed(address, allowed);
    }
}
