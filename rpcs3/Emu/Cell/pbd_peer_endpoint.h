#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

// The peer's UDP endpoint. PBD_PEER_ADDRESS selects one socket family for the session;
// endpoint comparisons include the family, so identical IPv4 and IPv6 address bytes never match.
namespace pbd::peer_endpoint
{
	enum class family : std::uint8_t
	{
		none = 0,
		v4 = 4,
		v6 = 6,
	};

	struct endpoint
	{
		family kind = family::none;
		std::array<std::uint8_t, 16> address{}; // Network byte order. IPv4 uses the first four bytes; the rest stay zero.
		std::uint16_t port = 0;                 // Network byte order, as in sin_port / sin6_port.

		bool operator==(const endpoint&) const = default;
	};

	// address_network and port_network are in network byte order (s_addr, sin_port).
	inline endpoint make_v4(std::uint32_t address_network, std::uint16_t port_network) noexcept
	{
		endpoint result;
		result.kind = family::v4;
		std::memcpy(result.address.data(), &address_network, 4);
		result.port = port_network;
		return result;
	}

	inline endpoint make_v6(const std::array<std::uint8_t, 16>& address, std::uint16_t port_network) noexcept
	{
		endpoint result;
		result.kind = family::v6;
		result.address = address;
		result.port = port_network;
		return result;
	}

	inline std::uint16_t host_port(const endpoint& value) noexcept
	{
		std::array<std::uint8_t, 2> bytes{};
		std::memcpy(bytes.data(), &value.port, 2);
		return static_cast<std::uint16_t>(bytes[0] << 8 | bytes[1]);
	}

	// ::ffff:a.b.c.d names an IPv4 host; an IPv6-only socket cannot reach it, so parse_address returns it as that IPv4 peer.
	inline bool is_v4_mapped(const std::array<std::uint8_t, 16>& address) noexcept
	{
		for (std::size_t i = 0; i < 10; i++)
			if (address[i])
				return false;
		return address[10] == 0xff && address[11] == 0xff;
	}

	// Only 127.0.0.1 and ::1 select a loopback bind; other addresses in 127/8 do not.
	inline bool is_loopback(const endpoint& value) noexcept
	{
		if (value.kind == family::v4)
			return value.address[0] == 127 && value.address[1] == 0 && value.address[2] == 0 && value.address[3] == 1;
		if (value.kind != family::v6)
			return false;
		for (std::size_t i = 0; i < 15; i++)
			if (value.address[i])
				return false;
		return value.address[15] == 1;
	}

	// IPv4: dotted quad. IPv6: the RFC 5952 form (lowercase, no leading zeros, the first longest run of two or more zero
	// groups shortened to "::"), so a logged address can be compared with what the launcher passed.
	inline std::string address_text(const endpoint& value)
	{
		std::string out;
		if (value.kind == family::v4)
		{
			for (std::size_t i = 0; i < 4; i++)
			{
				if (i)
					out += '.';
				out += std::to_string(value.address[i]);
			}
			return out;
		}
		if (value.kind != family::v6)
			return "none";
		std::array<unsigned, 8> group{};
		for (std::size_t i = 0; i < 8; i++)
			group[i] = static_cast<unsigned>(value.address[2 * i]) << 8 | value.address[2 * i + 1];
		std::size_t zero_at = 8, zero_length = 0;
		for (std::size_t i = 0; i < 8;)
		{
			if (group[i])
			{
				i++;
				continue;
			}
			std::size_t end = i;
			while (end < 8 && !group[end])
				end++;
			if (end - i > zero_length)
			{
				zero_at = i;
				zero_length = end - i;
			}
			i = end;
		}
		if (zero_length < 2)
			zero_at = 8;
		constexpr char digits[] = "0123456789abcdef";
		for (std::size_t i = 0; i < 8; i++)
		{
			if (i == zero_at)
			{
				out += "::";
				i += zero_length - 1;
				continue;
			}
			if (!out.empty() && out.back() != ':')
				out += ':';
			bool leading = true;
			for (int shift = 12; shift >= 0; shift -= 4)
			{
				const unsigned digit = (group[i] >> shift) & 15;
				if (leading && digit == 0 && shift)
					continue;
				leading = false;
				out += digits[digit];
			}
		}
		return out;
	}

	// Log form: a.b.c.d:port for IPv4, [address]:port for IPv6.
	inline std::string text(const endpoint& value)
	{
		if (value.kind == family::v6)
			return "[" + address_text(value) + "]:" + std::to_string(host_port(value));
		return address_text(value) + ":" + std::to_string(host_port(value));
	}

	// The latch thread updates the send target during setup and endpoint learning; both it and the receiver read it.
	// The lock keeps family, address and port together in each snapshot.
	class shared_endpoint
	{
		mutable std::mutex m_mutex;
		endpoint m_value{};

	public:
		endpoint load() const
		{
			std::lock_guard lock(m_mutex);
			return m_value;
		}

		void store(const endpoint& value)
		{
			std::lock_guard lock(m_mutex);
			m_value = value;
		}
	};

#ifdef _WIN32
	// Requires <winsock2.h> and <ws2tcpip.h> (included before this header).

	// PBD_PEER_ADDRESS accepts IPv6 literals without brackets or a zone id and maps IPv4-mapped literals to IPv4.
	// Other text goes through inet_addr; INADDR_NONE is rejected.
	inline std::optional<endpoint> parse_address(const char* text, std::uint16_t port_network)
	{
		in6_addr v6{};
		if (::inet_pton(AF_INET6, text, &v6) == 1)
		{
			std::array<std::uint8_t, 16> bytes{};
			std::memcpy(bytes.data(), &v6, 16);
			if (is_v4_mapped(bytes))
			{
				std::uint32_t mapped = 0;
				std::memcpy(&mapped, bytes.data() + 12, 4);
				return make_v4(mapped, port_network);
			}
			return make_v6(bytes, port_network);
		}
		const auto v4 = ::inet_addr(text);
		if (v4 == INADDR_NONE)
			return std::nullopt;
		return make_v4(v4, port_network);
	}

	// Fills a sockaddr for sendto and returns its length (0 for an endpoint without a family).
	inline int to_sockaddr(const endpoint& value, sockaddr_storage& out) noexcept
	{
		out = {};
		if (value.kind == family::v4)
		{
			auto& target = reinterpret_cast<sockaddr_in&>(out);
			target.sin_family = AF_INET;
			std::memcpy(&target.sin_addr, value.address.data(), 4);
			target.sin_port = value.port;
			return static_cast<int>(sizeof(sockaddr_in));
		}
		if (value.kind == family::v6)
		{
			auto& target = reinterpret_cast<sockaddr_in6&>(out);
			target.sin6_family = AF_INET6;
			std::memcpy(&target.sin6_addr, value.address.data(), 16);
			target.sin6_port = value.port;
			return static_cast<int>(sizeof(sockaddr_in6));
		}
		return 0;
	}

	// The global unicast address (2000::/3) Windows sends to an IPv6 peer from right now: a UDP socket connected to the peer
	// reports the source it chose (connecting a UDP socket sends nothing). Empty for an IPv4 peer, without a route, or when the
	// choice is not global unicast.
	inline std::optional<in6_addr> v6_source_toward(const endpoint& peer) noexcept
	{
		if (peer.kind != family::v6)
			return std::nullopt;
		const SOCKET route_socket = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (route_socket == INVALID_SOCKET)
			return std::nullopt;
		sockaddr_storage target{};
		const int target_len = to_sockaddr(peer, target);
		sockaddr_in6 chosen{};
		int chosen_len = static_cast<int>(sizeof(chosen));
		const bool known = ::connect(route_socket, reinterpret_cast<const sockaddr*>(&target), target_len) == 0 &&
		                   ::getsockname(route_socket, reinterpret_cast<sockaddr*>(&chosen), &chosen_len) == 0 && chosen.sin6_family == AF_INET6;
		::closesocket(route_socket);
		std::array<std::uint8_t, 16> bytes{};
		std::memcpy(bytes.data(), &chosen.sin6_addr, 16);
		if (!known || (bytes[0] & 0xe0) != 0x20)
			return std::nullopt;
		return chosen.sin6_addr;
	}

	// The source recvfrom reported (family none if it is neither IPv4 nor IPv6, which then matches no target).
	inline endpoint from_sockaddr(const sockaddr_storage& in, int length) noexcept
	{
		endpoint result;
		if (in.ss_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in)))
		{
			const auto& source = reinterpret_cast<const sockaddr_in&>(in);
			result.kind = family::v4;
			std::memcpy(result.address.data(), &source.sin_addr, 4);
			result.port = source.sin_port;
		}
		else if (in.ss_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6)))
		{
			const auto& source = reinterpret_cast<const sockaddr_in6&>(in);
			result.kind = family::v6;
			std::memcpy(result.address.data(), &source.sin6_addr, 16);
			result.port = source.sin6_port;
		}
		return result;
	}
#endif
} // namespace pbd::peer_endpoint
