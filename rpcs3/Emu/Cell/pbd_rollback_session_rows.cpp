// The rows the latch writes: the match replay's events and segments, the publication of confirmed rows, the mismatch
// evidence, and the trace header and the writer thread.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_memory_dump_native.h"
#include "Emu/PBD/pbd_spectator_standby.h"
#include <functional>

void pbd::rollback_session::boundary_call::replay_refuse(u32 boundary, const char* reason)
{
	if (!data.match_replay.requested || data.match_replay.refused)
		return;
	data.match_replay.refused = true;
	pbd_session_log.warning("Match replay withheld at boundary %u: %s", boundary, reason);
	if (data.peer.file)
	{
		const auto text = fmt::format("{\"type\":\"replay_refused\",\"match\":1,\"boundary\":%u,\"reason\":\"%s\"}\n", boundary, reason);
		data.peer.file.write(text.data(), text.size());
	}
}

void pbd::rollback_session::boundary_call::replay_events(const pbd::match_recording::events& rows)
{
	using namespace pbd::match_recording;
	if (rows.refused != refusal::none)
	{
		const auto reason = fmt::format("native_lifecycle_%u", static_cast<u32>(rows.refused));
		replay_refuse(data.next_index, reason.c_str());
		return;
	}
	for (u32 i = 0; i < rows.count; ++i)
	{
		const auto& row = rows.values[i];
		std::string text;
		switch (row.type)
		{
		case kind::clean_start:
			text = fmt::format("{\"type\":\"replay_clean_start\",\"match\":%u,\"boundary\":%u,\"counter\":%u,\"clean_loaded\":true,\"seed\":%u,\"recipe_sha256\":\"%s\",\"title_id\":\"NPUB30009\",\"stage_sync\":%s}\n",
				row.match, row.boundary, row.counter, data.session.seed, data.match_replay.recipe_sha256, json_bool(data.match_replay.stage_sync));
			break;
		case kind::vs:
		case kind::intro:
			text = fmt::format("{\"type\":\"replay_presentation\",\"match\":%u,\"boundary\":%u,\"phase\":\"%s\"}\n",
				row.match, row.boundary, row.type == kind::vs ? "vs" : "intro");
			break;
		case kind::round_start:
		case kind::round_fight:
		case kind::round_end:
			text = fmt::format("{\"type\":\"replay_round_%s\",\"match\":%u,\"round\":%u,\"boundary\":%u}\n",
				row.type == kind::round_start ? "start" : row.type == kind::round_fight ? "fight" :
																						  "end",
				row.match, row.round, row.boundary);
			break;
		case kind::match_end:
			text = fmt::format("{\"type\":\"replay_match_end\",\"match\":%u,\"boundary\":%u,\"score_left\":%u,\"score_right\":%u,\"winner\":%s,\"end_reason\":\"completed\"}\n",
				row.match, row.boundary, row.score_left, row.score_right,
				row.outcome == winner::left ? "\"left\"" : row.outcome == winner::right ? "\"right\"" :
																						  "null");
			break;
		}
		if (!text.empty() && (!data.peer.file || data.peer.file.write(text.data(), text.size()) != text.size()))
		{
			replay_refuse(row.boundary, "marker_write_failed");
			return;
		}
	}
}

void pbd::rollback_session::boundary_call::replay_segment(u32 boundary)
{
	if (!data.match_replay.requested || data.match_replay.refused || !data.match_replay.lifecycle.active())
		return;
	// The exit barrier is the exclusive match end, so it cannot open an empty segment of this replay.
	if (data.session.in_match)
		return;
	u32 counter = 0;
	if (!read_word(0x1008f07c, counter) || data.match_replay.segment >= 255)
	{
		replay_refuse(boundary, "segment_metadata_invalid");
		return;
	}
	const auto text = fmt::format("{\"type\":\"replay_segment\",\"match\":1,\"segment\":%u,\"boundary\":%u,\"counter\":%u}\n",
		++data.match_replay.segment, boundary, counter);
	if (!data.peer.file || data.peer.file.write(text.data(), text.size()) != text.size())
		replay_refuse(boundary, "marker_write_failed");
}

void pbd::rollback_session::boundary_call::replay_publish(u32 through)
{
	if (!data.match_replay.requested || data.match_replay.refused || !data.match_replay.lifecycle.active())
		return;
	const u32 stop = data.match_replay.end_pending ? std::min(through, data.match_replay.end.boundary) : through;
	for (; data.match_replay.next < stop && data.match_replay.next < data.history.match_replay_observed.size(); ++data.match_replay.next)
	{
		const u32 next = data.match_replay.next;
		if (data.history.match_replay_observed[next])
			replay_events(data.match_replay.lifecycle.observe(data.history.match_replay_observations[next]));
		else if (data.sample_at(next).entry_seen)
			replay_refuse(next, "confirmed_phase_missing");
		// Alignment gaps never executed an input update; only observed boundaries can carry a native phase.
		if (data.match_replay.refused)
			return;
	}
	if (data.match_replay.end_pending && through >= data.match_replay.end.boundary)
	{
		replay_events(data.match_replay.lifecycle.observe(data.match_replay.end));
		data.match_replay.end_pending = false;
	}
}

void pbd::rollback_session::boundary_call::publish_lobby_through(u32 through)
{
	if (data.lobby.set && !data.lobby.set->publish_through(through, [&](u32 boundary)
							  {
								  return boundary < data.history.samples.size() && data.sample_at(boundary).entry_seen;
							  },
							  [&](u32 boundary)
							  {
								  const auto& row = data.sample_at(boundary);
								  return pbd::lobby_set::input_records{row.raw,
									  {static_cast<u16>(row.ports[0].injected_r3), static_cast<u16>(row.ports[1].injected_r3)},
									  row.completed && !row.failure && row.ports[0].injected && row.ports[0].posted && row.ports[1].injected && row.ports[1].posted};
							  },
							  [&](u32 boundary)
							  {
								  const auto& tick = data.tick_at(boundary);
								  return pbd::lobby_set::sync_facts{boundary, tick.session_segment, tick.frame_counter, tick.scene[0],
									  data.history.match_ticks[boundary] ? 7u : data.session.menu_hash_mask, tick.gate_hold, tick.combat,
									  tick.read && tick.combat_read && tick.generator_read};
							  }))
	{
		// Publication can reject an older confirmed sample after the current tick has completed.
		// Report that immutable sample rather than the speculative guest state at this callback.
		if (const auto rejected = data.lobby.set->last_refusal())
			pbd_session_log.warning("Lobby confirmed lifecycle refused at boundary %u: reason=%u, phase=%u, pending=%u, phase_verified=%u, result_verified=%u, score=%u/%u, outcome=%u, draw_verified=%u",
				rejected->phase.boundary, static_cast<u32>(rejected->reason), static_cast<u32>(rejected->phase.native_phase), rejected->picks.pending_phase,
				rejected->phase.phase_verified, rejected->phase.result_verified, rejected->phase.score_left, rejected->phase.score_right,
				static_cast<u32>(rejected->phase.outcome), rejected->phase.draw_verified);
		session_runtime.finish(guard_failed);
	}
	if (data.lobby.set && data.lobby.channel)
	{
		const auto& identity = data.lobby.channel->identity();
		// Scores come only from the confirmed lifecycle, under the same context lock as the health identity.
		data.peer.link.recovery.observe_progress(identity.session_id, identity.set_id, data.peer.session, data.peer.link.auth.epoch(),
			data.lobby.set->confirmed_progress());
	}
}

void pbd::rollback_session::boundary_call::publish_through(u32 through)
{
	pbd::stall_metrics::scope measure_publish("confirmed_publication", through, 0, true, 200'000);
	const u32 before = data.published.load(std::memory_order_acquire);
	if (through <= before)
	{
		// The natural end can be observed after its input watermark was published by a scene barrier.
		// Consume that terminal observation even when no new confirmed input advances.
		publish_lobby_through(through);
		return;
	}
	if (data.history.practice_continuous)
		for (u32 index = before; index < through; ++index)
		{
			if (!data.history_contains(index))
			{
				session_runtime.finish(output_failed);
				return;
			}
			const auto& row = data.sample_at(index);
			data.history.practice_produced.completed += row.completed;
			data.history.practice_produced.injected += u32{row.ports[0].injected} + u32{row.ports[1].injected};
		}
	// Marker observations from a predicted update are replaced by its corrected pass before this confirmed prefix advances.
	replay_publish(through);
	if (data.match_replay.live.enabled() || data.session.start_skip)
		for (u32 boundary = before; boundary < through; ++boundary)
		{
			if (!data.history_contains(boundary) && data.match_replay.live.enabled())
			{
				data.match_replay.live.close_incomplete("confirmed_history_unavailable");
			}
			if (!data.history_contains(boundary))
				continue;
			const auto& row = data.sample_at(boundary);
			const auto& tick = data.tick_at(boundary);
			if (data.match_replay.live.enabled() && row.entry_seen &&
				(!row.completed || row.failure || !row.ports[0].injected || !row.ports[1].injected))
			{
				data.match_replay.live.close_incomplete("confirmed_input_unavailable");
			}
			const bool executed = row.entry_seen && row.completed && !row.failure && row.ports[0].injected && row.ports[1].injected;
			if (data.match_replay.live.enabled() && !data.match_replay.live.confirmed(boundary, tick.session_segment, row.before_counter,
														{static_cast<u16>(row.ports[0].injected_r3), static_cast<u16>(row.ports[1].injected_r3)}, tick.scene[0], executed,
														{boundary, tick.session_segment, tick.frame_counter, tick.scene[0], data.match_at(boundary) ? 7u : data.session.menu_hash_mask,
															tick.gate_hold, tick.combat, tick.read && tick.combat_read && tick.generator_read}))
			{
				data.match_replay.live.close_incomplete("confirmed_prefix_refused");
			}
			if (executed && data.session.enabled && data.session.start_skip && data.match_at(boundary))
				for (u32 port = 0; port < 2; ++port)
				{
					const auto text = pbd::session_start::describe({boundary, port, row.raw[port], row.ports[port].injected_r3,
						row.start_phase});
					if (text.empty())
						continue;
					const bool masked = !(row.ports[port].injected_r3 & 0x0800);
					(masked ? data.session.start_fight_masked : data.session.start_passed)++;
					if (data.peer.file)
						data.peer.file.write(text.data(), text.size());
				}
		}
	publish_lobby_through(through);
	if (through > data.published.load(std::memory_order_acquire))
	{
		if (data.trace)
			for (u32 index = data.published.load(std::memory_order_relaxed); index < through; ++index)
				data.trace->push({index, data.sample_at(index), data.tick_at(index), data.match_at(index) != 0});
		data.published.store(through, std::memory_order_release);
	}
}

// Write the failing tick's combat regions and the preceding sixteen ticks retained in the ring.
void pbd::rollback_session::boundary_call::write_mismatch_bundle(s32 failing)
{
	// session_state::region_ring. Once per run; the first detected mismatch is the one that matters.
	if (data.session.mismatch_bundle_written || data.session.region_ring.empty() || failing < 0)
		return;
	data.session.mismatch_bundle_written = true;
	const auto path = (data.checkpoint.capture_directory / fmt::format("mismatch-%d.bin", failing)).u8string();
	fs::file out(std::string(path.begin(), path.end()), fs::rewrite);
	if (!out)
		return;
	out.write("DRMM0001", 8);
	const u32 t32 = static_cast<u32>(failing);
	out.write(&t32, 4);
	u32 kept = 0;
	for (s32 t = std::max<s32>(0, failing - 16); t <= failing; t++)
	{
		const auto& slot = data.session.region_ring[static_cast<u32>(t) % 128];
		if (slot.tag != static_cast<u32>(t) + 1)
			continue;
		const u32 u = static_cast<u32>(t);
		out.write(&u, 4);
		out.write(slot.r0.data(), slot.r0.size());
		out.write(slot.r1.data(), slot.r1.size());
		out.write(slot.r2.data(), slot.r2.size());
		kept++;
	}
	pbd_session_log.error("Mismatch bundle written for tick %d: %u ticks of combat-region bytes (%s).", failing, kept, path);
}

u32 pbd::rollback_session::boundary_call::write_memdump(const std::filesystem::path& stem,
	const std::vector<std::pair<u64, u64>>* only)
{
	using pbd::memory_dump::range;
	static constexpr std::array<range, 3> everything{
		{{0x10000, 0x30000000}, {0x40000000, 0x50000000}, {0xD0000000, 0xE0000000}}};
	std::span<const range> ranges = everything;
	std::vector<range> listed;
	if (only)
	{
		// A listed range may end at 0x100000000. Ending it at 0xffffffff keeps the same pages, since none starts there.
		for (const auto& [begin, end] : *only)
			listed.push_back({static_cast<u32>(begin), static_cast<u32>(std::min<u64>(end, 0xffffffff))});
		ranges = listed;
	}
	const auto bin_utf8 = std::filesystem::path(stem.wstring() + L".bin").u8string();
	const fs::file out(std::string(bin_utf8.begin(), bin_utf8.end()), fs::write_new + fs::isfile);
	return out ? pbd::memory_dump::write_file(out, ranges).value_or(0) : 0;
}

// Input source record: what each of the two game ports is, as the emulator actually bound it. "source" is
// where the port's injected value comes from in this run: the pad tape, the live device (owned port in live mode) or
// the network (the other peer's published inputs). Written into the update trace header so a run can never be read as
// human-controlled when it was not.
std::string pbd::rollback_session::initial_state_record() const
{
	const auto& f = initial_state();
	std::string scenes = "[";
	for (usz i = 0; i < f.scenes.size(); i++)
		scenes += fmt::format("%s[%u,%u]", i ? "," : "", f.scenes[i].first, f.scenes[i].second);
	return fmt::format("{\"source\":\"%s\",\"input_counter\":%u,\"battle_counter\":%u,\"player_sha256\":\"%s\",\"scenes\":%s]}",
		f.source, f.input_counter, f.battle_counter, f.player_sha256, scenes);
}

std::string pbd::rollback_session::pad_identity() const
{
	const auto& data = *m_storage;
	const auto escape = [](const std::string& text)
	{
		std::string out = "\"";
		for (const char c : text)
		{
			if (c == '"' || c == '\\')
				out += '\\';
			if (static_cast<unsigned char>(c) < 0x20)
			{
				out += ' ';
				continue;
			}
			out += c;
		}
		return out + "\"";
	};
	std::string pads = "[";
	std::lock_guard lock(pad::g_pad_mutex);
	const auto handler = pad::get_pad_thread(true);
	for (u32 port = 0; port < 2; port++)
	{
		const auto pad = handler ? handler->GetPads()[port] : nullptr;
		const cfg_player* cfg = g_cfg_input.player[port];
		const char* source = !data.peer.enabled ? "tape" : port == data.peer.role ? (data.peer.live.pad_script ? "pad_tape" : data.peer.live.enabled ? "live_device" :
																																					   "tape") :
		                                                                            "network";
		if (port)
			pads += ',';
		pads += fmt::format("{\"port\":%u,\"handler\":%s,\"device\":%s,\"connected\":%s,\"ldd\":%s,\"vid\":%u,\"pid\":%u,\"source\":\"%s\"}",
			port, escape(pad ? fmt::format("%s", pad->m_pad_handler) : std::string("none")), escape(cfg ? cfg->device.to_string() : std::string()),
			json_bool(pad && pad->is_connected()), json_bool(pad && pad->ldd), pad ? u32{pad->m_vendor_id} : 0u, pad ? u32{pad->m_product_id} : 0u, source);
	}
	return pads + ']';
}

// Status is published separately from diagnostic traces and checkpoint/recipe bytes.
bool pbd::rollback_session::write_status(const char* state, const char* reason) noexcept
{
	if (!m_storage || m_storage->status_directory.empty())
		return true;
	try
	{
		const auto& data = *m_storage;
		const auto text = fmt::format("{\"schema\":\"pbd-session-status-1\",\"state\":%s,\"nativeGeneration\":%u,\"reasonCode\":%s,\"checkpointUsable\":%s,\"rawTraceComplete\":%s,\"rawTraceDropped\":%u}\n",
			nullable_quoted(state), data.generation, nullable_quoted(reason), data.checkpoint_usable ? json_bool(*data.checkpoint_usable) : "null",
			json_bool(!data.trace || (!data.trace->disabled() && !data.trace->dropped())), data.trace ? data.trace->dropped() : 0);
		const auto file = data.status_directory / (std::string("session-") + state + ".json");
		if (!pbd::spectator_standby::publish(file, text))
		{
			pbd_session_log.error("Session status could not be published (%s).", state);
			return false;
		}
		return true;
	}
	catch (...)
	{
		pbd_session_log.error("Session status publication failed (%s).", state);
		return false;
	}
}

bool pbd::rollback_session::write_header() noexcept
{
	try
	{
		auto& data = *m_storage;
		data.header_attempted = true;
		if (data.practice.rewind_quiet || data.spectator.quiet)
		{
			data.header_written = true;
			return true;
		}
		const auto header = fmt::format(
			"{\"type\":\"header\",\"schema\":1,\"source\":\"update_latch\",\"game_id\":\"NPUB30009\","
			"\"unit\":\"main_input_update_invocation\",\"ppu_thread\":%u,\"pcs\":[%u,%u,%u,%u],\"opcodes\":[%u,%u,%u,%u],"
			"\"requested_updates\":%s,\"emulation_generation\":%u,\"initial_validation_passed\":%s,\"setup_failure\":%s,"
			"\"tape_sha256\":%s,\"clock\":\"host_steady_ns_since_latch_init\",\"origin_steady_ns\":%s,\"pads\":%s,\"fixture\":%s}\n",
			pbd::t5dr::main_ppu_id, pcs[0], pcs[1], pcs[2], pcs[3], opcodes[0], opcodes[1], opcodes[2], opcodes[3],
			number_or_null(!data.history.practice_continuous && !data.history.session_continuous && !data.input_sequence.empty(), data.extent()), data.generation, json_bool(data.initial_validation_passed),
			nullable_quoted(data.setup_failure), nullable_quoted(data.input_script_sha256.empty() ? nullptr : data.input_script_sha256.c_str()),
			// The origin on the steady clock (QueryPerformanceCounter in ns on Windows), so host_start_ns can be put on the
		    // same axis as the QPC stamps of ordinary-presentation.csv (boundary -> flip latency in absolute terms).
			std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(data.origin.time_since_epoch()).count()), pad_identity(), initial_state_record());
		data.header_written = data.file.write(header.data(), header.size()) == header.size();
		return data.header_written;
	}
	catch (...)
	{
		return false;
	}
}

#include "../PBD/pbd_session_record_writer.h"

void pbd::rollback_session::write_records() noexcept
{
	auto& data = *m_storage;
	try
	{
		record_writer{{.start_index = data.start_index,
						  .extent = data.extent(),
						  .session_enabled = data.session.enabled,
						  .quiet = data.practice.rewind_quiet || data.spectator.quiet,
						  .tick = data.checkpoint.tick,
						  .practice_continuous = data.history.practice_continuous,
						  .history_rolling = data.history.session_history_rolling,
						  .menu_hash_mask = data.session.menu_hash_mask,
						  .directory = data.checkpoint.capture_directory,
						  .origin = data.origin,
						  .samples = data.history.samples,
						  .ticks = data.history.ticks,
						  .matches = data.history.match_ticks,
						  .window = data.history.practice_window,
						  .rewind_prefix = data.history.practice_rewind_prefix,
						  .published = data.published,
						  .written_through = data.written_through,
						  .trace = data.trace.get(),
						  .reason = data.reason,
						  .in_callback = data.in_callback,
						  .last_callback_ns = data.last_callback_ns,
						  .overlay_poll_ns = data.overlay_poll_ns,
						  .network = data.peer.link.recovery,
						  .diagnostic_mutex = data.diagnostic_mutex,
						  .diagnostic = data.diagnostic,
						  .update_active = data.update_active,
						  .end_index = data.replay.end_index,
						  .taken = data.replay.taken,
						  .file = data.file,
						  .tick_file = data.checkpoint.tick_file,
						  .input_trace = data.peer.input_trace.get(),
						  .input_trace_file = data.peer.input_trace_file,
						  .rewind_request = data.takeover.rewind_request,
						  .rewind_ack = data.takeover.rewind_ack,
						  .rewind_to = data.takeover.rewind_to,
						  .rewind_from = data.takeover.rewind_from,
						  .rewind_pass = data.takeover.rewind_pass,
						  .rewind_ns = data.takeover.rewind_ns,
						  .rewind_pages = data.takeover.rewind_pages,
						  .rewind_action = data.takeover.rewind_action},
			[this]
			{
				finish(output_failed);
			},
			[this, &data](u32 reason)
			{
				data.match_replay.live.drain();
				if (data.lobby.channel && !data.lobby.channel->flush())
				{
					reason = output_failed;
					data.reason.store(reason);
				}
				const char* code = reason_text(reason);
				if (reason == setup_failed && data.setup_failure)
					code = data.setup_failure;
				else if (reason == guard_failed)
				{
					if (const char* guard = data.guard_failure.load())
						code = guard;
				}
				write_status("ended", code);
			}}
			.run();
	}
	catch (...)
	{
		finish(output_failed);
		while (m_storage->in_callback.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		write_status("ended", reason_text(output_failed));
		pbd_session_log.error("Update writer failed; injection disabled and output incomplete.");
	}
}
