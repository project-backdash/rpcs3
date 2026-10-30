#pragma once

#include "pbd_env_parse.h"

struct pbd::rollback_session::peer_configuration
{
	storage& s;
	const match_bind::configuration* bound;

	void run()
	{
		if (configure_standby())
			return;

		if (const auto role = pbd::env::find_wide("PBD_PEER_ROLE"); bound || (role && *role))
		{
			// Two-peer mode: owned port from the local tape or device, the other over UDP.

			configure_history();
			// LAN/internet: PBD_PEER_ADDRESS names the other peer's IPv4 address, or an IPv6 literal without brackets
			// or zone id (the socket family follows it). Absent keeps the loopback behaviour that every recorded two-peer
			// result used. The address is ASCII, so a narrow copy suffices.
			configure_identity();
			configure_route();
			configure_delay();
			configure_input_policy();
			configure_socket();
			open_transport_records();
			s.correction.active = true;
			s.peer.enabled = true;
		}
	}

	bool configure_standby()
	{
		if (const auto root = pbd::env::find_wide("PBD_MATCH_STANDBY_CHANNEL"); root && *root && !bound)
		{
			const auto absent = [](const char* name)
			{
				const auto value = pbd::env::find_wide(name);
				return !value || !*value;
			};
			require(s.session.enabled && s.round_lifecycle && s.session.digests.enabled && s.session.start_skip &&
						s.session.gate.enabled && s.battle_clock.version == 2 && !s.checkpoint.ring.empty() &&
						!s.practice.rewind && !s.practice.dummy.enabled && !s.replay.session_replay && !s.rolling &&
						!s.roundtrip.active && !s.checkpoint.late_index && !s.session.seed_set &&
						absent("PBD_PEER_ROLE") && absent("PBD_SET_CHANNEL") && absent("PBD_SPECTATOR_CHANNEL") &&
						absent("PBD_SYNC_FRAME_COUNTER") && !pbd_direct_match::requested(),
				"match_standby_policy_invalid");
			const auto required = pbd::env::find_wide("PBD_PEER_AUTH_REQUIRED");
			require(required && std::wstring_view(required) == L"1" && pbd::peer_key_pipe::inherited_descriptor(), "match_standby_auth_required");
			const std::filesystem::path directory(root);
			const auto stop = pbd::env::find_wide("PBD_STOP_FILE");
			require(directory.is_absolute() && std::filesystem::is_directory(directory) &&
						!std::filesystem::exists(directory / "match-bind.json") && !std::filesystem::exists(directory / "match-standby.json"),
				"match_standby_directory_invalid");
			require(stop && *stop && std::filesystem::path(stop).is_absolute(), "match_standby_stop_file_required");
			s.lobby.match_standby = directory;
			s.correction.active = true;
			s.history.session_continuous = s.history.session_history_rolling = true;
			s.trace = std::make_unique<pbd::session_runtime::trace_buffer>();
			s.extend_session_history();
			return true;
		}
		return false;
	}

	void configure_history()
	{
		// Practice is parsed first, so its own local-session check cannot see a peer configured here; refused here
		// instead, since the Practice dummy writes the other port's word and would change what a peer receives.
		require(!s.practice.rewind && !s.practice.dummy.enabled, "peer_forbids_practice");
		s.peer.role = bound ? bound->role : number("PBD_PEER_ROLE");
		s.peer.delay = number("PBD_PEER_DELAY");
		// Prediction bound: how far the local peer runs ahead of the first unusable remote tick before holding.
		if (bound && bound->prediction_bound)
			s.peer.max_prediction = *bound->prediction_bound;
		else if (const auto value = pbd::env::find_wide("PBD_PEER_MAX_PREDICTION"); value && *value)
			s.peer.max_prediction = number("PBD_PEER_MAX_PREDICTION");
		else
			s.peer.max_prediction = static_cast<u32>(s.checkpoint.ring.size() - 1);
		s.peer.link.local_port = bound ? bound->local_port : number("PBD_PEER_LOCAL_PORT");
		s.peer.link.remote_port = bound ? bound->remote_port : number("PBD_PEER_REMOTE_PORT");
		s.roll_begin = s.launch.direct() ? pbd::session_policy::roll_begin : number("PBD_TICK_ROLL_BEGIN");
		s.roll_end = s.launch.direct() ? 0 : number("PBD_TICK_ROLL_END");
		require(!s.checkpoint.ring.empty() && !s.roundtrip.active && !s.checkpoint.late_index && !s.rolling, "peer_requires_ring_without_rolling");
		require(s.peer.max_prediction >= s.peer.delay && s.peer.max_prediction < s.checkpoint.ring.size(), "peer_max_prediction_invalid");
		require(s.peer.role <= 1 && s.peer.delay >= 1 && s.peer.delay + 1 < s.checkpoint.ring.size() && s.checkpoint.ring.size() <= 9 &&
					s.peer.link.local_port >= 1024 && s.peer.link.local_port <= 65535 && s.peer.link.remote_port >= 1024 && s.peer.link.remote_port <= 65535 &&
					s.roll_begin > s.checkpoint.tick_hold_index + 1 + s.peer.delay &&
					(!s.roll_end || (s.roll_begin + s.peer.delay <= s.roll_end && s.roll_end + s.peer.delay < s.extent())),
			"peer_parameters_invalid");
		s.history.session_continuous = !s.roll_end;
		require(!s.history.session_continuous || s.session.enabled, "continuous_peer_requires_session");
		s.history.session_history_rolling = s.session.enabled && (s.history.session_continuous || s.extent() > max_updates);
		if (s.history.session_continuous && !s.trace)
			s.trace = std::make_unique<pbd::session_runtime::trace_buffer>();
		require(s.session.enabled || s.extent() <= max_updates, "long_history_requires_session");
		if (s.history.session_history_rolling)
			s.extend_session_history();
	}

	void configure_identity()
	{
		if (bound)
			s.peer.session = bound->session;
		else if (const auto session = pbd::env::find_wide("PBD_PEER_SESSION"); session && *session)
		{
			u32 value = 0;
			require(pbd::env::parse_u32_decimal(session, value), "peer_session_invalid");
			s.peer.session = static_cast<u32>(value);
		}
		if (const auto required = pbd::env::find_wide("PBD_PEER_AUTH_REQUIRED"); required && *required)
		{
			require(std::wstring_view(required) == L"1", "peer_auth_contract_invalid");
			s.peer.link.key_pipe = pbd::peer_key_pipe::inherited_descriptor();
			require(s.peer.link.key_pipe.has_value(), "peer_auth_pipe_invalid");
			s.peer.link.auth_set_id = bound ? bound->set_id : s.peer.link.key_pipe->initial_set_id;
			require(pbd::peer_key_pipe::install(s.peer.link.auth, *s.peer.link.key_pipe, s.peer.role, s.peer.session, s.peer.link.auth_set_id), "peer_auth_key_unavailable");
		}
		const auto* address = pbd::env::find_wide("PBD_PEER_ADDRESS");
		if (bound || (address && *address))
		{
			const std::wstring text = bound ? std::wstring(bound->address.begin(), bound->address.end()) : address;
			require(text.size() + 1 < s.peer.link.address.size(), "peer_address_too_long");
			s.peer.link.address.fill(0);
			for (usz i = 0; i < text.size(); i++)
			{
				require(text[i] > 0 && text[i] < 128, "peer_address_not_ascii");
				s.peer.link.address[i] = static_cast<char>(text[i]);
			}
		}
	}

	void configure_route()
	{
		// Live controller plumbing, guarded and off by default; peer mode only.
		if (const auto live = pbd::env::find_wide("PBD_PEER_LIVE"); live && std::wstring_view(live) == L"1")
			s.peer.live.enabled = true;
		if (const auto early = pbd::env::find_wide("PBD_PEER_USE_ARRIVED"); early && std::wstring_view(early) == L"1")
			s.peer.use_arrived = true;
		if (const auto wait = pbd::env::find_wide("PBD_PEER_WAIT_MS"); wait && *wait)
		{
			wchar_t* end = nullptr;
			const double ms = std::wcstod(wait, &end);
			require(end && !*end && ms >= 0 && ms <= 100, "peer_wait_ms_invalid");
			s.peer.wait_ns = static_cast<u64>(ms * 1e6);
		}
		if (const auto learn = pbd::env::find_wide("PBD_PEER_LEARN_ENDPOINT"); learn && std::wstring_view(learn) == L"1")
			s.peer.link.learn_endpoint = true;
		// The launcher's route: only the connection line's Relay tag reads it; the transport is the same either way (the relay
		// is a loopback port on this PC that forwards to the other PC's).
		s.peer.link.relayed = bound && bound->relayed;
		if (const auto route = pbd::env::find_wide("PBD_PEER_ROUTE"); !bound && route && std::wstring_view(route) == L"relay")
			s.peer.link.relayed = true;
		if (const auto always = pbd::env::find_wide("PBD_PEER_LEARN_ALWAYS"); always && std::wstring_view(always) == L"1")
			s.peer.link.learn_always = true;
		// An unauthenticated older pair is pinned even when its caller requested NAT endpoint learning.
		s.peer.link.learn_endpoint = s.peer.link.learn_endpoint && s.peer.link.auth.can_learn();
		s.peer.link.learn_always = s.peer.link.learn_always && s.peer.link.learn_endpoint;
	}

	void configure_delay()
	{
		std::optional<u32> delay;
		bool automatic = false;
		if (bound && bound->prediction_bound)
		{
			delay = bound->input_delay;
			automatic = !delay;
		}
		else if (const auto* value = pbd::env::find_wide("PBD_PEER_INPUT_DELAY"); value && *value)
		{
			automatic = std::wstring_view(value) == L"auto";
			if (!automatic)
			{
				u32 parsed = 0;
				require(pbd::env::parse_u32_decimal(value, parsed) && parsed <= 6, "peer_input_delay_invalid");
				delay = parsed;
			}
		}
		if (automatic)
		{
			s.peer.delay_auto = true;
			s.peer.input_delay = 4;
		}
		else if (delay)
		{
			require(*delay <= 6, "peer_input_delay_invalid");
			s.peer.input_delay = *delay;
			const auto* opening = pbd::env::find_wide("PBD_PEER_OPENING_DELAY");
			if (*delay < 4 && !(opening && std::wstring_view(opening) == L"0"))
			{
				s.peer.delay_auto = true;
				s.peer.delay_forced = static_cast<s32>(*delay);
				s.peer.input_delay = 4;
			}
		}
		if (const auto cap = pbd::env::find_wide("PBD_PEER_AUTO_MAX_DELAY"); cap && *cap)
		{
			wchar_t* end = nullptr;
			const auto v = std::wcstoul(cap, &end, 10);
			require(end && !*end && v <= 5, "peer_auto_max_delay_invalid");
			s.peer.delay_auto_max = static_cast<u32>(v);
		}
	}

	void configure_input_policy()
	{
		if (const auto balance = pbd::env::find_wide("PBD_PEER_BALANCE"); balance && std::wstring_view(balance) == L"1")
			s.peer.sync.balance = true;
		if (const auto sync = pbd::env::find_wide("PBD_PEER_SYNC"); sync && std::wstring_view(sync) == L"0")
			s.peer.sync.enabled = false;
		if (const auto rx = pbd::env::find_wide("PBD_PEER_RX_THREAD"); rx && std::wstring_view(rx) == L"0")
			s.peer.rx.enabled = false;
		if (const auto due = pbd::env::find_wide("PBD_PEER_WAIT_DUE"); due && std::wstring_view(due) == L"0")
			s.peer.wait_due = false;
		if (const auto entry = pbd::env::find_wide("PBD_PEER_ENTRY_SAMPLE"); entry && std::wstring_view(entry) == L"0")
			s.peer.live.entry_sample = false;
		if (const auto phase = pbd::env::find_wide("PBD_PEER_PHASE_DELAY"); phase && std::wstring_view(phase) == L"0")
			s.peer.phase_delay = false;
		if (!s.peer.live.entry_sample || !s.peer.live.enabled)
			s.peer.phase_delay = false;
		s.peer.live.bootstrap_sample = s.peer.phase_delay;
		if (const auto start = pbd::env::find_wide("PBD_PEER_FIGHT_START"); start && *start)
			s.peer.fight_start = static_cast<u32>(std::wcstoul(start, nullptr, 10));
		if (const auto mode = pbd::env::find_wide("PBD_PEER_FIGHT_PREDICT"); mode && *mode)
			s.peer.fight_mode = std::min<u32>(static_cast<u32>(std::wcstoul(mode, nullptr, 10)), 2u);
		if (!s.peer.phase_delay || !s.peer.fight_start)
			s.peer.fight_mode = 0;
		s.correction.readback.policy = 1;
		if (const auto readback = pbd::env::find_wide("PBD_RESTORE_READBACK"); readback && *readback)
		{
			const std::wstring_view v(readback);
			require(v == L"all" || v == L"sample" || v == L"off" || v == L"first4", "restore_readback_invalid");
			s.correction.readback.policy = v == L"all" ? 0u : v == L"sample" ? 1u :
			                                              v == L"first4"     ? 3u :
			                                                                   2u;
		}
		if (s.peer.live.enabled)
		{
			const auto input_sequence = pbd::env::find_wide("PBD_INPUT_TAPE");
			s.peer.live.pad_script = input_sequence && *input_sequence;
			// The ordinary keyread ring, sampling and transport remain authoritative; the diagnostic tape supplies only
			// the local pad read. A missing, malformed or foreign-port tape cannot silently become device input.
			if (s.peer.live.pad_script)
				require(pbd::get_input_script().supplies_only_port(s.peer.role), "peer_local_pad_tape_invalid");
		}
		if (const auto stop = pbd::env::find_wide("PBD_STOP_FILE"); stop && *stop)
		{
			require(std::filesystem::path(stop).is_absolute(), "stop_file_path_invalid");
			const auto stop_utf8 = std::filesystem::path(stop).u8string();
			s.stop_file.assign(stop_utf8.begin(), stop_utf8.end());
		}
	}

	void bind_socket(SOCKET sock, const auto& remote, bool remote_v6, bool loopback_only)
	{
		if (remote_v6)
		{
			// IPv6-only, so this socket never sees IPv4-mapped traffic and every source it reports is IPv6. It binds
			// the same port number the IPv4 path would have used.
			DWORD v6only = 1;
			require(::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) == 0, "peer_v6only_failed");
			sockaddr_in6 local{};
			local.sin6_family = AF_INET6;
			local.sin6_port = htons(static_cast<u16>(s.peer.link.local_port));
			// A loopback peer binds ::1. Otherwise the socket binds the address Windows sends to this peer from now:
			// the one the launcher's handshake (bound to the zero address) just used, the only one the other side's
			// firewall admits. Bound to the zero address, Windows chooses the source per send, so a temporary (privacy)
			// address replaced mid-set (about daily) can move later sends to the fresh one, which that firewall drops.
			// Without that address, or if binding it fails, the zero address (in6addr_any) is bound and accepts
			// datagrams on every interface.
			bool pinned = false;
			if (loopback_only)
				local.sin6_addr.s6_addr[15] = 1;
			else if (const auto source = pbd::peer_endpoint::v6_source_toward(*remote))
			{
				local.sin6_addr = *source;
				pinned = true;
			}
			bool bound = ::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
			if (!bound && pinned)
			{
				local.sin6_addr = in6_addr{};
				pinned = false;
				bound = ::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
			}
			require(local.sin6_port != remote->port || std::memcmp(local.sin6_addr.s6_addr, remote->address.data(), 16) != 0, "peer_self_endpoint");
			require(bound, "peer_bind_failed");
			if (!loopback_only)
			{
				auto bound_at = *remote;
				std::memcpy(bound_at.address.data(), &local.sin6_addr, 16);
				bound_at.port = local.sin6_port;
				pbd_session_log.notice("Netplay: IPv6 session to %s, sending from %s", pbd::peer_endpoint::text(*remote),
					pinned ? pbd::peer_endpoint::text(bound_at) : std::string("the address Windows picks per send"));
			}
		}
		else
		{
			sockaddr_in local{};
			local.sin_family = AF_INET;
			local.sin_port = htons(static_cast<u16>(s.peer.link.local_port));
			// A loopback peer binds loopback; a LAN peer must accept datagrams on the real interface.
			local.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
			require(local.sin_port != remote->port || std::memcmp(&local.sin_addr, remote->address.data(), 4) != 0, "peer_self_endpoint");
			require(::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "peer_bind_failed");
		}
	}

	void configure_socket()
	{
		WSADATA wsa{};
		require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "peer_wsastartup_failed");
		s.peer.link.wsa_started = true;
		const auto remote = pbd::peer_endpoint::parse_address(s.peer.link.address.data(), htons(static_cast<u16>(s.peer.link.remote_port)));
		const bool remote_v6 = remote && remote->kind == pbd::peer_endpoint::family::v6;
		const SOCKET sock = ::socket(remote_v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		require(sock != INVALID_SOCKET, "peer_socket_failed");
		s.peer.link.socket_handle = static_cast<u64>(sock);
		require(remote.has_value(), "peer_address_invalid");
		const bool loopback_only = pbd::peer_endpoint::is_loopback(*remote);
		bind_socket(sock, remote, remote_v6, loopback_only);
		// The socket is not connected; datagrams go to the current target (sendto) and arrive from
		// anywhere (recvfrom). A required key authenticates every payload before its codec or source can change state.
		s.peer.link.target.store(*remote);
		u_long nonblocking = 1;
		require(ioctlsocket(sock, FIONBIO, &nonblocking) == 0, "peer_nonblocking_failed");
	}

	void open_transport_records()
	{
		s.history.peer_remote_arrival.fill(-1);
		if (const auto* trace = pbd::env::find("PBD_PEER_TRACE_INPUTS"); trace && std::string_view(trace) == "1")
		{
			try
			{
				s.peer.input_trace_file.reset(open_diagnostic_file(s.checkpoint.capture_directory / "peer-inputs.jsonl"));
				if (s.peer.input_trace_file)
					s.peer.input_trace = std::make_unique<pbd::peer_input_trace::queue<>>();
				else
					pbd_session_log.error("Peer input tracing disabled: the output file could not be opened.");
			}
			catch (...)
			{
				s.peer.input_trace_file.reset();
				pbd_session_log.error("Peer input tracing disabled: the diagnostic buffer could not be allocated.");
			}
		}
		const auto peer_path = (s.checkpoint.capture_directory / "peer-transport.jsonl").u8string();
		require(s.peer.file.open(std::string(peer_path.begin(), peer_path.end())), "cannot_create_peer_transport_exclusively");
		const auto correction_path = (s.checkpoint.capture_directory / "tick-correction.jsonl").u8string();
		require(s.correction.file.open(std::string(correction_path.begin(), correction_path.end()), fs::write_new + fs::isfile), "cannot_create_tick_correction_exclusively");
		const auto header = fmt::format("{\"type\":\"header\",\"schema\":1,\"role\":%u,\"delay\":%u,\"local_port\":%u,\"remote_port\":%u,\"window\":[%u,%u],\"ring\":%u,\"address\":\"%s\",\"live\":%s,"
										"\"input_delay\":%u,\"learn_endpoint\":%s,\"use_arrived\":%s,\"wait_ms\":%u,\"message_version\":%u,"
										"\"sim_delay_us\":0,\"sim_jitter_us\":0,\"sim_loss_pct\":0,\"input_delay_auto\":%s,\"balance\":%s,\"sync\":%s,\"rx_thread\":%s,\"entry_sample\":%s,\"wait_due\":%s,\"phase_delay\":%s,\"bootstrap_sample\":%s,\"fight_start\":%u,\"fight_mode\":%u}\n",
			s.peer.role, s.peer.delay, s.peer.link.local_port, s.peer.link.remote_port, s.roll_begin, s.roll_end, s.checkpoint.ring.size(),
			s.peer.link.address.data(), s.peer.live.enabled ? "true" : "false",
			s.peer.input_delay, s.peer.link.learn_endpoint ? "true" : "false", s.peer.use_arrived ? "true" : "false", static_cast<u32>(s.peer.wait_ns / 1000000), pbd::peer_message::input_version,
			s.peer.delay_auto ? "true" : "false", s.peer.sync.balance ? "true" : "false",
			s.peer.sync.enabled ? "true" : "false", s.peer.rx.enabled ? "true" : "false", s.peer.live.entry_sample ? "true" : "false",
			s.peer.wait_due ? "true" : "false",
			s.peer.phase_delay && s.peer.input_delay >= 1 ? "true" : "false", s.peer.live.bootstrap_sample ? "true" : "false", s.peer.fight_start, s.peer.fight_mode);
		s.peer.file.write(header.data(), header.size());
		s.peer.file.start([]([[maybe_unused]] std::thread& writer)
			{
#ifdef _WIN32
				CancelSynchronousIo(writer.native_handle());
#endif
			});
	}

	u32 number(const char* name)
	{
		const auto text = pbd::env::find_wide(name);
		require(text && *text, "peer_parameter_missing");
		u32 value = 0;
		require(pbd::env::parse_u32_decimal(text, value) && value <= 0x7fff0000u, "peer_parameter_invalid");
		return static_cast<u32>(value);
	}
};
