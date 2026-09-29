#include "rcon/rcon_server.h"
#include "console/console_bridge.h"
#include "util/text.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <cstring>

namespace
{
	constexpr int32_t PACKET_SIZE_MIN = 10;            // id + type + two NULs
	constexpr int32_t PACKET_SIZE_MAX = 10 + 4096;     // id + type + body(4096) + NULs
	constexpr int SERVERDATA_AUTH = 3;
	constexpr int SERVERDATA_AUTH_RESPONSE = 2;
	constexpr int SERVERDATA_EXECCOMMAND = 2;
	constexpr int SERVERDATA_RESPONSE_VALUE = 0;
	constexpr int MAX_ACTIVE_CONNECTIONS = 4;
	constexpr int RESPONSE_TIMEOUT_MS = 5000;

	std::atomic<bool> g_running{ false };
	SOCKET g_listener = INVALID_SOCKET;
	std::thread g_listenerThread;
	std::atomic<int> g_activeConns{ 0 };
	std::atomic<unsigned short> g_port{ 0 };
	std::string g_password;
	std::vector<std::string> g_allowedIps;

	bool RecvExact(SOCKET sock, char *buf, int len)
	{
		int got = 0;
		while (got < len)
		{
			int n = recv(sock, buf + got, len - got, 0);
			if (n <= 0)
				return false;
			got += n;
		}
		return true;
	}

	bool SendAll(SOCKET sock, const char *buf, int len)
	{
		int sent = 0;
		while (sent < len)
		{
			int n = send(sock, buf + sent, len - sent, 0);
			if (n <= 0)
				return false;
			sent += n;
		}
		return true;
	}

	bool SendPacket(SOCKET sock, int32_t id, int32_t type, const std::string& body)
	{
		int32_t size = (int32_t)(10 + body.size());
		std::string pkt;
		pkt.resize(4 + size);
		memcpy(&pkt[0], &size, 4);
		memcpy(&pkt[4], &id, 4);
		memcpy(&pkt[8], &type, 4);
		if (!body.empty())
			memcpy(&pkt[12], body.data(), body.size());
		// trailing NUL (body terminator) + empty string terminator
		pkt[12 + body.size()] = '\0';
		pkt[13 + body.size()] = '\0';
		return SendAll(sock, pkt.data(), (int)pkt.size());
	}

	// Returns false when the connection should close.
	bool HandlePacket(SOCKET sock)
	{
		int32_t size = 0;
		if (!RecvExact(sock, (char *)&size, 4))
			return false;
		if (size < PACKET_SIZE_MIN || size > PACKET_SIZE_MAX)
			return false;

		std::string payload;
		payload.resize(size);
		if (!RecvExact(sock, &payload[0], size))
			return false;

		int32_t id = 0, type = 0;
		memcpy(&id, &payload[0], 4);
		memcpy(&type, &payload[4], 4);
		std::string body = payload.substr(8);
		// body is NUL-terminated on the wire
		size_t z = body.find('\0');
		if (z != std::string::npos)
			body.resize(z);

		static thread_local bool authenticated = false;

		if (type == SERVERDATA_AUTH)
		{
			// Empty configured password keeps automation friction low on a
			// localhost-only bind; set "password" in halflifecli.toml to require it.
			bool ok = g_password.empty() ? true : (body == g_password);
			if (ok)
			{
				authenticated = true;
				return SendPacket(sock, id, SERVERDATA_AUTH_RESPONSE, "");
			}
			SendPacket(sock, -1, SERVERDATA_AUTH_RESPONSE, "");
			return false; // one strike, as most Source servers do
		}

		if (type == SERVERDATA_EXECCOMMAND)
		{
			if (!authenticated)
				return false;
			uint64_t token = ConsoleBridge::SubmitCommand(body);
			std::string output;
			if (!ConsoleBridge::WaitForResponse(token, output, RESPONSE_TIMEOUT_MS))
				output = "[halflife-cli] timed out waiting for command output";
			// keep responses within the conventional 4096-byte body limit
			static const char truncNote[] = "\n[halflife-cli] output truncated";
			if (output.size() > 4096)
			{
				output.resize(4096 - (sizeof(truncNote) - 1));
				output += truncNote;
			}
			return SendPacket(sock, id, SERVERDATA_RESPONSE_VALUE, output);
		}

		// Unknown types are ignored per protocol convention.
		return true;
	}

	void ConnectionThread(SOCKET sock)
	{
		while (g_running && HandlePacket(sock))
		{
		}
		closesocket(sock);
		g_activeConns--;
	}

	bool IpAllowed(const std::string& ip)
	{
		if (g_allowedIps.empty())
			return true;
		for (const auto& allowed : g_allowedIps)
			if (!allowed.empty() && allowed == ip)
				return true;
		return false;
	}

	void ListenerThread()
	{
		while (g_running)
		{
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(g_listener, &readfds);
			timeval tv{ 1, 0 };
			int ret = select((int)g_listener + 1, &readfds, nullptr, nullptr, &tv);
			if (ret <= 0)
				continue;

			SOCKADDR_IN peer{};
			int peerLen = sizeof(peer);
			SOCKET client = accept(g_listener, (SOCKADDR *)&peer, &peerLen);
			if (client == INVALID_SOCKET)
				continue;

			char ip[64] = { 0 };
			inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
			if (!IpAllowed(ip))
			{
				closesocket(client);
				continue;
			}
			if (g_activeConns.load() >= MAX_ACTIVE_CONNECTIONS)
			{
				closesocket(client);
				continue;
			}

			g_activeConns++;
			std::thread(ConnectionThread, client).detach();
		}
	}
}

namespace RconServer
{
	StartResult Start(const std::string& bindAddr, unsigned short port,
		const std::string& password, const std::string& allowedIps)
	{
		StartResult result;

		// LoadClient runs again on every map change; keep the live listener.
		if (g_running)
		{
			result.ok = true;
			result.port = g_port.load();
			return result;
		}

		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			result.error = "WSAStartup failed";
			return result;
		}

		g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (g_listener == INVALID_SOCKET)
		{
			result.error = "socket() failed";
			return result;
		}

		BOOL reuse = TRUE;
		setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

		SOCKADDR_IN addr{};
		addr.sin_family = AF_INET;
		if (bindAddr.empty() || bindAddr == "0.0.0.0")
			addr.sin_addr.s_addr = INADDR_ANY;
		else if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1)
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // fail safe: localhost
		addr.sin_port = htons(port);

		if (bind(g_listener, (SOCKADDR *)&addr, sizeof(addr)) == SOCKET_ERROR)
		{
			result.error = "bind failed (" + std::to_string(WSAGetLastError()) + ")";
			closesocket(g_listener);
			g_listener = INVALID_SOCKET;
			return result;
		}
		if (listen(g_listener, 4) == SOCKET_ERROR)
		{
			result.error = "listen failed";
			closesocket(g_listener);
			g_listener = INVALID_SOCKET;
			return result;
		}

		SOCKADDR_IN bound{};
		int blen = sizeof(bound);
		getsockname(g_listener, (SOCKADDR *)&bound, &blen);
		result.port = ntohs(bound.sin_port);
		g_port = result.port;

		g_password = password;
		// SplitCsv skips empty items, so an empty configured list stays empty
		// and IpAllowed keeps meaning "bind address only".
		g_allowedIps = text::SplitCsv(allowedIps);

		g_running = true;
		g_listenerThread = std::thread(ListenerThread);

		result.ok = true;
		return result;
	}

	void Shutdown()
	{
		if (!g_running.exchange(false))
			return;
		if (g_listener != INVALID_SOCKET)
		{
			closesocket(g_listener);
			g_listener = INVALID_SOCKET;
		}
		if (g_listenerThread.joinable())
			g_listenerThread.join();
		WSACleanup();
	}

	bool Running()
	{
		return g_running;
	}

	unsigned short CurrentPort()
	{
		return g_port.load();
	}
}
