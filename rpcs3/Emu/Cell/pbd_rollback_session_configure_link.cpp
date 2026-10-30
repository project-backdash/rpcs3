// configure()'s steps for a session and its peer link: the session switches, the peer parameters and socket, and the
// thread that receives the other peer's datagrams.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_held_presentation.h"
#include "Emu/PBD/pbd_input_script_format.h"
#include "Emu/PBD/pbd_match_bind.h"
#include "Emu/PBD/pbd_match_return.h"
#include "Emu/PBD/pbd_spectator_standby.h"
#include "Modules/pbd_input_script_runtime.h"

void pbd_flip_reset_for_boot(); // sys_rsx.cpp

#ifdef _WIN32
#include "../PBD/pbd_session_options.h"
#include "../PBD/pbd_peer_configuration.h"
#include "../PBD/pbd_peer_receiver.h"

namespace
{
	// The old guest is fully torn down before any setting changes. The executable and its private profile stay owned.
	void watch_match_return(std::filesystem::path root, pbd_checkpoint_boot_identity checkpoint)
	{
		std::thread([root = std::move(root), checkpoint = std::move(checkpoint)]
			{
				try
				{
					const auto generation = std::to_string(checkpoint.emulation_generation);
					while (static_cast<u64>(Emu.GetEmulationIdentifier()) == checkpoint.emulation_generation && !Emu.IsStopped())
					{
						auto text = pbd::json::detail::read_text(root / "match-return.json");
						if (text.value)
						{
							auto parsed = pbd::match_return::parse(*text.value, generation, checkpoint.sha256, checkpoint.path);
							if (!parsed.value)
								return;
							Emu.CallFromMainThread([root, checkpoint, generation, request = std::move(*parsed.value)]
								{
									if (static_cast<u64>(Emu.GetEmulationIdentifier()) != checkpoint.emulation_generation ||
										Emu.IsStopped() || Emu.after_kill_callback)
										return;
									pbd::held_presentation::output().park(true);
									Emu.after_kill_callback = [root, generation, request]
									{
										// The next set's seed bind and every reseed count start from a fresh game's values.
										if (!pbd::fn_hook::reset_for_next_game())
										{
											Emu.Quit(true);
											return;
										}
										pbd::replay_pacing::reset_for_boot();
										pbd_flip_reset_for_boot();
										const auto wide = [](const std::string& value)
										{
											return std::filesystem::u8path(value).wstring();
										};
										for (const auto& [key, value] : request.environment)
											if (!pbd::env::set_wide(key, wide(value).c_str()))
											{
												Emu.Quit(true);
												return;
											}
										pbd::held_presentation::output().rearm();
										if (!pbd::spectator_standby::publish(root / "match-released.json",
												"{\"schema\":\"pbd-match-released-1\",\"nativeGeneration\":" + pbd::json::quoted(generation) +
													",\"channel\":" + pbd::json::quoted(request.channel) + "}\n"))
										{
											Emu.Quit(true);
											return;
										}
										Emu.SetForceBoot(true);
										if (Emu.BootGame(request.checkpoint, "", true, cfg_mode::custom_selection, request.config) != game_boot_result::no_errors)
											Emu.Quit(true);
									};
									Emu.Kill(false);
								});
							return;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
					}
				}
				catch (...)
				{
					pbd_session_log.error("Warm game: the return request could not be read.");
				}
			})
			.detach();
	}
} // namespace

#include "../PBD/pbd_match_standby_steps.h"

void pbd::rollback_session::configure_session()
{
	session_configuration{*m_storage}.run();
}

void pbd::rollback_session::configure_peer(const match_bind::configuration* bound)
{
	peer_configuration{*m_storage, bound}.run();
}

void pbd::rollback_session::start_peer_receiver()
{
	if (m_storage->peer.enabled && m_storage->peer.rx.enabled)
	{
		m_storage->peer.rx.thread = std::thread([this]
			{
				auto& d = *m_storage;
				peer_receiver{d, static_cast<SOCKET>(d.peer.link.socket_handle), d.session.enabled && d.session.failstop}.run();
			});
	}
}
#endif

bool pbd::rollback_session::boundary_call::match_standby()
{
#ifdef _WIN32
	if (!data.lobby.match_standby || pc != pcs[0] || data.update_active || data.roundtrip.repeat_active)
		return false;
	const auto root = *std::exchange(data.lobby.match_standby, std::nullopt);
	const auto started = session_clock::now();
	return match_standby_steps{*this, root, started}.run();
#else
	return false;
#endif
}

bool pbd::rollback_session::boundary_call::local_standby()
{
#ifdef _WIN32
	if (data.lobby.local_standby_done || pc != pcs[0] || data.update_active || data.roundtrip.repeat_active)
		return false;
	const char* channel = pbd::env::find("PBD_LOCAL_STANDBY_CHANNEL");
	if (!channel || !*channel)
		return false;
	data.lobby.local_standby_done = true;
	const auto end = [&]
	{
		session_runtime.finish(guard_failed);
		data.end_callback();
		return true;
	};
	try
	{
		const char* token = pbd::env::find("PBD_LOCAL_STANDBY_TOKEN");
		const auto root = std::filesystem::u8path(channel);
		u32 scene = 0, subscene = 0;
		if (!token || std::string_view(token).size() != 36 || !root.is_absolute() || data.peer.enabled || data.lobby.match_standby ||
			(!data.practice.rewind && !data.lobby.direct_match.enabled()) ||
			!read_word(pbd::t5dr::scene_address, scene) || !read_word(pbd::t5dr::subscene_address, subscene) ||
			scene != 3 || subscene != 44 || executable_sha256().empty())
			return end();
		vm::temporary_unlock(ppu);
		pbd::held_presentation::output().park(true);
		if (!Emu.Pause(false, false))
			return end();
		const std::string generation = std::to_string(data.generation);
		if (!pbd::spectator_standby::publish(root / "local-ready.json",
				"{\"schema\":\"pbd-local-standby-1\",\"token\":" + pbd::json::quoted(token) +
					",\"nativeGeneration\":" + pbd::json::quoted(generation) + ",\"emulatorSha256\":" +
					pbd::json::quoted(executable_sha256()) + "}\n"))
			return end();
		pbd_session_log.notice("Local game: parked at character select (generation %u).", data.generation);
		pbd::spectator_standby::bind_wait waiting;
		const char* stop = pbd::env::find("PBD_STOP_FILE");
		for (;;)
		{
			if (pbd_emulation_stopping(ppu) || (stop && *stop && std::filesystem::exists(std::filesystem::u8path(stop))))
				return end();
			auto request = pbd::json::detail::read_text(root / "local-activate.json");
			if (request.value)
			{
				const auto parsed = pbd::json::detail::json_parser(*request.value).parse();
				if (!parsed.value)
					return end();
				const auto& value = *parsed.value;
				if (value.kind != pbd::json::json_kind::object || value.fields.size() != 3 ||
					!pbd::json::detail::is(value.find("schema"), "pbd-local-activate-1") ||
					!pbd::json::detail::is(value.find("token"), token) ||
					!pbd::json::detail::is(value.find("nativeGeneration"), generation))
					return end();
				if (!pbd::held_presentation::output().bound(pbd::held_presentation::purpose::local))
					return end();
				pbd::held_presentation::output().park(false);
				SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
				Emu.Resume();
				pbd_session_log.notice("Local game: activated at character select.");
				return false;
			}
			if (!waiting.again(request.error, session_clock::now()))
				return end();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
	catch (...)
	{
		return end();
	}
#else
	return false;
#endif
}
