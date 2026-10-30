#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_match_pad.h"
#include "Emu/PBD/pbd_replay_standby.h"

struct pbd::rollback_session::setup_inputs
{
#ifdef _WIN32
	const wchar_t* trace = pbd::env::find_wide("PBD_UPDATE_TRACE");
	const wchar_t* input = pbd::env::find_wide("PBD_UPDATE_TAPE");
	const wchar_t* quiet = pbd::env::find_wide("PBD_PRACTICE_REWIND_QUIET");
#else
	const char* trace = pbd::env::find("PBD_UPDATE_TRACE");
	const char* input = pbd::env::find("PBD_UPDATE_TAPE");
	const char* quiet = pbd::env::find("PBD_PRACTICE_REWIND_QUIET");
#endif
	fs::file input_file;
};

void pbd::rollback_session::configure_output(const setup_inputs& sources)
{
#ifdef _WIN32
	const bool quiet_practice = sources.quiet && *sources.quiet;
	require(!quiet_practice || std::wstring_view(sources.quiet) == L"1", "practice_rewind_quiet_invalid");
#else
	const bool quiet_practice = sources.quiet && *sources.quiet;
	require(!quiet_practice || std::string_view(sources.quiet) == "1", "practice_rewind_quiet_invalid");
#endif
	const bool quiet_spectator = pbd::spectator::requested_directory().has_value() && !(sources.trace && *sources.trace);
	require((sources.trace && *sources.trace) || quiet_practice || quiet_spectator, "missing_PBD_UPDATE_TRACE");
	auto data = std::make_unique<storage>();
	data->practice.rewind_quiet = quiet_practice;
	data->spectator.quiet = quiet_spectator;
	if (!quiet_practice && !quiet_spectator)
	{
		const std::filesystem::path output(sources.trace);
		require(output.is_absolute(), "update_trace_requires_absolute_new_path");
		data->status_directory = output.parent_path();
		const auto output_utf8 = output.u8string();
		require(data->file.open(std::string(output_utf8.begin(), output_utf8.end()), fs::write_new + fs::isfile), "cannot_create_update_trace_exclusively");
	}
	data->generation = static_cast<u64>(Emu.GetEmulationIdentifier());
	pbd::character_rule::configure(data->generation, false);
	data->origin = session_clock::now();
	m_storage = std::move(data);
	require(m_storage->launch.selected != pbd::session_policy::mode::invalid, "session_policy_invalid");
	m_storage->sound_options.configure_environment(m_storage->generation);
}

void pbd::rollback_session::read_input_history(setup_inputs& sources)
{
	require(sources.input && *sources.input, "missing_PBD_UPDATE_TAPE");
	const std::filesystem::path input(sources.input);
	require(input.is_absolute(), "update_tape_requires_absolute_path");
	const auto input_utf8 = input.u8string();
	sources.input_file.open(std::string(input_utf8.begin(), input_utf8.end()), fs::read + fs::isfile);
	require(!!sources.input_file, "cannot_read_update_tape");
	const u64 size = sources.input_file.size();
	require(size <= pbd::update_script_max_bytes, "update_tape_exceeds_64KiB");
	std::string text(static_cast<usz>(size), '\0');
	require(sources.input_file.read(text.data(), text.size()) == size && sources.input_file.size() == size, "update_tape_changed_or_short_read");
	m_storage->input_script_sha256 = hash_hex(text.data(), text.size());
	std::istringstream input_stream(text);
	m_storage->input_sequence = pbd::parse_update_script(input_stream);
	// A held replay is prepared before its file is selected. Cover the file limit and its neutral continuation.
	if (pbd::replay_standby::pbd_requested())
		m_storage->input_sequence.resize(std::max<usz>(m_storage->input_sequence.size(), pbd::replay_standby::pbd_history_extent));
	m_storage->history.samples.resize(m_storage->input_sequence.size());
	m_storage->history.ticks.resize(m_storage->input_sequence.size());
}

#ifdef _WIN32
bool pbd::rollback_session::parse_history_index(const char* name, u32& out, const char* message)
{
	const auto text = pbd::env::find_wide(name);
	if (!text || !*text)
		return false;
	u32 value = 0;
	require(pbd::env::parse_u32_decimal(text, value) && value < m_storage->input_sequence.size(), message);
	out = value;
	return true;
}

void pbd::rollback_session::configure_input_options()
{
	if (parse_history_index("PBD_UPDATE_START_INDEX", m_storage->start_index, "update_start_index_invalid"))
		require(m_storage->start_index >= 1 && !m_storage->capture, "update_start_index_invalid");
	m_storage->inspect_saved_boss = parse_history_index("PBD_JINPACHI_PLAYED_PROBE_INDEX", m_storage->inspection_index, "played_jinpachi_probe_index_invalid");
	require(m_storage->start_index + m_storage->rollback_updates < m_storage->input_sequence.size() &&
				m_storage->capture_index + m_storage->rollback_updates < m_storage->input_sequence.size(),
		"native_rollback_window_invalid");
	m_storage->published.store(m_storage->start_index);
	m_storage->written_through.store(m_storage->start_index);
	m_storage->next_index = m_storage->start_index;
	if (const auto clock = pbd::env::find_wide("PBD_BATTLE_CLOCK"); (clock && *clock) || m_storage->launch.direct())
	{
		require(!clock || std::wstring_view(clock) == L"1" || std::wstring_view(clock) == L"2", "battle_clock_invalid");
		m_storage->battle_clock.version = m_storage->launch.direct() || std::wstring_view(clock) == L"2" ? 2 : 1;
		m_storage->battle_clock.freq = sys_time_get_timebase_frequency();
		require(m_storage->battle_clock.freq != 0, "battle_clock_frequency_invalid");
		m_storage->battle_clock.enabled = true;
	}
}

void pbd::rollback_session::configure_tick_trace()
{
	if (const auto tick_trace = pbd::env::find_wide("PBD_TICK_TRACE"); (tick_trace && *tick_trace) || m_storage->practice.rewind_quiet || m_storage->spectator.quiet)
	{
		// Tick witnesses and enforced hashes remain active without formatting a diagnostic trace.
		const bool tick_output = !m_storage->practice.rewind_quiet && !m_storage->spectator.quiet;
		// Practice and spectator runs have no peer, so the diagnostic effect hash would default on; they switch it off here.
		if (const auto effect = pbd::env::find_wide("PBD_TICK_EFFECT_HASH"); effect && *effect)
			m_storage->checkpoint.effect_hash = std::wstring_view(effect) == L"0" ? 0 : 1;
		const std::filesystem::path tick_path = tick_output ? std::filesystem::path(tick_trace) : std::filesystem::path();
		require(!tick_output || tick_path.is_absolute(), "tick_trace_requires_absolute_new_path");
		const auto rollback = pbd::env::find_wide("PBD_NATIVE_ROLLBACK_STATE");
		// A tick trace excludes the native capture, probe, start index and rollback state; under LLVM its rollback
		// features are admitted only with write capture (checked below).
		require(!m_storage->capture && !m_storage->inspect_checkpoint && !m_storage->start_index && !(rollback && *rollback),
			"tick_boundary_requires_plain_static_run");
		if (tick_output && m_storage->launch.direct())
		{
			m_storage->checkpoint.tick_hold_index = pbd::session_policy::hold_index;
			m_storage->checkpoint.tick_hold_ms = 1;
		}
		else if (tick_output)
			require(parse_history_index("PBD_TICK_HOLD_INDEX", m_storage->checkpoint.tick_hold_index, "tick_hold_index_invalid") &&
						m_storage->checkpoint.tick_hold_index + 1 < m_storage->input_sequence.size(),
				"tick_hold_index_invalid");
		else
			m_storage->checkpoint.tick_hold_index = 0;
		if (const auto ms = pbd::env::find_wide("PBD_TICK_HOLD_MS"); ms && *ms && !m_storage->launch.direct())
		{
			u32 value = 0;
			require(pbd::env::parse_u32_decimal(ms, value) && value >= 1 && value <= 1000, "tick_hold_ms_invalid");
			m_storage->checkpoint.tick_hold_ms = static_cast<u32>(value);
		}
		if (tick_output)
		{
			const auto tick_utf8 = tick_path.u8string();
			require(m_storage->checkpoint.tick_file.open(std::string(tick_utf8.begin(), tick_utf8.end()), fs::write_new + fs::isfile), "cannot_create_tick_trace_exclusively");
		}
		m_storage->checkpoint.tick = true;
		configure_page_set();
		configure_checkpoint_checks();
		configure_initial_state();
		configure_replay_modes();
		configure_session();
		configure_match_replay();
		configure_practice();
		if (const auto pacing = pbd::env::find_wide("PBD_REPLAY_VBLANK"); m_storage->launch.direct() || (pacing && std::wstring_view(pacing) == L"1"))
		{
			// Catch-up presentation pacing for corrections; meaningless without the checkpoint ring.
			require(!m_storage->checkpoint.ring.empty(), "replay_vblank_requires_ring");
			m_storage->replay.vblank = true;
		}
		configure_rolling();
		configure_peer();
		configure_correct_at();
	}
}

#endif

void pbd::rollback_session::validate_dispatch_modes(const std::array<ppu_intrp_func_t, 4>& originals, bool compiled)
{
	if (m_storage->replay.takeover_fast)
	{
		const bool replay_mode = (m_storage->replay.session_replay || pbd_direct_match::requested()) &&
		                         !m_storage->peer.enabled && m_storage->replay.controls;
		const bool practice_mode = m_storage->practice.rewind && m_storage->session.enabled && !m_storage->peer.enabled &&
		                           !m_storage->replay.session_replay && !m_storage->replay.controls;
		require(replay_mode || practice_mode, "fast_takeover_requires_replay_or_practice_session");
		require(m_storage->checkpoint.capture_tick_state && m_storage->fault.capture && m_storage->checkpoint.ring.empty(),
			"fast_takeover_requires_fault_probe_without_ring");
		// Game-owned pages inside the RSX I/O aperture belong to the restored object graph;
		// omitting those spans splits it across restored and live pages.
		require(!m_storage->undo.admit_io_spans.empty(), "fast_takeover_requires_session_undo_admit_spans");
	}
	require(!m_storage->takeover.audit || m_storage->replay.takeover_fast, "takeover_audit_requires_fast_mode");
	// LLVM restores require undo-instrumented stores (PBD_LLVM_UNDO) or write-fault capture because ordinary compiled stores
	// bypass interpreter tracking. Instrumented objects use the undo1- namespace; both decoders share pbd_tick_timebase.
	const bool compiled_capture = compiled && (pbd_llvm_undo_requested() || m_storage->fault.capture);
	require(!compiled || compiled_capture || (m_storage->checkpoint.ring.empty() && !m_storage->correction.active),
		"tick_rollback_requires_static_interpreter_or_compiled_capture");
	// Round trips restore at input dispatch, where compiled code discards cached registers.
	// Identity checks compare restored context; S1/S2 checks compare it after re-execution.
	require(!compiled || compiled_capture || !m_storage->roundtrip.active,
		"compiled_roundtrip_requires_capture");
	// Rolling uses the same restore/resume seam as in-place corrections and requires capture under LLVM.
	// Context differences from concurrent writers do not gate rolling admission.
	require(!compiled || compiled_capture || !m_storage->rolling,
		"compiled_rolling_requires_capture");
	// Peer mode supplies rolling corrections with transport inputs and shares its compiled capture requirement.
	require(!compiled || compiled_capture || !m_storage->peer.enabled,
		"compiled_peer_requires_capture");
	if (!compiled)
		for (auto original : originals)
			require(!!original, "missing_original_dispatch_handler");
}

void pbd::rollback_session::configure_local_modes()
{
	// A replay boots the game fresh, so power-on is not its start state: its checks run once the boot reaches VS select
	// (pbd_replay_boot.cpp wait_for_replay_boot), before any recorded input is applied.
	if (!pbd_direct_match::requested())
	{
		try
		{
			check_start_state();
			m_storage->checkpoint_usable = true;
		}
		catch (const session_error&)
		{
			m_storage->checkpoint_usable = false;
			throw;
		}
	}
	if (m_storage->peer.enabled && m_storage->peer.live.enabled)
	{
		// Live controller: the owned port must be a real, connected device. A Null slot, an SDL device that was not
		// found ("Adding empty device") or a virtual pad would run the session with no human input. Fails closed.
		bool notice = false;
		const auto ready = pbd::match_pad::await_connection([&]
			{
				std::lock_guard lock(pad::g_pad_mutex);
				if (const auto handler = pad::get_pad_thread(true))
				{
					const auto& pad = handler->GetPads()[m_storage->peer.role];
					return pad && pad->is_connected() && !pad->ldd && pad->m_pad_handler != pad_handler::null;
				}
				return false;
			},
			[&]
			{
				return Emu.IsStopped() || (!m_storage->stop_file.empty() && fs::is_file(m_storage->stop_file));
			},
			[&]
			{
				if (!notice)
				{
					notice = true;
					pbd::session_wait::controller_wanted().store(true, std::memory_order_release);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			});
		pbd::session_wait::controller_wanted().store(false, std::memory_order_release);
		require(ready == pbd::match_pad::connection_result::ready,
			ready == pbd::match_pad::connection_result::cancelled ? "match_controller_wait_cancelled" : "match_controller_wait_timeout");
	}
	if (!m_storage->lobby.match_standby)
		configure_live_recording();
	require(m_storage->lobby.direct_match.configure(m_storage->generation,
				m_storage->session.enabled && m_storage->round_lifecycle && !m_storage->peer.enabled && !m_storage->replay.session_replay &&
					!m_storage->practice.rewind && !m_storage->rolling &&
					!m_storage->correction.active && !m_storage->capture && !m_storage->lobby.set &&
					m_storage->start_index == 0,
				m_storage->session.seed),
		"direct_match_configuration_invalid");
	if (const auto enabled = pbd::env::find_wide("PBD_PRACTICE_ROLLING"); enabled && std::wstring_view(enabled) == L"1")
	{
		const auto& s = *m_storage;
		require(s.practice.rewind && s.session.enabled && s.round_lifecycle && !s.peer.enabled && !s.replay.session_replay && !s.replay.controls &&
					!s.rolling && !s.correction.active && !s.roundtrip.active && !s.capture && !s.inspect_checkpoint && !s.checkpoint.late_index &&
					!s.session.mask_start && !s.session.start_skip && s.start_index == 0,
			"practice_rolling_requires_isolated_ordinary_practice");
		m_storage->history.practice_continuous = true;
		// Continuous Practice owns a fixed history ring; finite spectator tapes retain their declared extent.
		m_storage->history.samples.resize(2048);
		m_storage->history.ticks.resize(2048);
	}
}

void pbd::rollback_session::write_setup_headers()
{
	const bool local_replay = m_storage->session.enabled && !m_storage->peer.enabled &&
	                          !m_storage->history.session_continuous &&
	                          (m_storage->replay.session_replay || pbd_direct_match::requested());
	if (local_replay)
		m_storage->extend_session_history(true);
	require(m_storage->input_sequence.size() <= max_updates || m_storage->history.session_history_rolling || local_replay,
		"long_history_requires_peer_session");
	m_storage->initial_validation_passed = true;
	require(write_header(), "header_write_failed");
	if (m_storage->checkpoint.tick && !m_storage->practice.rewind_quiet && !m_storage->spectator.quiet)
	{
		const auto& w = battle_counter_addresses;
		const auto tick_header = fmt::format(
			"{\"type\":\"header\",\"schema\":1,\"source\":\"tick_boundary\",\"game_id\":\"NPUB30009\",\"unit\":\"battle_update_call\","
			"\"update_entry_pc\":%u,\"update_return_pc\":%u,\"boundary_pc\":%u,\"witness_addresses\":[%u,%u,%u,%u,%u],"
			"\"hold_index\":%u,\"hold_ms\":%u,\"hold_scope\":\"owner_ppu_only\",\"emulation_generation\":%u,\"combat_hash\":\"combat-v4-p8c0-libc140\","
			"\"hash_definition\":{\"version\":4,\"id\":\"combat-v4-p8c0-libc140\",\"session\":%s,\"round_lifecycle\":%s}}\n",
			0x15458, 0x50bac, pcs[0], w[0], w[1], w[2], w[3], w[4], m_storage->checkpoint.tick_hold_index, m_storage->checkpoint.tick_hold_ms, m_storage->generation,
			m_storage->session.enabled ? "true" : "false", m_storage->round_lifecycle ? "true" : "false");
		require(m_storage->checkpoint.tick_file.write(tick_header.data(), tick_header.size()) == tick_header.size(), "tick_header_write_failed");
	}
}

void pbd::rollback_session::start_session_workers(const std::array<ppu_intrp_func_t, 4>& originals)
{
	pbd_session_log.notice("Preparing input history for %u guarded updates; continuous peers retain a rolling window.", m_storage->extent());
	prepare_undo_capture();
	for (u32 index = 0; index < originals.size(); index++)
		m_originals[index].store(originals[index]);
	pbd::replay_pacing::credits.store(0);
	pbd::replay_pacing::armed.store(m_storage->replay.vblank ? 1u : 0u);
	pbd::replay_pacing::slew_us.store(0);
	pbd::replay_pacing::markers_missed.store(0);
	pbd::replay_pacing::exact.store(m_storage->correction.withhold_exact && m_storage->correction.replay_skip_draws ? 1u : 0u);
	// PBD_NET_OVERLAY: publish the stats block only when the connection line may show (decided once per process, off by default).
	m_storage->live_stats = pbd::live_stats::publishing();
	if (m_storage->live_stats)
	{
		const auto& live = *m_storage;
		m_storage->live_pub.configure({live.practice.rewind ? pbd::live_stats::mode_practice :
									   live.peer.enabled    ? (live.session.enabled ? pbd::live_stats::mode_session : pbd::live_stats::mode_peer) :
									   live.rolling         ? pbd::live_stats::mode_rolling :
															  pbd::live_stats::mode_straight,
			live.peer.role, live.peer.session,
			live.peer.enabled ? live.peer.input_delay : live.roll_delay, live.peer.max_prediction, live.peer.enabled ? live.peer.delay : live.roll_delay,
			live.peer.enabled && live.peer.link.relayed});
	}
	m_active.store(true);
	m_storage->writer = std::thread([this]
		{
			write_records();
		});
#ifdef _WIN32
	require(!m_storage->peer.enabled || !m_storage->session.enabled || !m_storage->session.failstop || m_storage->peer.rx.enabled, "session_failstop_requires_receiver_thread");
	start_peer_receiver();
#endif
}

void pbd::rollback_session::abort_setup()
{
	if (m_storage)
		write_status("failed", m_storage->setup_failure ? m_storage->setup_failure : "setup_failed");
	m_active.store(false);
	for (auto& original : m_originals)
		original.store(nullptr);
	finish(setup_failed);
	const bool writer_started = m_storage && m_storage->writer.joinable();
	if (m_storage)
	{
		// Dispatch slots were never admitted. Stop any partial worker startup before releasing its shared socket.
		m_storage->peer.rx.stop.store(true, std::memory_order_release);
		if (m_storage->peer.rx.thread.joinable())
			m_storage->peer.rx.thread.join();
		if (writer_started)
			m_storage->writer.join();
#ifdef _WIN32
		m_storage->peer.link.release_transport();
#endif
	}
	if (m_storage && m_storage->file)
	{
		// A failed/partial header is evidence, not an invitation to append a
		// second header to the same file. Never retry an attempted header write.
		if (!m_storage->header_attempted)
			write_header();
		if (m_storage->header_written && !writer_started)
			write_records();
	}
}

bool pbd::rollback_session::configure(const std::array<ppu_intrp_func_t, 4>& originals, const char* dispatch_failure, bool compiled) noexcept
{
	if (!requested())
		return false;
	std::lock_guard setup_lock(m_setup_mutex);
	if (m_stopping || m_storage)
		return false;
	try
	{
		setup_inputs sources;
		configure_output(sources);
		read_input_history(sources);
#ifdef _WIN32
		configure_input_options();
		configure_tick_trace();
#endif
		configure_watch_channels();
		require(!dispatch_failure, dispatch_failure);
		validate_dispatch_modes(originals, compiled);
		configure_local_modes();
		write_setup_headers();
		start_session_workers(originals);
		require(write_status("ready", nullptr), "session_ready_publication_failed");
		return true;
	}
	catch (const session_error& error)
	{
		if (m_storage)
			m_storage->setup_failure = error.message ? error.message : "setup_guard_failed";
		pbd_session_log.error("Disabled: %s", error.message ? error.message : "setup_guard_failed");
	}
	catch (const pbd::update_script_error& error)
	{
		if (m_storage)
			m_storage->setup_failure = error.message;
		pbd_session_log.error("Disabled by update-tape parser: %s", error.message);
	}
	catch (...)
	{
		if (m_storage)
			m_storage->setup_failure = "setup_path_allocation_or_worker_failure";
		pbd_session_log.error("Disabled after update-latch path, allocation, or worker startup failure.");
	}
	abort_setup();
	return false;
}
