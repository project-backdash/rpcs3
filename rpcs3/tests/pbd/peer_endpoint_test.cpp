#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_addr stays the IPv4 parser, exactly as in the peer socket setup
#endif
#include "pbd_test.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include "Emu/Cell/pbd_peer_endpoint.h"

#include <string>

#pragma comment(lib, "ws2_32.lib")

using pbd::test::check;

namespace
{
	using namespace pbd::peer_endpoint;

	endpoint groups(const std::array<unsigned, 8>& value, std::uint16_t port_network)
	{
		std::array<std::uint8_t, 16> bytes{};
		for (std::size_t i = 0; i < 8; i++)
		{
			bytes[2 * i] = static_cast<std::uint8_t>(value[i] >> 8);
			bytes[2 * i + 1] = static_cast<std::uint8_t>(value[i]);
		}
		return make_v6(bytes, port_network);
	}
} // namespace

static void canonical_ipv6_text(std::uint16_t port)
{
	// RFC 5952 text.
	check(address_text(groups({0, 0, 0, 0, 0, 0, 0, 0}, port)) == "::");
	check(address_text(groups({1, 0, 0, 0, 0, 0, 0, 0}, port)) == "1::");
	check(address_text(groups({1, 0, 0, 2, 0, 0, 0, 3}, port)) == "1:0:0:2::3");      // the longest run
	check(address_text(groups({1, 0, 0, 2, 3, 0, 0, 4}, port)) == "1::2:3:0:0:4");    // the first of equal runs
	check(address_text(groups({1, 0, 2, 3, 4, 5, 6, 7}, port)) == "1:0:2:3:4:5:6:7"); // a single zero group stays
	check(address_text(groups({0x2001, 0xdb8, 0xa, 0xbc, 0xdef, 0x1234, 0xffff, 0x10}, port)) == "2001:db8:a:bc:def:1234:ffff:10");
	check(address_text(endpoint{}) == "none");
}

PBD_TEST(PbdPeerEndpoint, Checks)
{
	WSADATA wsa{};
	check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	const std::uint16_t port = htons(7000);

	// IPv4 text: inet_addr decides; the log text is the dotted form.
	const auto v4 = parse_address("192.0.2.7", port);
	check(v4 && v4->kind == family::v4 && v4->port == port);
	check(v4->address[0] == 192 && v4->address[1] == 0 && v4->address[2] == 2 && v4->address[3] == 7 && v4->address[4] == 0 && v4->address[15] == 0);
	check(address_text(*v4) == "192.0.2.7" && host_port(*v4) == 7000 && text(*v4) == "192.0.2.7:7000");
	check(!is_loopback(*v4));
	const auto loopback4 = parse_address("127.0.0.1", port);
	check(loopback4 && loopback4->kind == family::v4 && is_loopback(*loopback4));
	const auto other127 = parse_address("127.0.0.2", port);
	check(other127 && !is_loopback(*other127)); // Only exactly 127.0.0.1 binds loopback.
	check(!parse_address("not-an-address", port));
	check(!parse_address("255.255.255.255", port)); // INADDR_NONE is refused.
	check(!parse_address("[2001:db8::5]", port));   // The launcher passes IPv6 without brackets.

	// IPv6 literals.
	const auto v6 = parse_address("2001:db8::5", port);
	check(v6 && v6->kind == family::v6 && v6->port == port);
	check(v6->address[0] == 0x20 && v6->address[1] == 0x01 && v6->address[2] == 0x0d && v6->address[3] == 0xb8 && v6->address[15] == 5);
	check(address_text(*v6) == "2001:db8::5" && text(*v6) == "[2001:db8::5]:7000" && !is_loopback(*v6));
	const auto spelled = parse_address("2001:0DB8:0000:0000:0000:0000:0000:0005", port);
	check(spelled && *spelled == *v6);
	const auto loopback6 = parse_address("::1", port);
	check(loopback6 && loopback6->kind == family::v6 && is_loopback(*loopback6) && address_text(*loopback6) == "::1");
	// An IPv4-mapped literal is the IPv4 peer it names (an IPv6-only socket could not reach it).
	const auto mapped = parse_address("::ffff:192.0.2.7", port);
	check(mapped && *mapped == *v4);

	// Comparison: family, address and port all count.
	check(*v6 == *v6 && !(*v6 != *v6));
	check(*v6 != *parse_address("2001:db8::5", htons(7001)));
	check(*v6 != *parse_address("2001:db8::6", port));
	std::array<std::uint8_t, 16> v4_bytes_as_v6{};
	v4_bytes_as_v6[0] = 192;
	v4_bytes_as_v6[2] = 2;
	v4_bytes_as_v6[3] = 7;
	check(make_v6(v4_bytes_as_v6, port) != *v4);
	check(make_v4(::inet_addr("192.0.2.7"), port) == *v4);
	check(endpoint{} != *v4 && endpoint{} == endpoint{});

	canonical_ipv6_text(port);
	// sockaddr round trips with the lengths sendto and recvfrom use.
	sockaddr_storage storage{};
	check(to_sockaddr(*v6, storage) == static_cast<int>(sizeof(sockaddr_in6)));
	const auto& in6 = reinterpret_cast<const sockaddr_in6&>(storage);
	check(in6.sin6_family == AF_INET6 && in6.sin6_port == port && in6.sin6_scope_id == 0);
	check(from_sockaddr(storage, sizeof(sockaddr_in6)) == *v6);
	check(from_sockaddr(storage, sizeof(sockaddr_in)).kind == family::none);
	check(to_sockaddr(*v4, storage) == static_cast<int>(sizeof(sockaddr_in)));
	const auto& in4 = reinterpret_cast<const sockaddr_in&>(storage);
	check(in4.sin_family == AF_INET && in4.sin_addr.s_addr == ::inet_addr("192.0.2.7") && in4.sin_port == port);
	check(from_sockaddr(storage, sizeof(sockaddr_in)) == *v4);
	check(to_sockaddr(endpoint{}, storage) == 0);
	check(from_sockaddr(storage, sizeof(storage)).kind == family::none);

	// The pinned source is only ever a global unicast IPv6 address: none for an IPv4 peer, and none toward ::1 (whose source
	// is ::1, or no route at all without IPv6), so loopback sessions keep binding ::1.
	check(!v6_source_toward(*v4));
	check(!v6_source_toward(*loopback6));

	// The shared target hands back exactly what was stored.
	shared_endpoint shared;
	check(shared.load() == endpoint{});
	shared.store(*v6);
	check(shared.load() == *v6);
	shared.store(*v4);
	check(shared.load() == *v4);

	WSACleanup();
}
