// The peer link at the boundary: which ticks run in lockstep, which received inputs are usable, the local input that
// is sent, the datagram sent and the datagrams received, and the digests both peers compare.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include "Emu/PBD/pbd_json.h"
#include "util/sysinfo.hpp"

// Menu hashes default to none because character select retains install-specific state until a match loads its records.
u32 pbd::rollback_session::boundary_call::hash_mask(s64 t)
{
	return data.session.enabled && t >= 0 && data.history_contains(static_cast<u32>(t)) && !data.match_at(static_cast<usz>(t)) ? data.session.menu_hash_mask : 7u;
}

bool pbd::rollback_session::boundary_call::phase_lockstep(u32 i)
{
	if (data.session.enabled && data.history_contains(i) && !data.match_at(i))
		return true;
	const bool late = data.round_lifecycle && i > data.last_fight_index + (data.peer.enabled ? data.peer.delay : data.roll_delay);
	// Fight-start prediction mode 2: from the fixture's first actionable tick until the first fight boundary is detected (at most 8 ticks).
	if (late && data.peer.enabled && data.peer.fight_mode == 2 && i >= data.peer.fight_start && i < data.peer.fight_start + 8 && data.last_fight_index < data.peer.fight_start)
		return false;
	return late;
}

// Lockstep entry resolves earlier mispredictions before executing the boundary.
// A correction retains its trigger's decision because last_fight_index moves during re-execution.
bool pbd::rollback_session::boundary_call::all_known_at(u32 now)
{
	return data.roundtrip.repeat_active ? data.correction.all_known : phase_lockstep(now);
}

// Session barrier: the other peer has already entered the NEXT segment; its inputs past that segment's first index (its exit from this
// segment) will never come and count as neutral here (it holds at the barrier; this peer is still loading).
bool pbd::rollback_session::boundary_call::remote_left(u32 t)
{
	if (!data.session.enabled || data.session.segments.peer_newest <= data.session.segments.number)
		return false;
	const u32 next = data.session.segments.number + 1;
	return data.session.segments.peer_number[next % 8] == next && t >= data.session.segments.peer_index[next % 8];
}

bool pbd::rollback_session::boundary_call::peer_usable(u32 t, u32 now)
{
	if (remote_left(t))
		return true;
	if (data.history.peer_remote_arrival[t] < 0)
		return false;
	// PBD_PEER_USE_ARRIVED: what has arrived is the truth and can be executed at once; the
	// confirmation scan, publishing and hashes below keep their delay-based schedule.
	if (data.peer.use_arrived)
		return true;
	return t < data.roll_begin || data.history.lockstep_ticks[t] || now >= t + data.peer.delay || (data.roll_end && now >= data.roll_end + data.peer.delay) ||
	       (t < now && all_known_at(now));
}

u16 pbd::rollback_session::boundary_call::peer_remote_for(u32 t, u32 now)
{
	if (peer_usable(t, now))
		return data.history.peer_remote_value[t];
	for (u32 k = t; k-- > 0;)
		if (peer_usable(k, now))
			return data.history.peer_remote_value[k];
	return 0;
}

// One local admission point. Its result is frozen before queuing/sending/recording; the remote value is never rewritten.
#include "../PBD/pbd_local_input.h"

u16 pbd::rollback_session::boundary_call::filter_local_sample(u16 raw, u32 source)
{
	return local_input_steps{*this, raw, source, raw}.run();
}

// Frame within the current match (0 outside one), counted from its first battle-scene boundary: the coordinate a replay's
// moments are named by (replay_state::fast_state::to_match), identical in the recording and in every replay of it.
u32 pbd::rollback_session::boundary_call::match_frame(u32 index)
{
	if (data.lobby.direct_match.enabled() && !data.lobby.direct_match.pbd_live_inputs() && data.lobby.direct_match.begun())
		return data.lobby.direct_match.pbd_recorded_frame(index);
	return data.session.in_match && index > data.session.match_start ? index - data.session.match_start : 0;
}

u16 pbd::rollback_session::boundary_call::scripted_local(u32 t, u32 boundary)
{
	const u16 raw = data.peer.role == 0 ? data.input_at(t).p1 : data.input_at(t).p2;
	// Start-skip disables the unconditional Start mask, not local admission. The selector, the in-game selection's
	// countdowns and the leave chord still own the sample before it is frozen and sent, exactly as on the live-controller path.
	if (!data.session.mask_start && !data.session.start_skip && !data.lobby.selection &&
		!data.lobby.select_timer)
		return raw;
	if (!data.history.live_fixed[t])
	{
		data.history.live_inputs[t] = filter_local_sample(raw, t);
		data.history.live_fixed[t] = 1;
	}
	return data.history.live_inputs[t];
}

// The entry sample that feeds tick t (taken at boundary t - back), with the button bits of samples a shortened
// delay left without a tick merged in (see peer_state::live_state::entry_consumed_through).
u16 pbd::rollback_session::boundary_call::entry_value(u32 t, u32 back)
{
	if (t < data.start_index + back || !data.history.entry_sampled[t - back])
		return u16{0};
	const u32 source = t - back;
	u16 value = data.history.entry_sample[source];
	if (data.peer.live.entry_consumed_any && source > data.peer.live.entry_consumed_through + 1)
	{
		u16 merged = 0;
		for (u32 k = data.peer.live.entry_consumed_through + 1; k < source; k++)
			if (data.history.entry_sampled[k])
				merged |= data.history.entry_sample[k] & 0x0fff;
		if (merged & ~value)
		{
			data.peer.live.entry_merged++;
			const auto text = fmt::format("{\"type\":\"sample_merge\",\"tick\":%u,\"source\":%u,\"skipped_from\":%u,\"value\":%u,\"merged_bits\":%u}\n",
				t, source, data.peer.live.entry_consumed_through + 1, value, merged & ~value);
			data.peer.file.write(text.data(), text.size());
		}
		value |= merged;
	}
	if (!data.peer.live.entry_consumed_any || source > data.peer.live.entry_consumed_through)
	{
		data.peer.live.entry_consumed_through = source;
		data.peer.live.entry_consumed_any = true;
	}
	return value;
}

void pbd::rollback_session::boundary_call::peer_send(u32 boundary)
{
#ifdef _WIN32
	peer_path_commands();
	pbd::peer_message::input_message m;
	m.role = data.peer.role;
	m.confirmed = data.peer.confirmed;
	if (data.peer.confirmed >= 0)
		m.combat = data.tick_at(data.peer.confirmed).combat;
	// With a scheduled input delay D the owned values of ticks up to boundary + D are already fixed
	// (device reads up to boundary - 1) and are published ahead; the 16-tick window ends at the last such tick.
	append_outgoing_inputs(m, boundary);
	const u64 now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
	const u32 echo_held_us = data.peer.link.last_recv_send_us ? static_cast<u32>(std::min<u64>(now_us - data.peer.link.last_recv_at_us, 0xffffffffu)) : 0u;
	// The boundary this peer is at (frame-advantage estimate on the other side) and, from role 0 once it has decided, the
	// session's input delay + 1 (0 = nothing announced); then this peer's smoothed advantage in milliticks (+ = ahead) and
	// whether it is valid yet (time sync).
	m.sync = {data.peer.session, now_us, data.peer.link.last_recv_send_us, echo_held_us, boundary, data.peer.role == 0 ? data.peer.delay_announce : 0u,
		static_cast<s32>(std::clamp(data.peer.sync.adv_ewma, -1000.0, 1000.0) * 1000.0), data.peer.sync.adv_valid ? 1u : 0u};
	// The session segment this peer has entered (0 = none / not a session) and the one before it, each with its first
	// (local, unaligned) index, scene, counter and rank plan: a peer can pass a one-boundary segment before the other has seen its
	// announcement.
	m.segment = {data.session.segments.number, data.session.segments.index, data.session.segments.scene, data.session.segments.prev_index,
		data.session.segments.prev_scene, data.session.segments.counter, data.session.segments.prev_counter, data.session.segments.rank_stakes,
		data.session.segments.prev_rank_stakes};
	// The exit gate (session_state::gate_state::enabled): the gated segment, its first ready index + 1, and the index below which this
	// peer's readiness tests are all recorded.
	m.gate = {data.session.gate.seg, data.session.gate.ready, data.session.gate.through};
	// The newest match start's stage digest, the newest options digest, the button map of the side this peer plays and the
	// newest segment state digest (each with its match, sequence or segment number; 0 = none yet).
	m.digests = {data.session.digests.stage_match, data.session.digests.stage, data.session.digests.options_seq, data.session.digests.options, data.session.digests.own_map_valid ? 1u : 0u,
		data.session.digests.own_map, data.session.digests.segment_seg, data.session.digests.segment};
	std::array<u8, pbd::peer_message::max_input_bytes> message;
	const usz size = pbd::peer_message::encode_input(m, message);
	if (data.peer.link.auth.retired())
		return;
	std::array<u8, pbd::peer_auth::maximum_bytes> authenticated;
	const auto sent_size = data.peer.link.auth.send(std::span(message).first(size), authenticated);
	if (!sent_size)
	{
		data.peer.fault = "peer_auth_write_failed";
		return;
	}
	const auto epoch = data.peer.link.auth.epoch();
	data.peer.link.recovery.sent(epoch, now_us);
	sockaddr_storage target{};
	const int target_len = pbd::peer_endpoint::to_sockaddr(data.peer.link.target.load(), target);
	::sendto(static_cast<SOCKET>(data.peer.link.socket_handle), reinterpret_cast<const char*>(authenticated.data()), static_cast<int>(sent_size), 0,
		reinterpret_cast<sockaddr*>(&target), target_len);
	data.peer.link.sent++;
	if (const auto candidate = pbd::peer_recovery::resend_target(data.peer.link.recovery.probe(epoch),
			data.peer.link.recovery.previous(epoch, now_us), data.peer.link.path_probe_turn))
	{
		// Separate authenticated sequences let a staged route prove itself even while the old route is faster.
		const auto probe_size = data.peer.link.auth.send(std::span(message).first(size), authenticated);
		if (probe_size)
		{
			const int length = pbd::peer_endpoint::to_sockaddr(*candidate, target);
			::sendto(static_cast<SOCKET>(data.peer.link.socket_handle), reinterpret_cast<const char*>(authenticated.data()), static_cast<int>(probe_size), 0,
				reinterpret_cast<sockaddr*>(&target), length);
			data.peer.link.sent++;
		}
	}
#endif
}

void pbd::rollback_session::boundary_call::peer_path_commands()
{
#ifdef _WIN32
	auto& link = data.peer.link;
	if (!link.path_reader || !data.lobby.channel || !link.auth.can_learn())
		return;
	const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
	if (now < link.path_poll_us)
		return;
	link.path_poll_us = now + 8000;
	const auto& identity = data.lobby.channel->identity();
	const auto epoch = link.auth.epoch();
	if (!link.recovery.matches(identity.session_id, identity.set_id, data.peer.session, epoch))
		link.recovery.configure({identity.session_id, identity.set_id, data.peer.session, epoch}, link.target.load(), now,
			static_cast<u16>(link.local_port));
	const auto next = link.recovery.traffic().command_serial + 1;
	const auto text = link.path_reader->poll(next);
	if (!text.value)
	{
		if (text.error != "unavailable" && text.error != "read_failed")
			link.recovery.refuse(next, "invalid_document");
		return;
	}
	const auto parsed = pbd::peer_recovery::parse(*text.value, next);
	if (!parsed.value)
	{
		link.recovery.refuse(next, "invalid_document");
		return;
	}
	std::optional<pbd::peer_endpoint::endpoint> endpoint;
	if (parsed.value->action == pbd::peer_recovery::operation::stage)
		endpoint = pbd::peer_endpoint::parse_address(parsed.value->address.c_str(), htons(parsed.value->port));
	if (link.recovery.apply(*parsed.value, endpoint, [&link](const pbd::peer_endpoint::endpoint& target)
			{
				link.target.store(target);
			},
			now) &&
		parsed.value->action == pbd::peer_recovery::operation::commit)
	{
		link.endpoint_changes++;
		link.target_confirmed.store(true, std::memory_order_release);
	}
#endif
}

#ifdef _WIN32
#include "../PBD/pbd_peer_poll.h"
#endif

void pbd::rollback_session::boundary_call::peer_poll(u32 boundary)
{
#ifdef _WIN32
	peer_path_commands();
	peer_poll_steps{*this, boundary}.run();
#endif
}

// RANDOM-PANEL SYNC (session_state::random_panel_syncs): in character select (scene 3) set both sides' random-panel
// characters sw[side]+4 (0x10093f5c, 0x10093fd4) to character indices derived from the session seed and the segment number.
// Returns true when both words were written.
bool pbd::rollback_session::boundary_call::sync_random_panel(u32 segment_n)
{
	// The value is a character index 0..0x22; the roulette (0xb0a58) skips locked or unavailable characters at its next step, exactly
	// as it does for the game's own rand() % N draw, so no availability test is needed here.
	u32 scene_word = 0;
	if (!read_word(pbd::t5dr::scene_address, scene_word) || scene_word != 3u)
		return false;
	const u32 base = data.session.seed ^ (segment_n * 0x9e3779b1u) ^ 0x52414e44u;
	const u32 chosen0 = base % 0x23u, chosen1 = (base >> 7) % 0x23u;
	be_t<u32> w0 = chosen0, w1 = chosen1;
	const bool ok = vm::try_access(0x10093f5c, &w0, sizeof(w0), true) && vm::try_access(0x10093fd4, &w1, sizeof(w1), true);
	if (ok && !pbd::character_rule::normalize(ppu, data.generation))
	{
		data.peer.fault = "character_rule_unverified";
		return false;
	}
	if (ok)
		data.session.random_panel_syncs++;
	pbd_session_log.warning("Session: random-panel sync at segment %u: characters %u / %u (%s)", segment_n, chosen0, chosen1, ok ? "ok" : "FAILED");
	return ok;
}

// SEGMENT STATE DIGEST (session_state::digests_state::segment): taken after a segment's syncs; exchanged in every datagram and compared by
// the parser as soon as the other peer's digest for the same segment arrives.
void pbd::rollback_session::boundary_call::take_segment_digest(u32 segment_n)
{
	u32 h = 0x811c9dc5u;
	const auto mix = [&](u32 v)
	{
		h ^= v;
		h *= 0x01000193u;
	};
	for (const u32 address : {0x10093f50u, 0x10093f54u, 0x10093f58u, 0x10093f5cu, 0x10093fd0u, 0x10093fd4u, 0x1008f088u, pbd::t5dr::frame_counter, 0x100934a0u})
	{
		u32 w = 0;
		mix(read_word(address, w) ? w : 0xdeadbeefu);
	}
	const u32 r13 = static_cast<u32>(ppu.gpr[13]);
	u32 seed_word = 0;
	mix(read_word(r13 - pbd::t5dr::random_tls::seed_before_r13, seed_word) ? seed_word : 0xdeadbeefu);
	if (pbd::character_rule::banned(data.generation))
		mix(0x4a494e50u);
	bool setup_complete = true;
	if (segment_n == 0)
	{
		// The session start also covers what each checkpoint took from its PC's game save (pbd_setup_digest.h).
		const auto setup = pbd::setup_digest::digest([](u32 address, std::span<u8> bytes)
			{
				return vm::try_access(address, bytes.data(), static_cast<u32>(bytes.size()), false);
			});
		mix(setup.save);
		mix(setup.record0);
		mix(setup.record1);
		// And the code target both translators compile for: one baseline (jit_compiler::cpu) unless a PC lacks AVX2 or FMA3,
		// which then compiles for itself and must not pair with the baseline.
#ifdef LLVM_AVAILABLE
		const std::string target = jit_compiler::cpu(g_cfg.core.llvm_cpu.to_string());
#else
		const std::string target = "none";
#endif
		u32 target_hash = 0x811c9dc5u;
		for (const char c : target)
			target_hash = (target_hash ^ static_cast<u8>(c)) * 0x01000193u;
		mix(target_hash);
		pbd_session_log.warning("Session: setup digest at the start: save %08x, records %08x / %08x, code target %s%s", setup.save,
			setup.record0, setup.record1, target, setup.complete ? "" : " (incomplete: guest memory unreadable)");
		setup_complete = setup.complete;
	}
	const u32 key = segment_n + 1; // the message treats 0 as "no digest yet", so segment 0 (the session start) is keyed 1
	data.session.digests.segment_seg = key;
	data.session.digests.segment = h;
	if (data.peer.enabled)
	{
		const auto cpu = segment_n == 0 ? ",\"cpu_model\":" + pbd::json::quoted(utils::get_cpu_brand()) : std::string{};
		const auto text = fmt::format("{\"type\":\"session_segment_digest\",\"segment\":%u,\"boundary\":%u,\"digest\":%u,\"session\":%u,\"role\":%u%s}\n",
			segment_n, data.next_index, h, data.peer.session, data.peer.role, cpu);
		data.peer.file.write(text.data(), text.size());
		if (data.session.digests.peer_segment_seg[key % 8] == key)
		{
			if (data.session.digests.peer_segment[key % 8] != h)
			{
				pbd_session_log.error("Session: segment %u state digest %08x differs from the other peer's %08x (their message came first).", segment_n, h, data.session.digests.peer_segment[key % 8]);
				data.peer.fault = pbd::setup_digest::segment_mismatch_guard(key);
			}
		}
	}
	// A start digest that could not read this PC's setup would compare unreadable bytes, so a peer session ends at its
	// start instead. Set last, so it names the cause even when the other PC's digest differs too.
	if (!setup_complete && data.peer.enabled)
	{
		pbd_session_log.error("Session: this PC's setup could not be read in full, so the session cannot start.");
		data.peer.fault = pbd::setup_digest::setup_unreadable_guard;
	}
}

#ifdef _WIN32
void pbd::rollback_session::boundary_call::append_outgoing_inputs(pbd::peer_message::input_message& m, u32 boundary)
{
	u32 last = std::min<u32>(boundary + data.peer.input_delay, data.input_sequence.empty() ? 0 : static_cast<u32>(data.extent()) - 1);
	// Scripted inputs have no future scene classification yet. Admit at the current boundary, then resend the frozen result.
	if (!data.peer.live.enabled && (data.session.mask_start ||
									   (data.lobby.selection && data.lobby.selection->active()) ||
									   (data.lobby.select_timer && data.lobby.select_timer->selecting())))
		last = std::min(last, boundary);
	// Under the phase delay the window ends at the newest tick the push model has fixed (never below the boundary).
	const bool pushed = data.peer.live.enabled && data.peer.phase_delay && data.peer.live.assigned_any;
	if (pushed)
		last = std::min<u32>(std::max(boundary, data.peer.live.assigned_through), data.input_sequence.empty() ? 0 : static_cast<u32>(data.extent()) - 1);
	u32 first = last >= 15 ? last - 15 : 0;
	// The options/map barrier sends transport metadata before boundary 0 has sampled the physical controller, so
	// those packets deliberately contain no owned-input entries (the format permits count == 0).
	const bool bootstrap_pending = data.peer.live.enabled && data.peer.phase_delay && data.peer.live.bootstrap_sample && !data.peer.live.assigned_any;
	// A tick lost in all 16 datagrams that carried it (an outage of about 270 ms) would never be sent
	// again and both peers would hold until the timeout. The other peer reports the newest of this peer's ticks it has confirmed;
	// when that report has not moved for 250 ms and its next tick has already left the window, every other datagram
	// carries the 16 ticks after it instead (the newest ticks keep flowing in the datagrams between).
	{
		const u64 send_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		const s64 needed = static_cast<s64>(data.peer.remote_confirmed) + 1;
		if (data.peer.remote_confirmed_us && send_us - data.peer.remote_confirmed_us > 250000 && needed < static_cast<s64>(first) && (data.peer.resend_toggle ^= 1))
		{
			first = static_cast<u32>(needed);
			last = std::min<u32>(last, first + 15);
		}
	}
	if (bootstrap_pending)
		data.peer.live.bootstrap_withheld++;
	for (u32 t = first; !bootstrap_pending && t <= last && t < data.extent() && m.count < pbd::peer_message::max_inputs; t++)
	{
		// Live controller: publish what the owned port ACTUALLY executed, not what the tape said. Executed ticks
		// hold the live value in samples[t].raw; a future tick's value is the device read of tick t-1-D; without
		// live mode this is the tape.
		u16 value = 0;
		// Session barrier: a tick this peer skipped at an alignment barrier was never dispatched here, so its
		// row holds no executed input; resend the value published for it before the barrier (never a different one: the other peer
		// fails on it with peer_conflicting_duplicate), or 0 if nothing was ever fixed for it (the other side ran it as neutral).
		if (data.session.enabled && pbd::update_source_was_skipped(t, data.next_index, data.sample_at(t).entry_seen))
			value = data.history.live_fixed[t] ? data.history.live_inputs[t] : u16{0};
		else if (!data.peer.live.enabled)
			value = scripted_local(t, boundary);
		else if (t <= boundary)
			value = static_cast<u16>(data.sample_at(t).raw[data.peer.role]);
		else
		{
			// Freeze the published value; the first dispatch of tick t reuses it (live_fixed), so a
			// re-executed earlier tick's fresh device read can never change what the other peer already received.
			if (!data.history.live_fixed[t] && pushed)
				break; // only the push model fixes ticks (the window above already ends there)
			if (!data.history.live_fixed[t])
			{
				const u32 back = (data.peer.live.entry_sample ? 0 : 1) + data.peer.input_delay;
				if (data.peer.live.entry_sample)
					data.history.live_inputs[t] = entry_value(t, back);
				else
					data.history.live_inputs[t] = t >= data.start_index + back ? filter_local_sample(static_cast<u16>(data.sample_at(t - back).ports[data.peer.role].original_r3 & 0xffff), t - back) : u16{0};
				data.history.live_fixed[t] = 1;
			}
			value = data.history.live_inputs[t];
		}
		m.inputs[m.count++] = {t, value};
	}
}
#endif
