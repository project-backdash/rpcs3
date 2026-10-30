#pragma once
#include "Emu/PBD/pbd_match_pad.h"

struct pbd::rollback_session::match_standby_steps : boundary_call
{
	const std::filesystem::path& root;
	const session_clock::time_point& started;

	bool run()
	{
		try
		{
			if (!spectator_start_clean() || data.session.start_synced || data.peer.enabled || data.lobby.channel || executable_sha256().empty())
				return end("clean_start_unverified", guard_failed);
			const auto checkpoint = Emu.PBDGetCheckpointBootIdentity();
			const pbd::match_bind::identity expected{data.generation, checkpoint->sha256, executable_sha256(),
				pbd::env::find("PBD_MATCH_REPLAY_RECIPE_SHA256")};
			const bool assign_side = []
			{
				std::lock_guard lock(pad::g_pad_mutex);
				const auto handler = pad::get_pad_thread(true);
				return handler && handler->pbd_match_side_available();
			}();
			vm::temporary_unlock(ppu);
			pbd::held_presentation::output().park(true);
			if (!Emu.Pause(false, false))
				return end("match_standby_pause_failed", guard_failed);
			if (!pbd::spectator_standby::publish(root / "match-standby.json", pbd::match_bind::ready_text(expected, assign_side)))
				return end("match_standby_report_failed", guard_failed);
			const pbd::spectator_standby::ready watch_ready{data.generation, ppu.id, checkpoint->sha256,
				executable_sha256(), expected.recipe, session_runtime.watcher_policy()};
			if (!pbd::spectator_standby::publish(root / pbd::spectator_standby::ready_file, pbd::spectator_standby::ready_text(watch_ready)))
				return end("watch_standby_report_failed", guard_failed);
			pbd_session_log.notice("Warm game: parked at boundary 0 (generation %u).", data.generation);
			pbd::spectator_standby::bind_wait waiting;
			const auto stop = pbd::env::find_wide("PBD_STOP_FILE");
			for (;;)
			{
				if (pbd_emulation_stopping(ppu) || (stop && *stop && std::filesystem::exists(std::filesystem::path(stop))))
					return end("match_standby_cancelled", user_stopped);
				if (std::filesystem::exists(root / "spectator-session.json"))
					return activate_spectator(*checkpoint);
				auto text = pbd::json::detail::read_text(root / "match-bind.json");
				if (text.value)
					return activate_match(*text.value, expected, *checkpoint, assign_side, stop);
				if (!waiting.again(text.error, session_clock::now()))
					return end("match_bind_read_failed", guard_failed);
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
		catch (...)
		{
			return end("match_bind_setup_failed", guard_failed);
		}
	}

	bool wait_activation(const pbd::match_bind::configuration& bound, const pbd::match_bind::identity& expected, const wchar_t* stop)
	{
		if (bound.activate)
		{
			const auto deadline = session_clock::now() + std::chrono::seconds(10);
			pbd::spectator_standby::bind_wait activation;
			for (;;)
			{
				if (pbd_emulation_stopping(ppu) || (stop && *stop && std::filesystem::exists(std::filesystem::path(stop))))
					return end("match_activation_cancelled", user_stopped);
				auto request = pbd::json::detail::read_text(root / "match-activate.json");
				if (request.value)
				{
					if (!pbd::match_bind::activates(*request.value, expected, bound.set_id))
						return end("match_activation_mismatch", guard_failed);
					break;
				}
				if (!activation.again(request.error, session_clock::now()) || session_clock::now() >= deadline)
					return end("match_activation_timeout", guard_failed);
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
		}
		return false;
	}

	bool activate_spectator(const pbd_checkpoint_boot_identity& checkpoint)
	{
		if (std::filesystem::exists(root / "match-bind.json") || !spectator_start_clean())
			return end("watch_bind_conflict", guard_failed);
		auto manifest = pbd::spectator::read_manifest(root);
		if (!manifest.value)
			return end("watch_bind_invalid", guard_failed);
		// No tick has run. The watcher consumes confirmed input and never owns a prediction ring or a fighter's menu.
		data.rolling = false;
		data.correction.active = false;
		data.checkpoint.ring.clear();
		data.game_overlay.enabled = false;
		data.peer.live.enabled = false;
		// The players' held games bind this seed before the first fight frame reseeds the generator; so must the watcher.
		if (!pbd::fn_hook::bind_match_seed(manifest.value->seed))
			return end("watch_seed_bind_failed", guard_failed);
		data.session.seed = manifest.value->seed;
		data.session.seed_set = true;
		data.spectator.saved_vblank = g_cfg.video.vblank_rate.get();
		data.spectator.saved_frame_limit_disabled = g_disable_frame_limit.load();
		data.spectator.saved_skip_draws = pbd::replay_pacing::skip_draws.load(std::memory_order_relaxed);
		data.session.failstop = true;
		if (const char* refusal = session_runtime.accept_spectator(root, std::move(*manifest.value)))
			return end(refusal, guard_failed);
		if (!pbd::held_presentation::output().bound(pbd::held_presentation::purpose::match, false))
			return end("watch_presentation_bind_failed", guard_failed);
		pbd::held_presentation::output().park(false);
		Emu.Resume();
		watch_match_return(root, checkpoint);
		return false;
	}

	bool activate_match(const std::string& text, const pbd::match_bind::identity& expected, const pbd_checkpoint_boot_identity& checkpoint, bool assign_side, const wchar_t* stop)
	{
		auto bound = pbd::match_bind::parse(text, expected);
		if (!bound.value || !spectator_start_clean())
			return end("match_bind_unverified", guard_failed);
		if (!pbd::fn_hook::bind_match_seed(bound.value->seed))
			return end("match_seed_bind_failed", guard_failed);
		data.session.seed = bound.value->seed;
		data.session.seed_set = true;
		if (bound.value->activate)
			data.lobby.match_spectator_from_start = bound.value->spectator_from_start;
		session_runtime.configure_peer(&*bound.value);
		session_runtime.configure_lobby_channel(root, true);
		if (data.match_replay.requested && !data.match_replay.attempted)
			session_runtime.configure_match_replay();
		if (data.lobby.channel->identity().set_id != bound.value->set_id)
			return end("match_bind_set_mismatch", guard_failed);
		// The live port check is delayed along with its assigned side; an absent real device never starts a match.
		if (data.peer.live.enabled)
		{
			bool notice = false;
			const auto result = pbd::match_pad::await_connection([&]
				{
					std::lock_guard lock(pad::g_pad_mutex);
					if (const auto handler = pad::get_pad_thread(true))
					{
						if (assign_side && !handler->pbd_bind_match_side(data.peer.role))
							return false;
						assign_side = false;
						const auto& owned = handler->GetPads()[data.peer.role];
						return owned && owned->is_connected() && !owned->ldd && owned->m_pad_handler != pad_handler::null;
					}
					return false;
				},
				[&]
				{
					return pbd_emulation_stopping(ppu) || (stop && *stop && std::filesystem::exists(std::filesystem::path(stop)));
				},
				[&]
				{
					if (!notice)
					{
						notice = true;
						pbd_session_log.notice("Warm game: waiting for the assigned controller to reconnect.");
						pbd::session_wait::controller_wanted().store(true, std::memory_order_release);
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
				});
			pbd::session_wait::controller_wanted().store(false, std::memory_order_release);
			if (result != pbd::match_pad::connection_result::ready)
				return end(result == pbd::match_pad::connection_result::cancelled ? "match_controller_wait_cancelled" : "match_controller_wait_timeout",
					result == pbd::match_pad::connection_result::cancelled ? user_stopped : setup_failed);
		}
		session_runtime.configure_live_recording();
		if (data.live_stats)
			data.live_pub.configure({pbd::live_stats::mode_session, data.peer.role, data.peer.session,
				data.peer.input_delay, data.peer.max_prediction, data.peer.delay, data.peer.link.relayed});
		require(!data.session.failstop || data.peer.rx.enabled, "session_failstop_requires_receiver_thread");
		session_runtime.start_peer_receiver();
		// Fixed picks hide the checkpoint select until VS; lobby picks must show the select as soon as play resumes.
		if (!pbd::held_presentation::output().bound_match(data.lobby.preselect))
			return end("match_presentation_bind_failed", guard_failed);
		if (!pbd::spectator_standby::publish(root / "match-bound.json", pbd::match_bind::bound_text(expected, bound.value->set_id, &*bound.value)))
			return end("match_bind_report_failed", guard_failed);
		if (wait_activation(*bound.value, expected, stop))
			return true;
		pbd::held_presentation::output().park(false);
		Emu.Resume();
		watch_match_return(root, checkpoint);
		pbd_session_log.notice("Warm game: activated set %s at boundary 0.", bound.value->set_id);
		return false;
	}

	bool end(const char* reason, u32 how)
	{
		const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(session_clock::now() - started).count();
		pbd::spectator_standby::publish(root / "match-standby-end.json", "{\"schema\":\"pbd-match-standby-1\",\"type\":\"ended\",\"reason\":" +
																			 pbd::json::quoted(reason) + ",\"waitedMs\":" + std::to_string(waited) + "}\n");
		session_runtime.finish(how);
		data.end_callback();
		return true;
	}
};
