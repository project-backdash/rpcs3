#include "pbd_test.h"
#include "Emu/PBD/pbd_peer_local_route.h"
#include "Emu/PBD/pbd_peer_auth_state.h"
#include "Emu/PBD/pbd_peer_message.h"
#include "Emu/PBD/pbd_peer_recovery.h"

using pbd::test::check;

#ifdef _WIN32
namespace
{
	using endpoint = pbd::peer_endpoint::endpoint;
	using local_socket = pbd::peer_local_route::socket;
	endpoint at(const local_socket& socket)
	{
		return *pbd::peer_endpoint::parse_address("::1", htons(socket.port()));
	}
	void send(SOCKET socket, endpoint target, std::span<const std::uint8_t> packet)
	{
		sockaddr_storage address{};
		const auto length = pbd::peer_endpoint::to_sockaddr(target, address);
		check(::sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
				  reinterpret_cast<const sockaddr*>(&address), length) == static_cast<int>(packet.size()));
	}
	struct peer
	{
		local_socket direct, local, shim;
		pbd::peer_auth::state auth;
		pbd::peer_recovery::state path;
		pbd::peer_recovery::context identity;
		std::uint32_t role = 0, inputs = 0;
		endpoint target{};
		peer(std::uint32_t side, const pbd::peer_auth::key& key) : identity{"game-" + std::to_string(side), "set", 42, 1}, role(side)
		{
			check(direct.open() && local.open() && shim.open() && auth.install(key, role));
		}
		void configure(const peer& other)
		{
			target = at(other.direct);
			path.configure(identity, target, 1000, direct.port(), local.port());
		}
		void emit(bool drop_direct, std::uint64_t now)
		{
			pbd::peer_message::input_message input;
			input.role = role;
			input.sync.session = 42;
			input.sync.send_us = now;
			input.count = 1;
			input.inputs[0] = {inputs, static_cast<std::uint16_t>(role + 1)};
			std::array<std::uint8_t, pbd::peer_message::max_input_bytes> payload{};
			const auto size = pbd::peer_message::encode_input(input, payload);
			const auto transmit = [&](endpoint route)
			{
				std::array<std::uint8_t, pbd::peer_auth::maximum_bytes> packet{};
				const auto sealed = auth.send(std::span(payload).first(size), packet);
				check(sealed > 0);
				if (route == at(shim))
					send(local.handle(), route, std::span(packet).first(sealed));
				else if (!drop_direct)
					send(direct.handle(), route, std::span(packet).first(sealed));
			};
			path.sent(auth.epoch(), now);
			transmit(target);
			if (const auto candidate = path.probe(1))
				transmit(*candidate);
		}
		void relay_to(peer& other, bool drop)
		{
			std::array<std::uint8_t, 1024> bytes{};
			const auto n = ::recv(shim.handle(), reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
			if (n > 0 && !drop)
				send(other.shim.handle(), at(other.local), std::span(bytes).first(n));
		}
		void receive(std::uint64_t now)
		{
			for (const auto socket : {direct.handle(), local.handle()})
			{
				std::array<std::uint8_t, 1024> bytes{};
				sockaddr_storage address{};
				int length = sizeof(address);
				for (int n; (n = ::recvfrom(socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
								 reinterpret_cast<sockaddr*>(&address), &length)) > 0;
					length = sizeof(address))
				{
					const auto source = pbd::peer_endpoint::from_sockaddr(address, length);
					path.datagram(source == target, now);
					const auto message = auth.receive(std::span(bytes).first(n));
					if (!message || !auth.current(message->epoch) ||
						pbd::peer_message::check_input(message->payload, role, 42) != pbd::peer_message::input_check::valid ||
						(source != target && !path.allows(source, message->epoch, now)))
						continue;
					path.received(message->epoch, now);
					path.prove(source, message->epoch, now);
					++inputs;
				}
			}
		}
		void stage(std::uint64_t now)
		{
			check(path.apply({identity, 1, pbd::peer_recovery::operation::stage, 1}, at(shim), {}, now));
		}
		bool commit(std::uint64_t now)
		{
			return path.apply({identity, path.get().command_serial + 1, pbd::peer_recovery::operation::commit, 1}, {}, [&](const auto& next)
				{
					target = next;
				},
				now);
		}
	};
} // namespace

PBD_TEST(PbdDirectFallback, LoopbackKeepsAuthenticatedInputsAcrossDroppedAndAsymmetricPaths)
{
	WSADATA wsa{};
	check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	for (const bool drop_direct : {false, true})
	{
		pbd::peer_auth::key key{};
		key[0] = 53;
		peer left(0, key), right(1, key);
		left.configure(right);
		right.configure(left);
		std::uint64_t now = 10000;
		const auto exchange = [&](bool drop_relay)
		{
			left.emit(drop_direct, now);
			right.emit(drop_direct, now);
			left.relay_to(right, drop_relay);
			right.relay_to(left, drop_relay);
			left.receive(now);
			right.receive(now);
			now += 10000;
		};
		exchange(false);
		if (!drop_direct)
		{
			check(left.inputs && right.inputs && !left.path.get().active_proposal && !right.path.get().active_proposal);
			continue;
		}
		check(!left.inputs && !right.inputs);
		left.stage(now);
		right.stage(now);
		// A failed relay cannot manufacture proof or admit a switch.
		exchange(true);
		check(!left.commit(now) && !right.commit(now));
		for (int i = 0; i < 8; ++i)
			exchange(false);
		check(left.inputs && right.inputs && left.path.get().verified_proposal == 1 && right.path.get().verified_proposal == 1);
		check(left.commit(now));
		const auto before = right.inputs;
		// One acknowledgement is delayed: the staged side keeps receiving the committed side's inputs.
		for (int i = 0; i < 8; ++i)
			exchange(false);
		check(right.inputs > before && right.commit(now));
		const auto played_left = left.inputs, played_right = right.inputs;
		for (int i = 0; i < 8; ++i)
			exchange(false);
		check(left.inputs > played_left && right.inputs > played_right);
		check(left.path.get().active_proposal == 1 && right.path.get().active_proposal == 1);
		check(left.auth.epoch() == 1 && right.auth.epoch() == 1);
	}
	WSACleanup();
}
#endif
