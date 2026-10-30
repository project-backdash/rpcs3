#include "pbd_test.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

#include "lobby_set_driver_model.h"
#include "Emu/Cell/pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include <fstream>

namespace pbd::test
{
	// Uses the emulator's test mode with an unstarted PPU and real session/channel state; no transport workers run.
	struct peer_wait_fixture
	{
		rollback_session runtime;
		std::unique_ptr<ppu_thread> owner;
		u32 pc = rollback_session::pcs[0];
		bool saved_window_close = session_mode::window_closed_by_player();

		explicit peer_wait_fixture(const std::filesystem::path& directory)
		{
			Emu.SetTestMode();
			struct construction_id
			{
				u32 previous = std::exchange(id_manager::g_id, ppu_thread::id_base);
				~construction_id()
				{
					id_manager::g_id = previous;
				}
			} identity;
			owner = std::make_unique<ppu_thread>(ppu_thread_params{}, "peer-wait-test", 100);
			runtime.m_storage = std::make_unique<rollback_session::storage>();
			auto& data = *runtime.m_storage;
			data.origin = session_clock::now();
			data.input_sequence.resize(2);
			data.extend_session_history();
			data.history.peer_remote_arrival.fill(-1);
			data.peer.enabled = true;
			data.session.enabled = true;
			data.peer.phase_delay = false;
			data.peer.link.auth.retire();
			data.roll_begin = 10;
			data.lobby.channel = std::make_unique<set_channel::channel>(directory, lobby_test_state::identity());
			data.lobby.set = std::make_unique<lobby_set::driver>(*data.lobby.channel,
				[](const std::vector<std::uint8_t>&)
				{
					return std::string(64, 'a');
				});
			runtime.m_active.store(true);
			data.in_callback.store(true);
		}

		~peer_wait_fixture()
		{
			runtime.m_storage->in_callback.store(false);
			runtime.m_storage->peer.rx.stop.store(true);
			runtime = thread_state::finished;
			// An unfinished fixture has no presentation to finalize.
			runtime.m_finalized = true;
			runtime.m_storage.reset();
			session_mode::window_closed.store(saved_window_close);
		}

		std::jthread deadline()
		{
			// Bound a broken cancellation or recovery path without changing the production wait's clock or lease.
			return std::jthread([this](std::stop_token stop)
				{
					const auto until = session_clock::now() + std::chrono::seconds(2);
					while (!stop.stop_requested() && session_clock::now() < until)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					if (!stop.stop_requested())
						owner->state += cpu_flag::exit;
				});
		}

		void end_input_wait()
		{
			auto timeout = deadline();
			auto& data = *runtime.m_storage;
			const u32 index = 1;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			rollback_session::tick_dispatch tick{call, index, data.sample_at(index)};
			bool correction = false;
			check(tick.peer_boundary(correction) && !correction);
			// Input waits return to dispatch_tick, which owns the single callback close.
			check(data.in_callback.load() && data.diagnostic.sequence == 0);
			data.end_callback();
			check_ending();
		}

		void end_start_wait()
		{
			auto timeout = deadline();
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			check(call.wait_start_digest(0));
			check_ending();
		}

		void end_exit_gate()
		{
			auto timeout = deadline();
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			check(call.wait_exit_gate(1, 1, 0));
			check_ending();
		}

		void check_ending()
		{
			auto& data = *runtime.m_storage;
			check(!runtime.m_active.load() && data.reason.load() == session_runtime::lobby_set_ended);
			check(!data.in_callback.load() && data.diagnostic.sequence == 1 && data.last_callback_ns.load());
			check(data.lobby.channel->last_command() == 1 && data.lobby.channel->last_event() == 1);
			check(!data.lobby.set->complete() && !data.lobby.set->sealed_game());
			check(!data.lobby.leave_decided.load() && !data.peer.fault);
			check(data.next_index == 0 && data.stage == 0 && !data.sample_at(0).completed && !data.sample_at(1).completed);
		}

		void recovery_budget()
		{
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			const auto now = session_clock::now();
			const auto from = now - std::chrono::seconds(80);
			const auto from_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(from.time_since_epoch()).count());
			auto& recovery = data.peer.link.recovery;
			const peer_endpoint::endpoint direct{peer_endpoint::family::v4, {10, 0, 0, 2}, 47001};
			recovery.configure({"session-test", "set-test", 42, 7}, direct, from_us);
			recovery.received(7, from_us);
			recovery.poll(from_us + 500000);
			const auto credit = call.peer_pause_credit();
			check(!call.lobby_wait_end() && !call.peer_wait_expired(from, 60, credit));
			check(runtime.m_active.load() && !data.peer.fault && data.in_callback.load());
			recovery.configure({"session-test", "set-test", 42, 8}, direct, from_us - 20'000'000);
			recovery.received(8, from_us - 20'000'000);
			recovery.poll(from_us - 19'500'000);
			check(call.peer_wait_expired(now - std::chrono::seconds(100), 60, credit));
			check(std::string_view(data.peer.fault) == "peer_recovery_timeout");
			check(data.lobby.channel->last_event() == 0 && !data.lobby.set->sealed_game());
		}

		enum class start_control
		{
			ready,
			fault,
			stop,
			recovery
		};

		void start_wait_control(start_control control)
		{
			auto timeout = deadline();
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			if (control == start_control::ready)
			{
				data.session.digests.peer_map_valid = true;
				data.session.digests.peer_options_seq[0] = 0x10000;
				check(!call.wait_start_digest(0) && !data.peer.fault && runtime.m_active.load());
			}
			else if (control == start_control::fault)
			{
				data.peer.fault = "session_options_mismatch";
				check(!call.wait_start_digest(0) && runtime.m_active.load());
				check(std::string_view(data.peer.fault) == "session_options_mismatch");
			}
			else if (control == start_control::stop)
			{
				owner->state += cpu_flag::exit;
				check(call.wait_start_digest(0) && !runtime.m_active.load());
				check(data.reason.load() == session_runtime::emulation_stopped);
			}
			else
			{
				const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
				auto& recovery = data.peer.link.recovery;
				const peer_endpoint::endpoint direct{peer_endpoint::family::v4, {10, 0, 0, 2}, 47001};
				recovery.configure({"session-test", "set-test", 42, 7}, direct, now - 100'000'000);
				recovery.received(7, now - 100'000'000);
				recovery.poll(now - 99'500'000);
				check(!call.wait_start_digest(0) && runtime.m_active.load());
				check(std::string_view(data.peer.fault) == "peer_recovery_timeout");
			}
			check(data.in_callback.load() == (control != start_control::stop) &&
				  data.diagnostic.sequence == (control == start_control::stop ? 1u : 0u));
			check(data.lobby.channel->last_event() == 0 && !data.lobby.set->sealed_game());
			check(data.next_index == 0 && !data.update_active && !data.sample_at(0).completed);
			check(!session_wait::showing().load());
		}

		void end_unacknowledged_start()
		{
			auto timeout = deadline();
			auto& data = *runtime.m_storage;
			rollback_session::boundary_call call{runtime, data, *owner, pc};
			check(call.wait_start_digest(0));
			check(data.reason.load() == session_runtime::lobby_set_ended && !runtime.m_active.load());
			check(!data.in_callback.load() && data.diagnostic.sequence == 1 && !data.lobby.pending_command);
			check(!data.lobby.channel->healthy() && data.lobby.channel->last_command() == 0);
			check(data.lobby.channel->last_event() == 0 && !data.lobby.set->sealed_game());
		}

		void quit_after_drain(bool window_close)
		{
			auto& data = *runtime.m_storage;
			data.session.failstop = true;
			data.in_callback.store(false);
			// Hold the actual writer join past the peer-silence threshold after the abort stops reception.
			data.writer = std::thread([&data]
				{
					while (!data.peer.rx.stop.load(std::memory_order_acquire))
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					std::this_thread::sleep_for(std::chrono::milliseconds(1200));
				});
			const auto heard = session_clock::now() - std::chrono::milliseconds(100);
			data.peer.link.last_valid_us.store(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(heard.time_since_epoch()).count()));
			session_mode::window_closed.store(window_close);
			if (!window_close)
			{
				data.session.pending_end = session_runtime::user_stopped;
				runtime.finish(session_runtime::user_stopped);
			}
			runtime = thread_state::aborting;
			check(data.peer.rx.stop.load() && !runtime.m_active.load() && !runtime.m_finalized);
			check(data.lobby.channel->last_event() == 0 && !data.lobby.leave_decided.load());
			// Facts arriving after the quit must not reclassify its cause or connection eligibility.
			data.peer.link.stop_noticed.store(true);
			session_mode::window_closed.store(!window_close);
			runtime = thread_state::finished;
			check(session_clock::now() - heard > std::chrono::seconds(1));
			check(data.lobby.channel->last_event() == 0 && !data.lobby.leave_decided.load());
			runtime = thread_state::destroying_context;
			check(data.lobby.channel->last_event() == 1 && data.lobby.leave_decided.load());
			check(data.reason.load() == (window_close ? session_runtime::emulation_stopped : session_runtime::user_stopped));
			runtime = thread_state::destroying_context;
			check(data.lobby.channel->last_event() == 1);
		}

		enum class quit_control
		{
			silent,
			peer_end,
			guard,
			invalidation,
			lobby_end,
			waiting
		};

		void neutral_quit(quit_control control)
		{
			auto& data = *runtime.m_storage;
			data.session.failstop = true;
			const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
			data.peer.link.last_valid_us.store(now - (control == quit_control::silent ? 2'000'000 : 100'000));
			data.peer.link.stop_noticed.store(control == quit_control::peer_end);
			session_mode::window_closed.store(false);
			u32 reason = session_runtime::user_stopped;
			if (control == quit_control::guard || control == quit_control::invalidation || control == quit_control::lobby_end)
			{
				reason = control == quit_control::guard        ? session_runtime::guard_failed :
				         control == quit_control::invalidation ? session_runtime::hook_invalidated :
				                                                 session_runtime::lobby_set_ended;
				runtime.finish(reason);
			}
			else if (control == quit_control::waiting)
			{
				rollback_session::boundary_call call{runtime, data, *owner, pc};
				call.leave_session_wait(1);
			}
			runtime.finish(session_runtime::user_stopped);
			// A late packet or a later window close cannot replace the first termination decision.
			data.peer.link.last_valid_us.store(now);
			data.peer.link.stop_noticed.store(false);
			session_mode::note_window_closed();
			data.in_callback.store(false);
			runtime = thread_state::aborting;
			runtime = thread_state::finished;
			runtime = thread_state::destroying_context;
			runtime = thread_state::destroying_context;
			check(data.reason.load() == reason && data.lobby.leave_decided.load());
			check(data.lobby.channel->last_event() == (control == quit_control::waiting ? 1 : 0));
		}
		// Exercise the publication and audio consumers around the actual VS screen.
		void loading_cover_and_mute()
		{
			struct restore_volume
			{
				int configured = static_cast<int>(g_cfg.audio.volume.get());
				int player;
				bool muted;

				restore_volume()
				{
					auto& state = pbd::player_volume::state();
					std::lock_guard lock(state.mutex);
					player = state.player;
					muted = state.muted;
				}
				~restore_volume()
				{
					auto& state = pbd::player_volume::state();
					std::lock_guard lock(state.mutex);
					state.player = player;
					state.muted = muted;
					g_cfg.audio.volume.set(configured);
				}
			} volume;
			auto& data = *runtime.m_storage;
			data.generation = 7;
			pbd::test::check(data.lobby.rematch.arm_start({{12, 32}, 17}, {3, 2, {100, 100}}, data.generation, 0));
			pbd::rollback_session::boundary_call call{runtime, data, *owner, pc};
			call.publish_game_overlay();
			call.lobby_selection_audio();
			pbd::test::check(pbd::game_overlay::load().selection_transition && data.lobby.selection_audio_muted && data.game_overlay.audio_muted);

			for (const u32 scene : {8u, 9u})
			{
				call.publish_set_notices(scene);
				call.lobby_selection_audio();
				pbd::test::check(!data.lobby.vs_seen && pbd::game_overlay::load().selection_transition);
				pbd::test::check(data.lobby.selection_audio_muted && data.game_overlay.audio_muted && g_cfg.audio.volume.get() == 0);
			}
			for (const u32 scene : {10u, 11u, 12u})
			{
				call.publish_set_notices(scene);
				call.lobby_selection_audio();
				pbd::test::check(data.lobby.vs_seen && !pbd::game_overlay::load().selection_transition);
				pbd::test::check(!data.lobby.selection_audio_muted && !data.game_overlay.audio_muted);
			}
		}

		void fast_replay_publication()
		{
			using namespace pbd::replay_viewer;
			struct restore_presentation
			{
				snapshot view = presentation.load();
				bool claimed = pbd::settings_menu::claim.exchange(true);
				~restore_presentation()
				{
					presentation.store(view);
					pbd::settings_menu::claim.store(claimed);
				}
			} saved;
			auto& data = *runtime.m_storage;
			data.replay.controls = data.replay.viewer_enabled = data.replay.fast.active = true;
			data.session.in_match = true;
			pbd::rollback_session::boundary_call call{runtime, data, *owner, pc};
			for (const u32 previous_match : {0u, 2u})
			{
				snapshot previous;
				previous.enabled = previous.fast = 1;
				previous.match = previous_match;
				presentation.store(previous);
				pointer_surface surface;
				std::array<pointer_rectangle, 12> rectangles{};
				rectangles[0] = {100, 600, 200, 30, pointer_exit};
				surface.publish(previous, rectangles, 1);
				pbd::test::check(surface.press(110, 610));
				data.session.matches = previous_match + 1;
				pbd::test::check(!call.apply_replay_controls(0));
				const auto current = presentation.load();
				pbd::test::check(current.match == data.session.matches && current.enabled && current.fast);
				surface.publish(current, rectangles, 1);
				pbd::test::check(surface.take(page::none, pbd::takeover_phase::watching, data.session.matches) == pointer_discard);
				pbd::test::check(surface.press(110, 610));
				pbd::test::check(surface.take(page::none, pbd::takeover_phase::watching, data.session.matches) == pointer_exit);
				pbd::test::check(surface.take(page::none, pbd::takeover_phase::watching, data.session.matches) == pointer_none);
			}
		}
	};
} // namespace pbd::test

PBD_TEST(PbdPeerWait, ProtectedWaitsHonorTheLobbyEnding)
{
	using fixture = pbd::test::peer_wait_fixture;
	for (const auto enter : {&fixture::end_start_wait, &fixture::end_input_wait, &fixture::end_exit_gate})
	{
		lobby_test_state::sample_document test(440);
		{
			std::ofstream file(test.directory / "set-command-1.json", std::ios::binary);
			file << R"({"schema":"pbd-set-contract-1","serial":1,"sessionId":"session-test","setId":"set-test","type":"end_set","reason":"left"})";
			pbd::test::check(file.good());
		}
		pbd::test::peer_wait_fixture wait(test.directory);
		(wait.*enter)();
		pbd::test::check(pbd::set_channel::detail::is(test.read(1).find("type"), "command_applied"));
	}
}

PBD_TEST(PbdPeerWait, LoadingCoverAndMuteEndAtTheVsFrame)
{
	lobby_test_state::sample_document test(447);
	pbd::test::peer_wait_fixture(test.directory).loading_cover_and_mute();
}

PBD_TEST(PbdPeerWait, FastReplayPublicationUsesTheCurrentMatch)
{
	lobby_test_state::sample_document test(448);
	pbd::test::peer_wait_fixture(test.directory).fast_replay_publication();
}

PBD_TEST(PbdPeerWait, StartWaitPreservesReadinessFaultAndCallbackOwnership)
{
	using control = pbd::test::peer_wait_fixture::start_control;
	for (const auto value : {control::ready, control::fault, control::stop, control::recovery})
	{
		lobby_test_state::sample_document test(445);
		pbd::test::peer_wait_fixture(test.directory).start_wait_control(value);
	}
}

PBD_TEST(PbdPeerWait, AnEndingWithAFailedAcknowledgementStillEndsTheStart)
{
	lobby_test_state::sample_document test(446);
	{
		std::ofstream file(test.directory / "set-command-1.json", std::ios::binary);
		file << R"({"schema":"pbd-set-contract-1","serial":1,"sessionId":"session-test","setId":"set-test","type":"end_set","reason":"left"})";
		pbd::test::check(file.good());
	}
	std::filesystem::create_directory(test.directory / "set-event-1.json");
	pbd::test::peer_wait_fixture(test.directory).end_unacknowledged_start();
}

PBD_TEST(PbdPeerWait, AnEmptyLobbyRetainsTheProtectedRecoveryBudget)
{
	lobby_test_state::sample_document test(442);
	pbd::test::peer_wait_fixture(test.directory).recovery_budget();
}

PBD_TEST(PbdPeerWait, QuitEligibilityPrecedesTheShutdownDrain)
{
	for (const bool window_close : {true, false})
	{
		lobby_test_state::sample_document test(443);
		pbd::test::peer_wait_fixture(test.directory).quit_after_drain(window_close);
		const auto event = test.read(1);
		pbd::test::check(pbd::set_channel::detail::is(event.find("type"), "leave"));
		pbd::test::check(pbd::set_channel::detail::is(event.find("action"), "leave_seat"));
		pbd::test::check(pbd::set_channel::detail::is(event.find("cause"), window_close ? "window_closed" : "start_select"));
	}
}

PBD_TEST(PbdPeerWait, ShutdownPreservesNeutralEndDecisions)
{
	using control = pbd::test::peer_wait_fixture::quit_control;
	for (const auto value : {control::silent, control::peer_end, control::guard, control::invalidation, control::lobby_end, control::waiting})
	{
		lobby_test_state::sample_document test(444);
		pbd::test::peer_wait_fixture(test.directory).neutral_quit(value);
		if (value == control::waiting)
			pbd::test::check(pbd::set_channel::detail::is(test.read(1).find("action"), "peer_wait"));
	}
}
