#pragma once

#include <netadr.h>
#include <cstddef>

// Windows x86 SvEngine 8948/10257. Only type and IPv4 offsets are consumed;
// retain every trailing byte when passing or copying an engine-owned address.
typedef struct netadr_svengine_s
{
    netadrtype_t type;
    unsigned char ip[4];
    unsigned char opaque[28];
} netadr_svengine_t;

static_assert(sizeof(netadrtype_t) == 4);
static_assert(offsetof(netadr_svengine_t, ip) == 4);
static_assert(sizeof(netadr_svengine_t) == 36);
