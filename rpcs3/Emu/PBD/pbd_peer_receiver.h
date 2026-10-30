#pragma once

struct pbd::rollback_session::peer_receiver
{
	storage& d;
	const SOCKET sock;
	const bool watch_peer;
	u64 last_valid_us = 0;
	u64 key_epoch = 0;
	bool peer_notice_written = false;

	void run()
	{
		while (!d.peer.rx.stop.load(std::memory_order_acquire))
		{
			const u64 now_us = std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count();
			const auto active_epoch = d.peer.link.auth.epoch();
			if (active_epoch != key_epoch || d.peer.link.auth.retired() || d.peer.link.terminal_idle.load(std::memory_order_acquire))
				last_valid_us = 0; // A completed authenticated set is quiescent until its fresh next key arrives.
			key_epoch = active_epoch;
			d.peer.link.recovery.poll(now_us, d.peer.link.auth.retired() || d.peer.link.terminal_idle.load(std::memory_order_acquire));
			const auto recovery = d.peer.link.recovery.traffic();
			const bool protected_match = recovery.enabled && d.peer.link.auth.current(recovery.epoch);
			pbd::session_wait::recovering().store(protected_match && recovery.recovering, std::memory_order_release);
			if (protected_match && d.peer.link.recovery.expired(now_us))
				peer_notice("peer_recovery_timeout", ~0u, ~0u);
			if (watch_peer && !protected_match && last_valid_us && now_us - last_valid_us >= 10000000)
				peer_notice("peer_silence_timeout", ~0u, ~0u);

			fd_set set;
			FD_ZERO(&set);
			FD_SET(sock, &set);
			timeval tv{0, 20000};
			if (::select(0, &set, nullptr, nullptr, &tv) <= 0)
				continue;
			receive_available();
		}
		pbd::session_wait::recovering().store(false, std::memory_order_release);
	}

	void receive_available()
	{
		for (; !d.peer.rx.stop.load(std::memory_order_acquire);)
		{
			rx_packet packet;
			sockaddr_storage from{};
			int from_len = sizeof(from);
			const int n = ::recvfrom(sock, packet.bytes.data(), static_cast<int>(packet.bytes.size()), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
			if (n <= 0)
				break; // Nothing pending (WSAEWOULDBLOCK) or the other peer is not bound yet (WSAECONNRESET).
			const auto authenticated = d.peer.link.auth.receive(std::span(reinterpret_cast<const u8*>(packet.bytes.data()), static_cast<usz>(n)));
			if (!authenticated)
				continue; // No counters, endpoint changes, liveness, faults or diagnostic output for wrong tags or replays.
			packet.auth_epoch = authenticated->epoch;
			std::memmove(packet.bytes.data(), authenticated->payload.data(), authenticated->payload.size());
			packet.arrival_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
			packet.source = pbd::peer_endpoint::from_sockaddr(from, from_len);
			packet.size = static_cast<u16>(authenticated->payload.size());
			std::lock_guard lock(d.peer.rx.mutex);
			if (!d.peer.link.auth.current(packet.auth_epoch))
				continue;
			if (watch_peer)
			{
				const std::span bytes(reinterpret_cast<const u8*>(packet.bytes.data()), packet.size);
				// The current send target (learned or configured) counts; any other source only while learning may
				// still adopt it (before the first valid message from the target, or always by opt-in).
				const auto target = d.peer.link.target.load();
				const bool local_route = d.peer.link.recovery.traffic().enabled &&
				                         (pbd::peer_endpoint::is_loopback(packet.source) || pbd::peer_endpoint::is_loopback(target));
				const bool source_ok = packet.source == target || d.peer.link.recovery.allows(packet.source, packet.auth_epoch, packet.arrival_us) ||
				                       (!local_route && d.peer.link.learn_endpoint && d.peer.link.auth.can_learn() && (d.peer.link.learn_always || !d.peer.link.target_confirmed.load(std::memory_order_relaxed) || (last_valid_us && packet.arrival_us > last_valid_us + 500000)));
				if (pbd::peer_message::is_end_message(bytes))
				{
					if (const auto end = pbd::peer_message::read_end(bytes, d.peer.role, d.peer.session);
						source_ok && end && !d.peer.link.terminal_idle.load(std::memory_order_acquire))
						peer_notice("peer_session_ended", end->reason, end->tick);
					continue; // An end message is never interpreted as inputs.
				}
				if (source_ok &&
					pbd::peer_message::check_input(bytes, d.peer.role, d.peer.session) == pbd::peer_message::input_check::valid)
				{
					last_valid_us = packet.arrival_us;
					d.peer.link.last_valid_us.store(last_valid_us, std::memory_order_release);
					// The echo tells whether this side's own sends under the key have reached the peer.
					d.peer.link.recovery.received(packet.auth_epoch, last_valid_us, pbd::peer_message::input_echo_us(bytes));
				}
			}

			if (d.peer.rx.queue.size() >= 8192)
			{
				d.peer.rx.overflow.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			d.peer.rx.queue.push_back(packet);
		}
	}

	void peer_notice(const char* reason, u32 remote_reason, u32 remote_tick)
	{
		// Set even if the record below cannot be written: a quit after this is never a lobby forfeit (finish).
		d.peer.link.stop_noticed.store(true, std::memory_order_release);
		if (peer_notice_written)
			return;
		const auto text = fmt::format("{\"schema\":1,\"reason\":\"%s\",\"peer_role\":%u,\"session\":%u,\"remote_reason\":%u,\"remote_tick\":%u,\"last_valid_receive_us\":%u}\n",
			reason, 1 - d.peer.role, d.peer.session, remote_reason, remote_tick, last_valid_us);
		const auto path = (d.checkpoint.capture_directory / "session-peer-stop.json").u8string();
		if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
		{
			out.write(text.data(), text.size());
			peer_notice_written = true;
			pbd_session_log.warning("peer stop %s: peer role %u; host fail-stop requested", reason, 1 - d.peer.role);
		}
	}
};
