#include "usermsg/hook_policy.h"
#include <cstdio>

namespace
{
	using Callback = int(*)();
	Callback original = nullptr, pluginOriginal = nullptr;
	int clientCalls = 0, pluginCalls = 0, monitorCalls = 0;
	int Client() { ++clientCalls; return 42; }
	int Monitor()
	{
		if (++monitorCalls > 3) return -1; // Bound the old callback cycle in this test.
		return original ? original() : 1;
	}
	int Plugin() { ++pluginCalls; return pluginOriginal(); }
	int OtherMessage() { return 7; }
}

int main()
{
	Callback top = Client;
	original = top;
	top = Monitor;
	pluginOriginal = top;
	top = Plugin;
	// A later plugin wraps the monitor. Periodic repair must keep this chain.
	for (int frame = 0; frame != 4; ++frame)
		if (UserMsgHooks::ShouldInstall(true, top, &Monitor, original, false))
		{
			original = top;
			top = Monitor;
		}
	if (42 != top() || 1 != clientCalls || 1 != monitorCalls || 1 != pluginCalls)
	{
		std::puts("FAIL: reinstallation created a cycle or bypassed a callback");
		return 1;
	}
	// A real client registration can replace the entry without retaining us.
	if (!UserMsgHooks::ShouldInstall(true, &Client, &Monitor, original, true) ||
		!UserMsgHooks::ShouldInstall(true, &OtherMessage, &Monitor, original, true) ||
		!UserMsgHooks::ShouldInstall(false, &Plugin, &Monitor, Callback{}, false) ||
		UserMsgHooks::ShouldInstall(true, &Plugin, &Monitor, Callback{}, false))
		return 1;
	return 0;
}
