#pragma once

struct pbd::rollback_session::peer_poll_steps : boundary_call
{
	const u32& boundary;
	std::array<char, pbd::peer_auth::maximum_bytes> buffer{};
	enum class message_action
	{
		keep,
		ignore,
		stop
	};

	void run()
	{

		// One datagram with its arrival time, from the receive thread's queue (or, with PBD_PEER_RX_THREAD=0,
		// from the socket, stamped now).

		for (;;)
		{
			pbd::peer_endpoint::endpoint from{};
			int got = 0;
			u64 arrival_us = 0;
			{
				got = take(from, arrival_us);
				if (got == -2)
					continue;
				if (got <= 0)
					break; // Nothing pending (WSAEWOULDBLOCK) or the other peer is not bound yet (WSAECONNRESET).
			}
			// Only this thread changes the target (endpoint learning below), so one read serves both source checks.
			const auto target = data.peer.link.target.load();
			using pbd::peer_message::input_check;
			pbd::peer_message::input_message m;
			const u32 first = std::max(data.start_index, boundary >= max_updates ? boundary - (max_updates - 1) : 0u);
			// Character select can schedule more than 16 ticks ahead. A proof may cover the owned inputs already fixed
			// for that lead, even before this peer executes them; the receive bound must not impose a smaller delay cap.
			const u32 lead = data.peer.live.assigned_any && data.peer.live.assigned_through >= boundary ?
			                     data.peer.live.assigned_through - boundary + 1 :
			                     0u;
			const auto status = pbd::peer_message::read_session_input(std::span(reinterpret_cast<const u8*>(buffer.data()), static_cast<usz>(got)),
				data.peer.role, data.peer.session,
				{first, data.extent(), boundary, std::max(lead, static_cast<u32>(data.peer.pending_hash_tick.size())), data.session.segments.number}, m);
			const auto configuration = admit_message(from, target, arrival_us, status);
			if (configuration == message_action::ignore)
				continue;
			if (configuration == message_action::stop)
				break;
			if (!check_message_digests(m))
				break;
			keep_message_segments(m);
			if (!adopt_endpoint(from, target, arrival_us))
				break;
			// A valid message from the (possibly just learned) target pins it; later sources are dropped above.
			data.peer.link.target_confirmed.store(true, std::memory_order_relaxed);
			observe_round_trip(m, arrival_us);
			if (!observe_peer_progress(m))
				break;
			store_inputs(m);
			confirm_state(m);
			if (data.peer.fault)
				break;
		}
	}

	message_action admit_message(const pbd::peer_endpoint::endpoint& from, const pbd::peer_endpoint::endpoint& target, u64 arrival_us, pbd::peer_message::input_check status)
	{
		using pbd::peer_message::input_check;
		// Source first: a datagram from anywhere but the current target is dropped and counted before any of it (button map, gates,
		// digests) is used, so it cannot fault the session, unless learning may adopt
		// it: a well-formed message of this session while nothing valid has arrived from the target yet (or always, by opt-in).
		if (from != target)
		{
			const bool shaped = status == input_check::valid || status == input_check::other_session;
			const bool tagged = status == input_check::valid;
			// Without learning a message of THIS session from another source is still a named fault, not a silent stall.
			if (!data.peer.link.learn_endpoint && tagged && !data.peer.link.recovery.allows(from, data.peer.link.auth.epoch(), arrival_us))
			{
				data.peer.fault = "peer_unexpected_source";
				return message_action::stop;
			}
			// Re-learning is allowed again once the pinned target has sent nothing valid for 500 ms (NAT rebinding, a port the
			// other side's router changed after its first reply). Only an authenticated datagram can adopt that path.
			const bool target_silent = data.peer.link.last_recv_at_us && arrival_us > data.peer.link.last_recv_at_us + 500000;
			const bool staged = tagged && data.peer.link.recovery.allows(from, data.peer.link.auth.epoch(), arrival_us);
			const bool local_route = data.peer.link.recovery.traffic().enabled &&
			                         (pbd::peer_endpoint::is_loopback(from) || pbd::peer_endpoint::is_loopback(target));
			const bool may_learn = staged || (data.peer.link.learn_endpoint && data.peer.link.auth.can_learn() && tagged &&
												 !local_route && (data.peer.link.learn_always || !data.peer.link.target_confirmed.load(std::memory_order_relaxed) || target_silent));
			if (!may_learn)
			{
				// Visible without the peer_end record (a stopped session never writes it): first drop and every 512th.
				if ((data.peer.link.dropped_foreign++ & 511) == 0)
				{
					pbd_session_log.warning("Netplay: dropped a datagram from %s (not the pinned peer; %u dropped so far, %s)",
						pbd::peer_endpoint::text(from), data.peer.link.dropped_foreign,
						!shaped ? "malformed" : tagged ? "right session tag" :
														 "wrong session tag");
				}
				return message_action::ignore;
			}
		}
		if (status == input_check::bad_message)
		{
			data.peer.fault = "peer_bad_message";
			return message_action::stop;
		}
		if (status == input_check::bad_length)
		{
			data.peer.fault = "peer_bad_length";
			return message_action::stop;
		}
		if (status == input_check::bad_confirmed)
		{
			data.peer.fault = "peer_confirmed_tick_invalid";
			return message_action::stop;
		}
		// The session tag is checked before any field of the message is compared or stored. A datagram of an earlier session
		// on this endpoint would otherwise be compared against this session's segment, options and stage digests (and could
		// end it under one of those guard names) and would leave its segments, gates and button map behind. It still ends
		// the session, as peer_session_mismatch, and nothing of it is kept.
		if (status == input_check::other_session)
		{
			data.peer.fault = "peer_session_mismatch";
			return message_action::stop;
		}

		return message_action::keep;
	}

	bool check_message_digests(const pbd::peer_message::input_message& m)
	{
		// The other peer's segment state digest (compared as soon as this peer has its own for the same segment).
		if (const u32 seg = m.digests.state_segment; seg)
		{
			if (seg == 1 && data.session.digests.peer_segment_seg[seg % 8] != seg)
			{
				const auto text = fmt::format("{\"type\":\"peer_setup_digest\",\"digest\":%u}\n", m.digests.state);
				data.peer.file.write(text.data(), text.size());
			}
			data.session.digests.peer_segment_seg[seg % 8] = seg;
			data.session.digests.peer_segment[seg % 8] = m.digests.state;
			if (seg == data.session.digests.segment_seg && data.session.digests.peer_segment[seg % 8] != data.session.digests.segment)
			{
				pbd_session_log.error("Session: segment %u state digest %08x differs from the other peer's %08x.", seg, data.session.digests.segment, data.session.digests.peer_segment[seg % 8]);
				data.peer.fault = pbd::setup_digest::segment_mismatch_guard(seg);
				return false;
			}
		}
		// The other peer's options digest (compared as soon as this peer has its own for the same sequence) and its button map.
		if (const u32 seq = m.digests.options_seq; seq)
		{
			data.session.digests.peer_options_seq[seq % 8] = seq;
			data.session.digests.peer_options[seq % 8] = m.digests.options;
			if (seq == data.session.digests.options_seq && data.session.digests.peer_options[seq % 8] != data.session.digests.options)
			{
				data.peer.fault = "session_options_mismatch";
				return false;
			}
		}
		if (m.digests.map_valid == 1u && !data.session.digests.peer_map_valid)
		{
			data.session.digests.peer_map = m.digests.map;
			data.session.digests.peer_map_valid = true;
		}
		// The other peer's stage digest; compared as soon as this peer has its own for the same match.
		if (const u32 stage_match = m.digests.stage_match; stage_match)
		{
			data.session.digests.peer_stage_match[stage_match % 8] = stage_match;
			data.session.digests.peer_stage[stage_match % 8] = m.digests.stage;
			if (stage_match == data.session.digests.stage_match && data.session.digests.peer_stage[stage_match % 8] != data.session.digests.stage)
			{
				data.peer.fault = "session_stage_mismatch";
				return false;
			}
		}

		return true;
	}

	void keep_message_segments(const pbd::peer_message::input_message& m)
	{
		// The other peer's exit gate (ready index by gated segment; recorded-through index by its current segment).
		if (const u32 gate_seg = m.gate.segment, gate_ready = m.gate.ready; gate_seg && gate_ready)
		{
			data.session.gate.peer_ready_seg[gate_seg % 8] = gate_seg;
			data.session.gate.peer_ready[gate_seg % 8] = gate_ready;
		}
		if (const u32 sender_seg = m.segment.number; sender_seg)
		{
			const u32 through = m.gate.through;
			if (data.session.gate.peer_through_seg[sender_seg % 8] != sender_seg)
			{
				data.session.gate.peer_through_seg[sender_seg % 8] = sender_seg;
				data.session.gate.peer_through[sender_seg % 8] = through;
			}
			else
				data.session.gate.peer_through[sender_seg % 8] = std::max(data.session.gate.peer_through[sender_seg % 8], through);
		}
		// The other peer's session segment and the one before it (first index, scene, counter, rank plan), kept by segment number.
		if (const u32 segment = m.segment.number; segment)
		{
			const auto keep = [&](u32 n, u32 first, u32 scene, u32 counter, u32 rank_stakes)
			{
				auto& slot_n = data.session.segments.peer_number[n % 8];
				if (slot_n == n)
					return;
				slot_n = n;
				data.session.segments.peer_index[n % 8] = first;
				data.session.segments.peer_scene[n % 8] = scene;
				data.session.segments.peer_counter[n % 8] = counter;
				data.session.segments.peer_rank_stakes[n % 8] = rank_stakes;
			};
			keep(segment, m.segment.first_index, m.segment.scene, m.segment.counter, m.segment.rank_stakes);
			if (segment > 1)
				keep(segment - 1, m.segment.prev_first_index, m.segment.prev_scene, m.segment.prev_counter, m.segment.prev_rank_stakes);
			data.session.segments.peer_newest = std::max(data.session.segments.peer_newest, segment);
		}
	}

	bool adopt_endpoint(const pbd::peer_endpoint::endpoint& from, const pbd::peer_endpoint::endpoint& target, u64 arrival_us)
	{
		// A staged route may deliver this same game's input, but only a separate current-context commit changes the target.
		if (data.peer.link.recovery.allows(from, data.peer.link.auth.epoch(), arrival_us))
		{
			data.peer.link.recovery.prove(from, data.peer.link.auth.epoch(), arrival_us);
			return true;
		}
		// A valid message from another address:port is the same peer behind NAT or a VPN; follow it.
		if (from != target)
		{
			if (!data.peer.link.learn_endpoint)
			{
				data.peer.fault = "peer_unexpected_source";
				return false;
			}
			data.peer.link.target.store(from);
			data.peer.link.recovery.learned(from);
			data.peer.link.endpoint_changes++;
			// The address field holds the plain address in either family (IPv6 without brackets); the port is separate.
			const auto text = fmt::format("{\"type\":\"peer_endpoint\",\"boundary\":%u,\"address\":\"%s\",\"port\":%u}\n",
				boundary, pbd::peer_endpoint::address_text(from), pbd::peer_endpoint::host_port(from));
			data.peer.file.write(text.data(), text.size());
		}

		return true;
	}

	void observe_round_trip(const pbd::peer_message::input_message& m, u64 arrival_us)
	{
		// Round trip through the actual path. The peer echoes this peer's newest timestamp and how long it held it.
		{
			// The arrival stamp, not the time of this poll, so round trips and echo ages are those of the path.
			const u64 now_us = arrival_us;
			data.peer.link.last_recv_send_us = m.sync.send_us;
			data.peer.link.last_recv_at_us = now_us;
			const u64 echo_us = m.sync.echo_us, echo_age_us = m.sync.echo_held_us;
			if (echo_us && now_us >= echo_us + echo_age_us)
			{
				const u64 rtt = now_us - echo_us - echo_age_us;
				data.peer.rtt.last_us = rtt;
				data.peer.rtt.min_us = std::min(data.peer.rtt.min_us, rtt);
				data.peer.rtt.max_us = std::max(data.peer.rtt.max_us, rtt);
				data.peer.rtt.sum_us += rtt;
				data.peer.rtt.count++;
				data.peer.rtt.ring[data.peer.rtt.ring_count++ % data.peer.rtt.ring.size()] = static_cast<u32>(std::min<u64>(rtt, 0xffffffffu));
				if (data.live_stats)
					data.live_pub.rtt(rtt);
				if (boundary >= data.peer.rtt.logged + 60)
				{
					data.peer.rtt.logged = boundary;
					const auto text = fmt::format("{\"type\":\"rtt\",\"boundary\":%u,\"rtt_us\":%u}\n", boundary, rtt);
					data.peer.file.write(text.data(), text.size());
				}
			}
		}
	}

	bool observe_peer_progress(const pbd::peer_message::input_message& m)
	{
		// The sender's boundary (frame-advantage estimate) and the announced input delay (role 1 adopts it).
		{
			const u32 remote_boundary = m.sync.boundary;
			if (data.live_stats)
				data.live_pub.remote_boundary(remote_boundary); // datagram loss for the net overlay
			// The FIRST datagram of a boundary is sent at its start; later ones (hold resends, the pass after a
			// correction) carry the same boundary at a later time and must not move the stamp.
			if (remote_boundary > data.peer.sync.remote_boundary || !data.peer.sync.remote_boundary_us)
			{
				data.peer.sync.remote_boundary = remote_boundary;
				data.peer.sync.remote_boundary_us = data.peer.link.last_recv_at_us;
			}
			const u32 announce = m.sync.delay_announce;
			if (data.peer.role == 1 && data.peer.delay_auto && announce && !data.peer.delay_announce)
			{
				if (announce > 7)
				{
					data.peer.fault = "peer_bad_delay_announce";
					return false;
				}
				data.peer.delay_announce = announce;
			}
			data.peer.sync.remote_adv_milli = m.sync.advantage_milli;
			data.peer.sync.remote_adv_valid = m.sync.advantage_valid == 1;
		}

		return true;
	}

	void store_inputs(const pbd::peer_message::input_message& m)
	{
		for (u32 k = 0; k < m.count && !data.peer.fault; k++)
		{
			const auto [t, value] = m.inputs[k];
			if (t >= data.extent())
			{
				data.peer.fault = "peer_bad_tick";
				break;
			}
			if (data.history.session_history_rolling)
			{
				if (!pbd::session_history::buffer<u8, max_updates>::retained(t, data.next_index))
					continue;
				if (std::uint64_t{t} > std::uint64_t{boundary} + max_updates / 2 ||
					!pbd::session_history::buffer<u8, max_updates>::reclaimable(t, data.history_retired()))
				{
					data.peer.fault = "peer_history_window_invalid";
					break;
				}
			}
			if (data.history.peer_remote_arrival[t] < 0)
			{
				if (static_cast<s32>(t) > data.peer.remote_newest)
					data.peer.remote_newest = static_cast<s32>(t);
				data.history.peer_remote_value[t] = value;
				data.history.peer_remote_arrival[t] = static_cast<s32>(boundary);
				data.peer.link.received++;
				if (data.peer.input_trace)
					data.peer.input_trace->push({pbd::peer_input_trace::kind::receive, boundary, t, value});
			}
			else if (data.history.peer_remote_value[t] != value)
				data.peer.fault = "peer_conflicting_duplicate";
			else
				data.peer.link.duplicates++;
		}
	}

	void confirm_state(const pbd::peer_message::input_message& m)
	{
		const s32 confirmed = m.confirmed;
		if (confirmed > data.peer.remote_confirmed || !data.peer.remote_confirmed_us)
		{
			data.peer.remote_confirmed = std::max(data.peer.remote_confirmed, confirmed);
			data.peer.remote_confirmed_us = data.peer.link.last_recv_at_us;
		}
		if (!data.peer.fault && confirmed >= 0 && confirmed > data.peer.hash_checked)
		{
			const std::array<u64, 3>& hashes = m.combat;
			if (confirmed <= data.peer.confirmed)
			{
				bool equal = true;
				const u32 mask = hash_mask(confirmed);
				for (u32 k = 0; k < 3; k++)
					equal = equal && (!((mask >> k) & 1u) || hashes[k] == data.tick_at(confirmed).combat[k]);
				data.peer.hash_checked = confirmed;
				if (data.live_stats)
					data.live_pub.hash_check(confirmed, equal);
				const auto text = fmt::format("{\"type\":\"hash\",\"boundary\":%u,\"tick\":%d,\"equal\":%s}\n", boundary, confirmed, json_bool(equal));
				data.peer.file.write(text.data(), text.size());
				if (!equal)
				{
					const auto& local = data.tick_at(confirmed).combat;
					const auto mismatch_record = fmt::format("{\"type\":\"hash_mismatch\",\"session\":%u,\"role\":%u,\"tick\":%d,\"mask\":%u,"
															 "\"local_hashes\":[\"%016x\",\"%016x\",\"%016x\"],\"peer_hashes\":[\"%016x\",\"%016x\",\"%016x\"],\"cpu_model\":%s}\n",
						data.peer.session, data.peer.role, confirmed, mask, local[0], local[1], local[2], hashes[0], hashes[1], hashes[2],
						pbd::json::quoted(utils::get_cpu_brand()));
					data.peer.file.write(mismatch_record.data(), mismatch_record.size());
					data.peer.fault = "peer_confirmed_state_mismatch";
					write_mismatch_bundle(confirmed);
				}
			}
			else
			{
				// The other peer is ahead; keep its hash (ring by tick) until this peer confirms that tick.
				const u32 slot = static_cast<u32>(confirmed) % data.peer.pending_hash_tick.size();
				data.peer.pending_hash_tick[slot] = static_cast<u32>(confirmed) + 1u; // 0 means empty; receive admission bounded the tick.
				data.peer.pending_hash[slot] = hashes;
			}
		}
	}

	int take(pbd::peer_endpoint::endpoint& source, u64& arrival_us)
	{
		if (data.peer.rx.enabled)
		{
			std::lock_guard lock(data.peer.rx.mutex);
			if (data.peer.rx.queue.empty())
				return 0;
			const auto& packet = data.peer.rx.queue.front();
			if (!data.peer.link.auth.current(packet.auth_epoch))
			{
				data.peer.rx.queue.pop_front();
				return -2;
			}
			const int n = packet.size;
			std::memcpy(buffer.data(), packet.bytes.data(), packet.size);
			source = packet.source;
			arrival_us = packet.arrival_us;
			data.peer.rx.queue.pop_front();
			return n;
		}
		sockaddr_storage from{};
		int from_len = sizeof(from);
		const int n = ::recvfrom(static_cast<SOCKET>(data.peer.link.socket_handle), buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
		arrival_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		source = pbd::peer_endpoint::from_sockaddr(from, from_len);
		if (n <= 0)
			return n;
		const auto authenticated = data.peer.link.auth.receive(std::span(reinterpret_cast<const u8*>(buffer.data()), static_cast<usz>(n)));
		if (!authenticated)
			return -2;
		std::memmove(buffer.data(), authenticated->payload.data(), authenticated->payload.size());
		return static_cast<int>(authenticated->payload.size());
	}
};
