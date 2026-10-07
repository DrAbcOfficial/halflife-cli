#pragma once

#include <netadr.h>
#include <cstddef>

// Native UDP RCON passes engine addresses by value and copies whole objects, so
// the adapter is instantiated per address layout:
// - Windows x86 GoldSrc/CoF (every cataloged HL build): SDK netadr_t, 20 bytes.
// - Windows x86 SvEngine 8948/10257: netadr_svengine_t, 36 bytes. Only type and
//   IPv4 offsets are consumed; retain every trailing byte when passing or copying
//   an engine-owned address.
typedef struct netadr_svengine_s
{
    netadrtype_t type;
    unsigned char ip[4];
    unsigned char opaque[28];
} netadr_svengine_t;

static_assert(sizeof(netadrtype_t) == 4);
static_assert(offsetof(netadr_t, ip) == 4);
static_assert(offsetof(netadr_t, port) == 18);
static_assert(sizeof(netadr_t) == 20);
static_assert(offsetof(netadr_svengine_t, ip) == 4);
static_assert(sizeof(netadr_svengine_t) == 36);
