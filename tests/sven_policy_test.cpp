#include "rcon/sven_policy.h"
#include <cstdio>

int main()
{
    using namespace SvenRconPolicy;
    const netadr_svengine_t local{IPv4, {127, 0, 0, 1}, {}};
    const netadr_svengine_t alias{IPv4, {127, 0, 0, 2}, {}};
    const netadr_svengine_t lan{IPv4, {192, 168, 1, 7}, {}};
    const netadr_svengine_t loopback{Loopback, {}, {}};
    const std::vector<IP> denyLocal{{192, 168, 1, 7}};
    const std::vector<IP> permitLocal{{127, 0, 0, 1}};
    if (!Local(local) || !Local(loopback) || Local(alias) || Local(lan) ||
        !EmptyPasswordAllowed(true, local, {}) ||
        EmptyPasswordAllowed(false, local, {}) ||
        EmptyPasswordAllowed(true, alias, {}) ||
        EmptyPasswordAllowed(true, lan, {}) ||
        EmptyPasswordAllowed(true, local, denyLocal) ||
        EmptyPasswordAllowed(true, loopback, denyLocal) ||
        !EmptyPasswordAllowed(true, loopback, permitLocal) ||
        !Allowed(lan, denyLocal) || Allowed(alias, permitLocal))
    {
        std::fprintf(stderr, "Sven local-address / allowlist policy regression\n");
        return 1;
    }
    return 0;
}
