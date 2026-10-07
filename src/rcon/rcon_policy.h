#pragma once
#include "enginedef.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Engine-independent Native UDP RCON policy. Address templates accept every
// native layout in enginedef.h; only type and IPv4 bytes are read.
namespace RconPolicy
{
    constexpr netadrtype_t Loopback = NA_LOOPBACK;
    constexpr netadrtype_t IPv4 = NA_IP;
    using IP = std::array<uint8_t, 4>;

    template<class Address> bool Local(const Address& address)
    {
        return address.type == Loopback ||
            (address.type == IPv4 && address.ip[0] == 127 && address.ip[1] == 0 &&
                address.ip[2] == 0 && address.ip[3] == 1);
    }

    template<class Address> bool Allowed(const Address& address, const std::vector<IP>& allowed)
    {
        if (allowed.empty())
            return true;
        for (const auto& ip : allowed)
            if ((address.type == IPv4 && std::equal(ip.begin(), ip.end(), address.ip)) ||
                (address.type == Loopback && ip == IP{127, 0, 0, 1}))
                return true;
        return false;
    }

    template<class Address> bool EmptyPasswordAllowed(bool running, const Address& address, const std::vector<IP>& allowed)
    {
        return running && Local(address) && Allowed(address, allowed);
    }

    // Menu packet dispatch for engines whose SV_HandleRconPacket exists only
    // inline (HL 10210 Windows). The native parser reads one line after the
    // 0xFFFFFFFF header (MSG_ReadStringLine stops at NUL, newline or a 0xFF
    // byte, at most 2047 bytes), then serves "challenge" (any case) and "rcon"
    // (exact case). Client connection challenges are irrelevant without an
    // active server.
    constexpr size_t PacketLineLimit = 2047;
    enum class Verb { None, Challenge, Rcon };

    inline std::string PacketLine(const unsigned char* data, int length)
    {
        std::string line;
        for (int i = 4; i < length && line.size() < PacketLineLimit; ++i)
        {
            if (data[i] == 0 || data[i] == '\n' || data[i] == 0xFF)
                break;
            line += static_cast<char>(data[i]);
        }
        return line;
    }

    inline Verb Classify(const char* token)
    {
        if (!strcmp(token, "rcon"))
            return Verb::Rcon;
        if (!_stricmp(token, "challenge"))
            return Verb::Challenge;
        return Verb::None;
    }

    // The engine tokenizes "<dispatcher> <arguments>": argv[1..] and argc equal
    // the native packet's, only the verb in argv[0] is replaced.
    inline std::string DispatchLine(const char* dispatcher, const char* arguments)
    {
        return std::string(dispatcher) + " " + arguments;
    }

    // Failure accounting for engines whose SV_CheckRconFailure exists only
    // inline (HL 10210 Windows). Mirrors the native table: 32 peers with 20
    // failure times each; sv_rcon_minfailures failures within
    // sv_rcon_minfailuretime seconds mark a peer for rejection, and
    // sv_rcon_maxfailures bounds the remembered history.
    struct Peer
    {
        int type = 0;
        IP ip{};
        uint16_t port = 0;

        // NET_CompareAdr: loopback peers are equal, others match address and port.
        bool operator==(const Peer& other) const
        {
            if (type != other.type)
                return false;
            return type == Loopback || (ip == other.ip && port == other.port);
        }
    };

    inline Peer PeerOf(const netadr_t& address)
    {
        Peer peer;
        peer.type = address.type;
        std::copy(address.ip, address.ip + 4, peer.ip.begin());
        peer.port = address.port;
        return peer;
    }

    struct FailureLimits
    {
        int minFailures = 0;
        int maxFailures = 0;
        int minFailureTime = 0;
    };

    // The native clamp applied before each failure is recorded: truncate,
    // keep both counts in [1, 20], swap them when inverted, at least 1 second.
    constexpr int FailureHistory = 20;
    inline FailureLimits NormalizeFailureLimits(float minFailures, float maxFailures, float minFailureTime)
    {
        FailureLimits limits;
        limits.minFailures = std::clamp(static_cast<int>(minFailures), 1, FailureHistory);
        limits.maxFailures = std::clamp(static_cast<int>(maxFailures), 1, FailureHistory);
        if (limits.minFailures > limits.maxFailures)
            std::swap(limits.minFailures, limits.maxFailures);
        limits.minFailureTime = (std::max)(static_cast<int>(minFailureTime), 1);
        return limits;
    }

    class FailureTracker
    {
    public:
        static constexpr int Peers = 32;

        bool Rejected(const Peer& peer) const
        {
            for (const auto& entry : m_entries)
                if (entry.active && entry.peer == peer && entry.reject)
                    return true;
            return false;
        }

        // Records one failure at time now (seconds). Returns true when this
        // failure marks the peer for rejection.
        bool Add(const Peer& peer, double now, const FailureLimits& limits)
        {
            Entry* entry = nullptr;
            bool found = false;
            int oldest = 0;
            double oldestAge = -99999.0;
            for (int i = 0; i < Peers && !entry; ++i)
            {
                Entry& candidate = m_entries[i];
                if (!candidate.active)
                    entry = &candidate;
                else if (candidate.peer == peer)
                {
                    entry = &candidate;
                    found = true;
                }
                else if (now - candidate.updated > oldestAge)
                {
                    oldestAge = now - candidate.updated;
                    oldest = i;
                }
            }
            if (!entry)
                entry = &m_entries[oldest];
            if (found && entry->reject)
                return false;

            entry->active = true;
            entry->reject = false;
            entry->updated = now;
            entry->peer = peer;
            if (!found)
                entry->count = 0;
            if (entry->count >= limits.maxFailures)
            {
                for (int i = 1; i < limits.maxFailures; ++i)
                    entry->times[i - 1] = entry->times[i];
                --entry->count;
            }
            entry->times[entry->count++] = now;
            int recent = 0;
            for (int i = 0; i < entry->count; ++i)
                if (now - entry->times[i] < limits.minFailureTime)
                    ++recent;
            entry->reject = recent >= limits.minFailures;
            return entry->reject;
        }

        void Clear() { m_entries = {}; }

    private:
        struct Entry
        {
            bool active = false;
            bool reject = false;
            Peer peer;
            int count = 0;
            double updated = 0;
            double times[FailureHistory]{};
        };
        std::array<Entry, Peers> m_entries{};
    };
}
