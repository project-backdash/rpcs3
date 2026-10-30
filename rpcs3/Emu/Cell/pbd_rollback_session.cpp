#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_held_presentation.h"

pbd::rollback_session::rollback_session() noexcept = default;

pbd::rollback_session::~rollback_session()
{
	*this = thread_state::finished;
	*this = thread_state::destroying_context;
}

bool pbd::rollback_session::requested() noexcept
{
#ifdef _WIN32
	const auto* input_sequence = pbd::env::find_wide("PBD_UPDATE_TAPE");
	const auto* trace = pbd::env::find_wide("PBD_UPDATE_TRACE");
#else
	const auto* input_sequence = pbd::env::find("PBD_UPDATE_TAPE");
	const auto* trace = pbd::env::find("PBD_UPDATE_TRACE");
#endif
	return (input_sequence && *input_sequence) || (trace && *trace);
}

void pbd::rollback_session::finish_lobby_leave()
{
	// Classify the first termination request using its saved peer status, then publish after the channel's owner has drained.
	if (m_storage && m_storage->lobby.set && !m_storage->lobby.leave_decided.exchange(true))
	{
		auto& d = *m_storage;
		auto facts = d.lobby.end;
		// Without the failstop receiver nothing here watches the other PC, so the quit stands as made.
		facts.peer_watched = d.session.failstop && d.peer.rx.enabled;
		if (pbd::lobby_set::leave_on_end(facts))
		{
			const char* cause = facts.window_close                    ? "window_closed" :
			                    d.session.pending_end == user_stopped ? "start_select" :
			                                                            "stop_request";
			if (d.lobby.set->leave_seat(cause))
				pbd_session_log.warning("Lobby set: this player left the set (%s); leave_seat written.", cause);
			else
				pbd_session_log.error("Lobby set: the leave_seat event for %s could not be written.", cause);
		}
		else if (facts.user_stop || facts.window_close)
			pbd_session_log.warning("Lobby set: the session ended by %s after the other PC ended or went silent; no leave_seat (no result).",
				facts.window_close ? "the window's close" : "the player's stop");
	}
}

void pbd::rollback_session::finish_hud()
{
	pbd::replay_viewer::presentation.enabled.store(0, std::memory_order_relaxed);
	pbd::replay_viewer::pointer_ui.clear();
	if (m_storage && m_storage->lobby.selection)
	{
		auto panel = pbd::game_overlay::load();
		if (panel.selection_transition)
		{
			panel.selection_transition = false;
			panel.visible = false;
			pbd::game_overlay::publish(panel);
		}
	}
	g_pbd_tick_undo_enabled.store(false, std::memory_order_release);
	// The two in-window clears are unreachable once m_active is false (observe returns early), so without this a
	// guard failure, match end or emulation stop would leave the replay flag set for the rest of the process.
	g_pbd_replay_timing.store(false, std::memory_order_relaxed);
	g_pbd_replay_frame.store(false, std::memory_order_relaxed);
	pbd::replay_pacing::skip_draws.store(0u, std::memory_order_relaxed);
}

void pbd::rollback_session::write_audio_health()
{
	if (m_storage && !m_storage->checkpoint.capture_directory.empty())
	{
		// cellAudio health totals for the whole run (per-correction deltas are in the correction_end rows).
		const auto text = fmt::format("{\"audio_callbacks\":%u,\"audio_underruns\":%u,\"audio_underrun_bytes\":%u,\"audio_time_advances\":%u,\"audio_silence_blocks\":%u}\n",
			g_pbd_audio_callbacks.load(), g_pbd_audio_underruns.load(), g_pbd_audio_underrun_bytes.load(),
			g_pbd_audio_time_advances.load(), g_pbd_audio_silence_blocks.load());
		const auto path = (m_storage->checkpoint.capture_directory / "audio-health.json").u8string();
		if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
			out.write(text.data(), text.size());
	}
}

void pbd::rollback_session::write_dummy_health()
{
	if (m_storage && m_storage->practice.dummy.enabled && !m_storage->checkpoint.capture_directory.empty())
	{
		const auto& d = *m_storage;
		u32 saved = 0, frames = 0;
		for (const auto& slot : d.practice.dummy.slots)
		{
			saved += !slot.frames.empty();
			frames += static_cast<u32>(slot.frames.size());
		}
		const auto text = fmt::format("{\"schema\":1,\"enabled\":true,\"slots_saved\":%u,\"saved_frames\":%u,\"recordings\":%u,\"plays\":%u,\"cycles\":%u,\"test_complete\":%s,\"rng_source\":\"host_xorshift32\",\"guest_rng_used\":false}\n",
			saved, frames, d.practice.dummy.recordings, d.practice.dummy.plays, d.practice.dummy.cycles, json_bool(d.practice.dummy.test.complete));
		const auto path = (d.checkpoint.capture_directory / "practice-dummy-health.json").u8string();
		if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
			out.write(text.data(), text.size());
	}
}

void pbd::rollback_session::write_input_policy()
{
	if (m_storage && m_storage->session.enabled && !m_storage->checkpoint.capture_directory.empty())
	{
		const auto& d = *m_storage;
		const auto text = fmt::format("{\"mask_start\":%s,\"start_masked\":%u,\"mask_scope\":\"all_session_scenes\",\"start_in_battle_executed\":%u,\"start_skip\":%s,\"start_fight_masked\":%u,\"start_passed\":%u,"
									  "\"select_masked\":%u,\"leave_hold_samples\":%u,\"pending_end\":\"%s\"}\n",
			json_bool(d.session.mask_start), d.session.start_masked, d.session.start_in_battle_executed, json_bool(d.session.start_skip), d.session.start_fight_masked, d.session.start_passed,
			d.session.selects_masked, session_leave_hold_ticks, d.session.pending_end == collecting ? "none" : reason_text(d.session.pending_end));
		const auto path = (d.checkpoint.capture_directory / "session-input-policy.json").u8string();
		if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
			out.write(text.data(), text.size());
	}
}

void pbd::rollback_session::send_session_end(u32 reason)
{
#ifdef _WIN32
	if (m_storage)
	{
		auto& d = *m_storage;
		std::lock_guard socket_lock(d.peer.link.socket_mutex);
		if (d.session.enabled && d.peer.enabled && d.session.failstop && d.peer.link.socket_handle != ~u64{0} && !d.session.end_sent.exchange(true))
		{
			const auto message = pbd::peer_message::encode_end({d.peer.role, d.peer.session, reason, pbd::fn_hook::current_tick.load()});
			std::array<u8, pbd::peer_auth::maximum_bytes> authenticated;
			const auto size = d.peer.link.auth.send(message, authenticated);
			sockaddr_storage target{};
			const int target_len = pbd::peer_endpoint::to_sockaddr(d.peer.link.target.load(), target);
			for (u32 attempt = 0; attempt < 5; attempt++)
				if (size)
					::sendto(static_cast<SOCKET>(d.peer.link.socket_handle), reinterpret_cast<const char*>(authenticated.data()),
						static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&target), target_len);
		}
	}
#endif
}

void pbd::rollback_session::restore_session_presentation()
{
	if (m_storage && m_storage->replay.fast.active)
		end_replay_fast(*m_storage, pbd::fn_hook::current_tick.load());
	if (m_storage && m_storage->spectator.channel)
	{
		if (m_storage->spectator.channel->healthy())
			m_storage->spectator.channel->refuse("native_session_ended");
		rsx::set_native_ui_flip();
		g_cfg.video.vblank_rate.set(m_storage->spectator.saved_vblank);
		g_disable_frame_limit = m_storage->spectator.saved_frame_limit_disabled;
		pbd::replay_pacing::skip_draws.store(m_storage->spectator.saved_skip_draws, std::memory_order_release);
		pbd::replay_pacing::vblank_request.store(static_cast<u32>(m_storage->spectator.saved_vblank), std::memory_order_release);
		pbd::player_volume::restore();
		pbd::corner_chat::clear();
		pbd::vs_cards::hide(m_storage->generation);
	}
	if (m_storage && m_storage->game_overlay.audio_muted)
	{
		pbd::player_volume::restore();
		m_storage->game_overlay.audio_muted = false;
		m_storage->lobby.selection_audio_muted = false;
	}
	if (m_storage && m_storage->lobby.channel)
		pbd::corner_chat::clear();
	if (m_storage && m_storage->lobby.select_timer)
		pbd::select_band::hide(m_storage->generation);
	if (m_storage && m_storage->lobby.channel)
		pbd::vs_cards::hide(m_storage->generation);
	if (m_storage && m_storage->game_overlay.enabled)
		pbd::game_overlay::publish({});
	if (m_storage && m_storage->practice.rewind)
	{
		m_storage->practice.history_pending = false;
		m_storage->practice.record_word_valid = false;
		m_storage->practice.play_before.valid = false;
		m_storage->practice.boundary_counter_valid = false;
		pbd::practice::publish({});
		pbd::practice::input_claim.store(false, std::memory_order_release);
	}
	if (m_storage && m_storage->practice.dummy.audio_muted)
	{
		pbd::player_volume::restore();
		m_storage->practice.dummy.audio_muted = false;
	}
	if (m_storage && m_storage->replay.controls && (m_storage->replay.speed || m_storage->replay.paused))
	{
		g_cfg.video.vblank_rate.set(m_storage->replay.fast.saved_vblank);
		pbd::replay_pacing::vblank_request.store(static_cast<u32>(m_storage->replay.fast.saved_vblank), std::memory_order_release);
		pbd::player_volume::restore();
	}
	if (m_storage && m_storage->fault.capture && m_storage->fault.armed_all)
		fault_disarm_all();
}

void pbd::rollback_session::finish(u32 reason) noexcept
{
	// Host and output threads may request termination while the owner is still running.
	// Close admission before publishing the reason; read owner state only after thread joins.
	m_active.store(false);
	if (reason == guard_failed && m_storage && m_storage->peer.fault)
	{
		const char* missing = nullptr;
		m_storage->guard_failure.compare_exchange_strong(missing, m_storage->peer.fault);
	}
	if (m_storage)
	{
		auto& d = *m_storage;
		pbd::lobby_set::end_facts facts;
		facts.user_stop = reason == user_stopped;
		facts.window_close = reason == emulation_stopped && pbd::session_mode::window_closed_by_player();
		facts.still_collecting = true;
		facts.peer_ended = d.peer.link.stop_noticed.load(std::memory_order_acquire);
		facts.peer_heard_us = d.peer.link.last_valid_us.load(std::memory_order_acquire);
		facts.now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		u32 expected = collecting;
		// Gather only atomic facts before reception can stop. The winning caller owns this snapshot until all callers join.
		if (d.reason.compare_exchange_strong(expected, reason))
			d.lobby.end = facts;
	}
}

void pbd::rollback_session::finalize() noexcept
{
	if (std::exchange(m_finalized, true))
		return;
	const u32 reason = m_storage ? m_storage->reason.load() : emulation_stopped;
	// A checkpoint load may retire a hook before the resident replay starts.
	if (m_storage)
		pbd::held_presentation::output().end();
	finish_lobby_leave();
	finish_hud();
	if (pbd::fn_hook::requested())
		pbd::fn_hook::write_report(m_storage ? m_storage->checkpoint.capture_directory : std::filesystem::path());
	write_audio_health();
	if (m_storage && m_storage->practice.rewind)
		m_storage->practice.inputs.flush();
	write_dummy_health();
	write_input_policy();
	send_session_end(reason);
	restore_session_presentation();
	g_pbd_takeover_anchor_live.store(false, std::memory_order_release);
	pbd::replay_pacing::armed.store(0);
	pbd::replay_pacing::credits.store(0);
	pbd::replay_pacing::pbd_player_rate.store(0, std::memory_order_release);
	pbd::replay_pacing::slew_us.store(0);
	pbd::replay_pacing::exact.store(0);
	if (m_storage)
	{
		m_storage->lobby.direct_match.stop_round_request();
		m_storage->match_replay.live.close_incomplete(reason_text(m_storage->reason.load()));
		if (m_storage->live_stats)
			pbd::live_stats::publish_end(reason_text(m_storage->reason.load()));
	}
}

void pbd::rollback_session::invalidate() noexcept
{
	finish(hook_invalidated);
}

void pbd::rollback_session::operator=(thread_state state) noexcept
{
	if (state == thread_state::aborting || state == thread_state::finished || state == thread_state::destroying_context)
	{
		std::lock_guard setup_lock(m_setup_mutex);
		m_stopping = true;
		finish(emulation_stopped);
		if (state != thread_state::aborting && m_storage && m_storage->writer.joinable())
			m_storage->writer.join();
		if (m_storage)
			m_storage->peer.rx.stop.store(true, std::memory_order_release);
		if (state != thread_state::aborting && m_storage && m_storage->peer.rx.thread.joinable())
			m_storage->peer.rx.thread.join();
#ifdef _WIN32
		if (state != thread_state::aborting && m_storage)
		{
			// An exceptional writer exit may precede the final owner callback.
			while (m_storage->in_callback.load())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif
		// System sends this phase only after every emulation thread has joined.
		// Function hooks can produce rows outside the session callback, so finished alone is insufficient.
		if (state == thread_state::destroying_context)
		{
			finalize();
#ifdef _WIN32
			if (m_storage)
				m_storage->peer.link.release_transport();
#endif
		}
	}
}

void pbd::rollback_session::observe(ppu_thread& ppu, u32 pc) noexcept
{
	if (ppu.id != pbd::t5dr::main_ppu_id || !m_storage)
		return;
	if (pbd::practice::native::in_native_call())
		return;
	auto& data = *m_storage;
	// Injected preference callbacks cannot reenter the input observer while its first-boundary state is pending.
	if (data.sound_options.in_guest_call())
		return;
	// An ended session can never return to independent local play. This scope runs
	// after the correction's temporary CPU/restore leases have unwound. The writer may
	// flush its end marker while the owner is parked; the host watchdog authenticates exit.
	struct session_end_scope_t
	{
		storage& d;
		ppu_thread& ppu;
		~session_end_scope_t()
		{
			if (!d.session.enabled || !d.session.failstop || d.reason.load() == collecting || Emu.IsStopped())
				return;
			pbd_session_log.warning("session ended (%s): gameplay owner parked until host shutdown", reason_text(d.reason.load()));
			d.in_callback.store(false);
			vm::temporary_unlock(ppu);
			while (!(ppu.state & (cpu_flag::exit + cpu_flag::req_exit)) && !Emu.IsStopped())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} session_end_scope{data, ppu};
	if (!m_active.load())
		return;
	data.in_callback.store(true);
	if (!m_active.load())
	{
		data.in_callback.store(false);
		return;
	}
	data.correction.window.scope_start = session_clock::now();
	if (!data.fault.owner_stack_hi)
	{
		data.fault.owner_stack_lo = ppu.stack_addr;
		data.fault.owner_stack_hi = ppu.stack_addr + ppu.stack_size;
	}
	struct hook_scope_t
	{
		storage& d;
		u32 pc;
		~hook_scope_t()
		{
			if (!d.correction.window.open)
				return;
			const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - d.correction.window.scope_start).count();
			d.correction.window.hook_ns += ns;
			for (auto& e : d.correction.window.hook_by_pc)
				if (e[0] == pc || !e[0])
				{
					e[0] = pc;
					e[1] += ns;
					e[2]++;
					break;
				}
		}
	} hook_scope{data, pc};
	data.last_callback_ns.store(data.now_ns() | 1, std::memory_order_relaxed);

	boundary_call{*this, data, ppu, pc}.run();
}

void pbd::rollback_session::configure_live_recording()
{
	const auto trace_environment = pbd::env::find_wide("PBD_TICK_TRACE");
	auto& s = *m_storage;
	std::array<pbd_live_replay::player, 2> players{{{"local-left", "Player 1"}, {"local-right", "Player 2"}}};
	if (s.lobby.channel)
	{
		const auto& identity = s.lobby.channel->identity();
		players = {{{identity.left.member_id, identity.left.name}, {identity.right.member_id, identity.right.name}}};
	}
#ifdef PBD_DIAGNOSTIC_BUILD
	const auto diagnostic = pbd::env::find_wide("PBD_TEST_REPLAY_V2_RECORD_SOURCE");
	const bool checkpoint_source = s.replay.session_replay && !s.peer.enabled && !s.replay.controls && diagnostic &&
	                               std::wstring_view(diagnostic) == L"1";
#else
	constexpr bool checkpoint_source = false;
#endif
	// A LIVE recording needs the run directory, not the optional page-probe capture.
	// Retain the existing journal location when that capture is already enabled.
	const auto recording_root = s.checkpoint.capture_directory.empty() && trace_environment && *trace_environment ?
	                                std::filesystem::path(trace_environment).parent_path() :
	                                s.checkpoint.capture_directory;
	if (!s.match_replay.live.configure(s.generation, recording_root, s.session.enabled, s.round_lifecycle,
			(s.replay.session_replay && !checkpoint_source) || s.practice.rewind || pbd_direct_match::requested(),
			s.session.seed, std::move(players), checkpoint_source))
		pbd_session_log.warning("Replay recording unavailable; gameplay continues.");
}
