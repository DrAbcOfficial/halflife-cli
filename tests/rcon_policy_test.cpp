#include "rcon/rcon_policy.h"
#include <cstdio>

namespace
{
    using namespace RconPolicy;
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    // The local-address and allowlist rules read only type and IPv4 bytes, so
    // both native layouts must give identical answers.
    template<class Address> void TestAddressPolicy(const char* layout)
    {
        Address local{IPv4, {127, 0, 0, 1}};
        Address alias{IPv4, {127, 0, 0, 2}};
        Address lan{IPv4, {192, 168, 1, 7}};
        Address loopback{Loopback};
        const std::vector<IP> denyLocal{{192, 168, 1, 7}};
        const std::vector<IP> permitLocal{{127, 0, 0, 1}};
        const bool ok = Local(local) && Local(loopback) && !Local(alias) && !Local(lan) &&
            EmptyPasswordAllowed(true, local, {}) &&
            !EmptyPasswordAllowed(false, local, {}) &&
            !EmptyPasswordAllowed(true, alias, {}) &&
            !EmptyPasswordAllowed(true, lan, {}) &&
            !EmptyPasswordAllowed(true, local, denyLocal) &&
            !EmptyPasswordAllowed(true, loopback, denyLocal) &&
            EmptyPasswordAllowed(true, loopback, permitLocal) &&
            Allowed(lan, denyLocal) && !Allowed(alias, permitLocal);
        if (!ok)
            std::fprintf(stderr, "layout: %s\n", layout);
        Check(ok, "local-address / allowlist policy");
    }

    void TestPacketDispatch()
    {
        const unsigned char rcon[] = "\xFF\xFF\xFF\xFFrcon 123 \"pass word\" status\nsecond line";
        Check(PacketLine(rcon, sizeof(rcon) - 1) == "rcon 123 \"pass word\" status", "line stops at newline");
        const unsigned char marker[] = "\xFF\xFF\xFF\xFF" "challenge rcon\xFF" "tail";
        Check(PacketLine(marker, sizeof(marker) - 1) == "challenge rcon", "line stops at 0xFF");
        const unsigned char nul[] = "\xFF\xFF\xFF\xFF" "challenge\0rcon";
        Check(PacketLine(nul, sizeof(nul) - 1) == "challenge", "line stops at NUL");
        Check(PacketLine(rcon, 4).empty() && PacketLine(rcon, 2).empty(), "header-only packet has no line");
        std::vector<unsigned char> longPacket(4 + PacketLineLimit + 10, 'a');
        Check(PacketLine(longPacket.data(), static_cast<int>(longPacket.size())).size() == PacketLineLimit, "line length limit");

        Check(Classify("rcon") == Verb::Rcon && Classify("RCON") == Verb::None, "rcon verb is case-sensitive");
        Check(Classify("challenge") == Verb::Challenge && Classify("Challenge") == Verb::Challenge, "challenge verb ignores case");
        Check(Classify("getchallenge") == Verb::None && Classify("") == Verb::None, "other verbs ignored");
        Check(DispatchLine("cli._rconpacket", " 123 \"pass word\" status") == "cli._rconpacket  123 \"pass word\" status",
            "dispatch line keeps the arguments verbatim");
    }

    void TestFailureLimits()
    {
        const auto limits = NormalizeFailureLimits(5.9f, 2.0f, 0.5f);
        Check(limits.minFailures == 2 && limits.maxFailures == 5 && limits.minFailureTime == 1, "truncate, swap and minimum time");
        const auto clamped = NormalizeFailureLimits(0.0f, 99.0f, 30.0f);
        Check(clamped.minFailures == 1 && clamped.maxFailures == FailureHistory && clamped.minFailureTime == 30, "count bounds");
    }

    Peer IPv4Peer(uint8_t last, uint16_t port = 27005)
    {
        Peer peer;
        peer.type = IPv4;
        peer.ip = {10, 0, 0, last};
        peer.port = port;
        return peer;
    }

    void TestFailureTracker()
    {
        const FailureLimits limits{3, 5, 30};
        FailureTracker tracker;
        const Peer attacker = IPv4Peer(1);
        Check(!tracker.Add(attacker, 100.0, limits) && !tracker.Add(attacker, 101.0, limits), "below the minimum");
        Check(!tracker.Rejected(attacker), "not rejected before the minimum");
        Check(tracker.Add(attacker, 102.0, limits) && tracker.Rejected(attacker), "minimum within the window rejects");
        Check(!tracker.Add(attacker, 103.0, limits) && tracker.Rejected(attacker), "a rejected peer stays rejected");
        Check(!tracker.Rejected(IPv4Peer(1, 27006)) && !tracker.Rejected(IPv4Peer(2)), "other port or address unaffected");
        tracker.Clear();
        Check(!tracker.Rejected(attacker), "clear forgets rejections");

        // Failures spread beyond the window never accumulate.
        FailureTracker slow;
        for (int i = 0; i < 10; ++i)
            Check(!slow.Add(attacker, 100.0 + 31.0 * i, limits), "spread failures stay below the minimum");

        // maxfailures bounds the history: the oldest failure drops out first.
        FailureTracker bounded;
        const FailureLimits three{3, 3, 30};
        bounded.Add(attacker, 0.0, three);
        bounded.Add(attacker, 1.0, three);
        bounded.Add(attacker, 100.0, three);
        Check(!bounded.Add(attacker, 101.0, three), "two recent failures in a full history");
        Check(bounded.Add(attacker, 129.0, three), "three recent failures after dropping the oldest");
        FailureTracker endless;
        const FailureLimits quiet{FailureHistory, FailureHistory, 1};
        for (int i = 0; i < 100; ++i)
            Check(!endless.Add(attacker, 2.0 * i, quiet), "history never exceeds its capacity");

        Peer loopbackA;
        loopbackA.type = Loopback;
        Peer loopbackB = loopbackA;
        loopbackB.port = 1234;
        FailureTracker loopback;
        const FailureLimits single{1, 5, 30};
        Check(loopback.Add(loopbackA, 0.0, single) && loopback.Rejected(loopbackB), "loopback peers compare equal");

        // A full table recycles the stalest peer; a second failure only rejects
        // a peer whose first one is still remembered.
        FailureTracker full;
        const FailureLimits pair{2, FailureHistory, 1000};
        for (int i = 0; i < FailureTracker::Peers; ++i)
            full.Add(IPv4Peer(static_cast<uint8_t>(i)), 10.0 + i, quiet);
        full.Add(IPv4Peer(200), 100.0, quiet);
        Check(full.Add(IPv4Peer(1), 101.0, pair), "younger peer survives recycling");
        Check(!full.Add(IPv4Peer(0), 102.0, pair), "stalest peer was recycled");
    }
}

int main()
{
    TestAddressPolicy<netadr_t>("netadr_t");
    TestAddressPolicy<netadr_svengine_t>("netadr_svengine_t");
    TestPacketDispatch();
    TestFailureLimits();
    TestFailureTracker();
    if (g_failures)
        std::fprintf(stderr, "%d Native UDP RCON policy check(s) failed\n", g_failures);
    return g_failures ? 1 : 0;
}
