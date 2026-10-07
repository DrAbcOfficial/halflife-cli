#pragma once

namespace UserMsgHooks
{
	template<class Callback>
	bool ShouldInstall(bool known, Callback current, Callback dispatcher,
		Callback original, bool clientCallback)
	{
		if (current == dispatcher)
			return !known;
		// An unknown plugin callback may already forward to our dispatcher.
		// Wrapping it again would replace our saved original with that plugin
		// and create a cycle. Client registrations do not retain our callback.
		return !known || !current || current == original || clientCallback;
	}
}
