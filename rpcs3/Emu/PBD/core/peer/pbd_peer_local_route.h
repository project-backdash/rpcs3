#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>

namespace pbd::peer_local_route
{
	// A pinned public IPv6 socket cannot receive on ::1. This companion uses the same authenticated stream.
	class socket
	{
		SOCKET value = INVALID_SOCKET;
		std::uint16_t bound_port = 0;

	public:
		~socket()
		{
			close();
		}
		void close()
		{
			if (value != INVALID_SOCKET)
				::closesocket(value);
			value = INVALID_SOCKET;
			bound_port = 0;
		}
		bool open()
		{
			close();
			value = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
			if (value == INVALID_SOCKET)
				return false;
			sockaddr_in6 local{};
			local.sin6_family = AF_INET6;
			local.sin6_addr.s6_addr[15] = 1;
			DWORD v6only = 1;
			u_long nonblocking = 1;
			int size = sizeof(local);
			if (::setsockopt(value, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) ||
				::bind(value, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) ||
				::getsockname(value, reinterpret_cast<sockaddr*>(&local), &size) || ::ioctlsocket(value, FIONBIO, &nonblocking))
			{
				close();
				return false;
			}
			bound_port = ntohs(local.sin6_port);
			return true;
		}
		SOCKET handle() const
		{
			return value;
		}
		std::uint16_t port() const
		{
			return bound_port;
		}
		SOCKET sender(SOCKET direct, bool loopback) const
		{
			return loopback && value != INVALID_SOCKET ? value : direct;
		}
	};
} // namespace pbd::peer_local_route
#endif
