#pragma once

#include <string>

#include "usermsg/usermsg_schema.h"

// Payload decoder: renders one UserMsg message buffer as the printable
// "[usermsg] Name size=N field=value ..." line, purely a function of the
// schema, the message definition and the raw bytes — no engine or monitor
// state, so it can be exercised without a running game.
namespace UserMsgDecoder
{
	// maxString bounds decoded string fields (longer ones truncate to
	// maxString characters plus "..."). Use the monitor's configured value.
	std::string Format(const UserMsgSchema& schema, const UserMsgDef& def,
		int size, const void* pbuf, size_t maxString);
}
