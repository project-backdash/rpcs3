// configure()'s steps for the checkpoint, probe, replay, Practice and correction switches, and the checks of the
// start state.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_env_parse.h"
#include "../PBD/pbd_emulator_identity.h"
#include "../PBD/pbd_replay_standby.h"
#include "../PBD/pbd_replay_capture.h"
#include "../PBD/pbd_spectator_standby.h"

void pbd::rollback_session::configure_watch_channels()
{
	if (pbd::replay_standby::pbd_requested())
	{
		const auto root = pbd::spectator::requested_directory();
		require(root && root->is_absolute() && pbd::spectator_standby::requested() && !m_storage->session.seed_set,
			"replay_standby_conflict");
	}
	if (const auto root = pbd::spectator::requested_directory())
		configure_spectator(*root);
	if (const auto root = pbd::set_channel::requested_directory())
		configure_lobby_channel(*root);
}

void pbd::rollback_session::configure_lobby_channel(const std::filesystem::path& root, bool late_bound)
{
	auto& s = *m_storage;
	const auto environment = [](const char* name)
	{
		const auto* value = pbd::env::find(name);
		return std::string(value ? value : "");
	};
	pbd::set_channel::capabilities capabilities;
	capabilities.build = environment("PBD_SET_BUILD");
	capabilities.compat = environment("PBD_SET_COMPAT");
	// The manifest may name any character row and stage id the native fields can hold (0..255).
	for (u32 value = 0; value < 256; ++value)
	{
		capabilities.character_rows.push_back(value);
		capabilities.stage_ids.push_back(value);
	}
	auto manifest = pbd::set_channel::read_manifest(root, capabilities);
	require(manifest.value.has_value(), "lobby_manifest_invalid");
	s.lobby.channel = std::make_unique<pbd::set_channel::channel>(root, std::move(*manifest.value), true);
	pbd::character_rule::configure(s.generation, !s.lobby.channel->identity().allow_jinpachi);
	s.lobby.set = std::make_unique<pbd::lobby_set::driver>(*s.lobby.channel, [](const std::vector<std::uint8_t>& bytes)
		{
			return hash_hex(bytes.data(), bytes.size());
		},
		[&s]
		{
			return s.match_replay.live.pbd_game_start_record(s.generation);
		},
		pbd::emulator_identity::executable_sha256(), [epoch = s.generation](u32 first, u32 end)
		{
			return pbd::fight_timing::samples.take(epoch, first, end);
		});
	const bool own_left = s.lobby.channel->identity().local_side == pbd::set_channel::side::left;
	if (!s.session.enabled || !s.peer.enabled || !s.peer.sync.enabled || !s.session.seed_set || !s.session.seed || !s.game_overlay.enabled ||
		!s.round_lifecycle || !s.session.digests.enabled || s.practice.rewind || s.replay.session_replay || s.peer.role != (own_left ? 0u : 1u))
	{
		s.lobby.set->refuse("lobby_session_unverified", "The lobby requires a synchronized peer session, verified game settings and one shared in-game menu.");
		require(false, "lobby_session_unverified");
	}
	if (!lobby_executable_matches(environment("PBD_SET_EMULATOR_SHA256")))
	{
		s.lobby.set->refuse("emulator_build_mismatch", "The running emulator does not match the admitted lobby build.");
		require(false, "lobby_emulator_build_mismatch");
	}
	// The pre-selection needs both halves: the launcher's request and fixed picks to enter. A fixed set without the
	// request, or the request with an in-game set, refuses at start rather than choosing a fighter for a player.
	const bool fixed_picks = !pbd::set_channel::in_game(s.lobby.channel->identity());
	// A warm game has no match yet at launch. Only its admitted manifest may choose the selection mode at the bind.
	s.lobby.preselect = late_bound ? fixed_picks : environment("PBD_LOBBY_PRESELECT") == "1";
	if (fixed_picks != s.lobby.preselect)
	{
		const char* code = fixed_picks ? "lobby_preselect_required" : "lobby_preselect_conflict";
		s.lobby.set->refuse(code, fixed_picks ? "A set with fixed fighters and stage needs the automatic pre-selection." :
												"The automatic pre-selection needs a set with fixed fighters and stage.");
		require(false, code);
	}
	if (!fixed_picks)
		s.lobby.select_timer.emplace();
	else
	{
		if (!late_bound)
			require(pbd::held_presentation::output().bound(pbd::held_presentation::purpose::match), "cold_match_presentation_bind_failed");
		// Cover the stock select before the first peer handshake, including a warm game's activation wait.
		pbd::game_overlay::publish(pbd::game_overlay::selection_card(s.game_overlay.panel));
	}
	if (s.session.failstop && s.peer.live.enabled && s.peer.link.auth.can_learn())
	{
		s.peer.link.path_reader = std::make_unique<pbd::set_channel::command_reader>([root](u64 serial)
			{
				auto text = pbd::json::detail::read_text(root / ("network-path-" + std::to_string(serial) + ".json"));
				if (text.value && text.value->size() > 4096)
					return pbd::json::result<std::string>{{}, "file_too_large"};
				return text;
			});
		const auto& identity = s.lobby.channel->identity();
		const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		s.peer.link.recovery.configure({identity.session_id, identity.set_id, s.peer.session, s.peer.link.auth.epoch()},
			s.peer.link.target.load(), now, static_cast<u16>(s.peer.link.local_port));
	}
}

void pbd::rollback_session::check_start_state()
{
	std::array<u8, 0x238> code{};
	require(vm::try_access(pcs[0], code.data(), code.size(), false), "input_code_read_failed");
	require(hash_hex(code.data(), code.size()) == "f61a7d328b86c9a858589baf16145389aa1b9ba962e579097b15ad42a840604c", "input_code_hash_mismatch");
	std::array<u8, 0x11a0> players{};
	require(vm::try_access(0x10090040, players.data(), players.size(), false), "initial_player_read_failed");
	const u32 start_index = m_storage->start_index;
	{
		// Log observed and declared start values before admission so a rejected checkpoint remains diagnosable.
		u32 observed_counter = 0;
		std::array<be_t<u32>, 2> observed_scene{};
		std::array<be_t<u32>, 5> observed_counters{};
		const bool c = read_word(counter_address, observed_counter);
		const bool s = vm::try_access(pbd::t5dr::scene_address, observed_scene.data(), sizeof(observed_scene), false);
		const bool w = vm::try_access(battle_counter_addresses[0], observed_counters.data(), 4, false) &&
		               vm::try_access(battle_counter_addresses[1], observed_counters.data() + 1, 4, false);
		pbd_session_log.notice("Fixture start state: input counter %s, battle consumer %s, scene %s/%s, player sha256 %s (declared: %u, %u, %u/%u, %s, source %s)",
			c ? fmt::format("%u", observed_counter) : "unreadable", w ? fmt::format("%u", u32{observed_counters[1]}) : "unreadable",
			s ? fmt::format("%u", u32{observed_scene[0]}) : "?", s ? fmt::format("%u", u32{observed_scene[1]}) : "?",
			hash_hex(players.data(), players.size()), initial_state().input_counter, initial_state().battle_counter,
			initial_state().scenes.front().first, initial_state().scenes.front().second, initial_state().player_sha256, initial_state().source);
		if (m_storage->session.enabled)
		{
			// Each install supplies its own character-select counters and player bytes; a session must start outside battle.
			require(c && s && w, "session_start_state_unreadable");
			require(u32{observed_scene[0]} != 17u && u32{observed_scene[0]} != 16u, "session_start_in_battle");
			auto& f = initial_state();
			f.input_counter = observed_counter - start_index;
			f.battle_counter = observed_counters[1];
			f.player_sha256 = hash_hex(players.data(), players.size());
			f.scenes = {{u32{observed_scene[0]}, u32{observed_scene[1]}}};
			f.source = "session";
		}
	}
	// Test switch: PBD_SYNC_FRAME_COUNTER=<value> writes the game-flow frame counter (0x1008f060 + 0x1c, main-loop
	// frames since power-on, incremented once per frame at 0x108e4) at the first boundary. Every offline match start and round start
	// reseeds the shared generator from it (srand at 0x15a98 in 0x159f0, which also resets the LCGs at 0x1008f0c4 / 0x1008f0d0), so two
	// independently made savestates differ there by construction.
	if (const auto text = pbd::env::find_wide("PBD_SYNC_FRAME_COUNTER"); text && *text)
	{
		u32 before = 0;
		u32 value = 0;
		require(pbd::env::parse_u32(text, value), "sync_frame_counter_invalid");
		require(read_word(0x1008f07c, before), "sync_frame_counter_read_failed");
		be_t<u32> word = value;
		require(vm::try_access(0x1008f07c, &word, sizeof(word), true), "sync_frame_counter_write_failed");
		pbd_session_log.warning("session sync: frame counter 0x1008f07c %u -> %u at the first boundary (PBD_SYNC_FRAME_COUNTER)", before, value);
	}
	if (!start_index)
		require(hash_hex(players.data(), players.size()) == initial_state().player_sha256, "initial_player_hash_mismatch");
#ifdef _WIN32
	else
	{
		// A loaded capture must hold the exact player bytes its capturing
		// generation hashed at that same input entry.
		const auto expected = pbd::env::find_wide("PBD_NATIVE_CAPTURE_PLAYER_SHA256");
		require(expected && *expected, "captured_player_hash_missing");
		const std::wstring_view wide(expected);
		require(std::string(wide.begin(), wide.end()) == hash_hex(players.data(), players.size()), "captured_player_hash_mismatch");
		std::array<be_t<u32>, 2> held{};
		require(vm::try_access(0x10099fb4, held.data(), sizeof(held), false) && held[0] == 0 && held[1] == 0, "resumed_held_masks_nonzero");
	}
#endif
	u32 initial = 0, pointer = 0;
	require(read_word(counter_address, initial) && initial == initial_state().input_counter + start_index, "initial_consumer_mismatch");
	std::array<be_t<u32>, 4> masks{};
	require(vm::try_access(masks_address, masks.data(), sizeof(masks), false) &&
				masks[0] == 0 && masks[1] == 0 && masks[2] == 0 && masks[3] == 0,
		"initial_masks_mismatch");
	require(scene_matches(), "initial_scene_mismatch");
	require(read_word(mapping_pointer, pointer) && pointer == mapping_base, "initial_mapping_pointer_mismatch");
	require(mapping_matches(0) && mapping_matches(1), "initial_mapping_table_mismatch");
}

void pbd::rollback_session::write_undo_policy()
{
	// Record RSX IO mappings and their overlap with checkpoint pages to diagnose undo admission.
	std::string mapped;
	u32 static_mapped = 0;
	const bool render_ready = rsx::get_current_renderer() != nullptr;
	u32 gcm_io = 0, gcm_io_size = 0, gcm_command = 0, gcm_context = 0, rsx_get = 0, rsx_put = 0, rsx_get_ea = 0, rsx_label = 0, rsx_dma = 0;
	if (const auto render = rsx::get_current_renderer(); render && render->ctrl)
	{
		rsx_get = render->ctrl->get.load();
		rsx_put = render->ctrl->put.load();
		rsx_get_ea = render->iomap_table.get_addr(rsx_get);
		rsx_label = render->label_addr;
		rsx_dma = render->dma_address;
	}
	if (const auto gcm = g_fxo->try_get<gcm_config>())
	{
		gcm_io = gcm->current_config.ioAddress;
		gcm_io_size = gcm->current_config.ioSize;
		gcm_command = gcm->gcm_info.command_size;
		gcm_context = gcm->gcm_info.context_addr;
	}
	if (const auto render = rsx::get_current_renderer())
	{
		for (u32 mb = 0; mb < 4096; mb++)
			if (render->iomap_table.io[mb].load() != umax)
				mapped += fmt::format("%s[%u,%u]", mapped.empty() ? "" : ",", mb << 20, render->iomap_table.io[mb].load());
		for (u32 page : m_storage->checkpoint.checkpoint_pages)
			static_mapped += render->iomap_table.io[page >> 20].load() != umax;
	}
	const auto text = fmt::format(
		"{\"type\":\"undo_policy\",\"admit_rsx_io\":%s,\"admit_library\":%s,\"deny_spans\":%u,\"static_pages\":%u,"
		"\"render_ready\":%s,\"static_pages_rsx_io_mapped\":%u,\"gcm_io_address\":%u,\"gcm_io_size\":%u,\"gcm_command_size\":%u,"
		"\"gcm_context_addr\":%u,\"rsx_get\":%u,\"rsx_put\":%u,\"rsx_get_ea\":%u,\"rsx_label_addr\":%u,\"rsx_dma_address\":%u,\"rsx_io_mapped_mb\":[%s]}\n",
		json_bool(m_storage->undo.admit_rsx_io), json_bool(m_storage->undo.admit_library), m_storage->undo.deny.size(),
		m_storage->checkpoint.checkpoint_pages.size(), json_bool(render_ready), static_mapped, gcm_io, gcm_io_size, gcm_command, gcm_context, rsx_get, rsx_put, rsx_get_ea, rsx_label, rsx_dma, mapped);
	m_storage->correction.file.write(text.data(), text.size());
}

void pbd::rollback_session::prepare_undo_capture()
{
	if (m_storage->correction.active || m_storage->replay.takeover_fast)
	{
		if (const auto mode = pbd::env::find_wide("PBD_UNDO_RESTORE"); mode && *mode)
		{
			const std::wstring_view value(mode);
			require(value == L"all" || value == L"unique", "undo_restore_mode_invalid");
			m_storage->correction.undo_restore_unique = value != L"all";
		}
		m_storage->checkpoint.static_page_bits.assign(1u << 14, 0);
		m_storage->undo.page_bits.assign(1u << 14, 0);
		m_storage->undo.classified_page_bits.assign(1u << 14, 0);
		m_storage->undo.refused_page_bits.assign(1u << 14, 0);
		m_storage->undo.denied_page_bits.assign(1u << 14, 0);
		for (u32 page : m_storage->checkpoint.checkpoint_pages)
			m_storage->checkpoint.static_page_bits[page >> 18] |= u64{1} << ((page >> 12) & 63);
		std::memset(g_pbd_undo_skip_bits, 0, sizeof(g_pbd_undo_skip_bits));
		for (u32 page : m_storage->checkpoint.checkpoint_pages)
			g_pbd_undo_skip_bits[page >> 18] |= u64{1} << ((page >> 12) & 63);
		m_storage->undo.max_pages = m_storage->replay.takeover_fast ? 4096 : 512;
		m_storage->undo.marked.reserve(m_storage->undo.max_pages * 2ull); // Saved plus library-skipped pages.
		for (auto& entry : m_storage->checkpoint.ring)
		{
			entry.undo_pages.reserve(m_storage->undo.max_pages);
			entry.undo_pcs.reserve(m_storage->undo.max_pages);
			entry.undo_lrs.reserve(m_storage->undo.max_pages);
			entry.skip_pages.reserve(m_storage->undo.max_pages);
			entry.skip_pcs.reserve(m_storage->undo.max_pages);
			entry.undo_bytes.reserve(m_storage->undo.max_pages * 4096ull);
		}
		m_storage->fault.never_arm_bits.assign(1u << 14, 0);
		m_storage->fault.foreign_page_bits.assign(1u << 14, 0);
		for (u32 page : m_storage->checkpoint.checkpoint_pages)
			m_storage->fault.never_arm_bits[page >> 18] |= u64{1} << ((page >> 12) & 63);
		m_storage->fault.disarmed.reserve(m_storage->undo.max_pages * 4ull);
		for (auto& entry : m_storage->checkpoint.ring)
			entry.foreign_pages.reserve(m_storage->undo.max_pages);
		// Fault mode keeps the host-side span reports on: reservation stores write through the sudo alias and cannot fault.
		g_pbd_tick_undo_enabled.store(true, std::memory_order_release);
		if (m_storage->correction.file)
		{
			write_undo_policy();
		}
	}
}

#ifdef _WIN32
void pbd::rollback_session::configure_checkpoint_checks()
{
	if (const auto roundtrip = pbd::env::find_wide("PBD_TICK_ROUNDTRIP"); roundtrip && std::wstring_view(roundtrip) == L"1")
	{
		require(m_storage->checkpoint.capture_tick_state, "tick_roundtrip_requires_probe");
		m_storage->roundtrip.clock.reserve(64);
		m_storage->roundtrip.active = true;
	}
	if (const auto branch = pbd::env::find_wide("PBD_TICK_BRANCH_RAW"); branch && *branch)
	{
		require(m_storage->roundtrip.active, "tick_branch_requires_roundtrip");
		u32 value = 0;
		require(pbd::env::parse_u32_decimal(branch, value) && value <= 0xffff, "tick_branch_raw_invalid");
		m_storage->roundtrip.branch_raw = static_cast<u16>(value);
		m_storage->roundtrip.branch = true;
	}
	if (const auto dumps = pbd::env::find_wide("PBD_TICK_DUMP_INDEX"); dumps && *dumps)
	{
		require(m_storage->checkpoint.capture_tick_state, "tick_dump_requires_probe");
		auto& indices = m_storage->checkpoint.dump_indices;
		for (std::wstring_view rest(dumps);;)
		{
			const auto comma = rest.find(L',');
			u32 value = 0;
			require(indices.size() < 8 && pbd::env::parse_u32_decimal(rest.substr(0, comma), value) && value > 0 &&
						value < m_storage->input_sequence.size() && std::find(indices.begin(), indices.end(), value) == indices.end(),
				"tick_dump_index_invalid");
			indices.push_back(value);
			if (comma == std::wstring_view::npos)
				break;
			rest.remove_prefix(comma + 1);
		}
	}
	if (const auto late = pbd::env::find_wide("PBD_TICK_LATE_INDEX"); late && *late)
	{
		require(m_storage->checkpoint.capture_tick_state, "tick_late_probe_requires_probe");
		u32 value = 0;
		require(pbd::env::parse_u32_decimal(late, value) && value > m_storage->checkpoint.tick_hold_index + 1 && value < m_storage->input_sequence.size(),
			"tick_late_index_invalid");
		m_storage->checkpoint.late_index = value;
	}
	if (const auto ring = pbd::env::find_wide("PBD_TICK_RING"); (ring && *ring) || m_storage->launch.direct())
	{
		require(m_storage->checkpoint.capture_tick_state, "tick_ring_requires_probe_pages");
		const auto value = pbd::session_policy::recipe_number(m_storage->launch.direct(), ring, pbd::session_policy::ring_size, 1, 16);
		require(value.has_value(), "tick_ring_size_invalid");
		m_storage->checkpoint.ring.resize(*value);
		for (auto& entry : m_storage->checkpoint.ring)
		{
			entry.pages.resize(m_storage->checkpoint.checkpoint_pages.size() * 4096ull); // Preallocated before play.
			entry.clock.reserve(64);
		}
	}
}

void pbd::rollback_session::configure_initial_state()
{
	{
		// Fixture declaration. Absent = the retained arcade fixture.
		auto& f = initial_state();
		const auto number = [&](const char* name, u32& into)
		{
			if (const auto text = pbd::env::find_wide(name); text && *text)
			{
				require(pbd::env::parse_u32(text, into), "fixture_counter_invalid");
				f.source = "environment";
			}
		};
		number("PBD_FIXTURE_INPUT_COUNTER", f.input_counter);
		number("PBD_FIXTURE_BATTLE_COUNTER", f.battle_counter);
		if (const auto text = pbd::env::find_wide("PBD_FIXTURE_PLAYER_SHA256"); text && *text)
		{
			const std::wstring_view wide(text);
			require(wide.size() == 64, "fixture_player_hash_invalid");
			f.player_sha256.assign(wide.begin(), wide.end());
			for (const char ch : f.player_sha256)
				require((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'), "fixture_player_hash_invalid");
			f.source = "environment";
		}
		if (const auto text = pbd::env::find_wide("PBD_FIXTURE_SCENES"); text && *text)
		{
			auto scenes = pbd::env::parse_initial_scenes(text);
			require(scenes.has_value(), "fixture_scenes_invalid");
			f.scenes = std::move(*scenes);
			f.source = "environment";
		}
	}
}

void pbd::rollback_session::configure_replay_modes()
{
	if (const auto suspend = pbd::env::find_wide("PBD_RESTORE_SUSPEND"); suspend && std::wstring_view(suspend) == L"1")
		m_storage->correction.restore_suspend = true;
	if (const auto skip = pbd::env::find_wide("PBD_REPLAY_SKIP_DRAWS"); m_storage->launch.direct() || (skip && std::wstring_view(skip) == L"1"))
		m_storage->correction.replay_skip_draws = true;
	// The direct policy's replay draw skips leave a re-executed frame incomplete, so it never presents one.
	if (const auto exact = pbd::env::find_wide("PBD_REPLAY_WITHHOLD_EXACT");
		pbd::session_policy::replay_presentation_of(m_storage->launch).withhold_exact || (exact && std::wstring_view(exact) == L"1"))
		m_storage->correction.withhold_exact = true;
	// OFF by default: bounded renderer wait at window start, in milliseconds (1-50).
	if (const auto wait = pbd::env::find_wide("PBD_RESTORE_RENDER_WAIT_MS"); wait && *wait)
	{
		u32 value = 0;
		require(pbd::env::parse_u32_decimal(wait, value), "restore_render_wait_invalid");
		m_storage->correction.render.wait_ms = std::min<u32>(value, 50u);
	}
	// OFF by default: withhold a live frame the renderer finishes while a restore's marker is pending (see pbd_replay_pacing.h).
	if (const auto risk = pbd::env::find_wide("PBD_REPLAY_WITHHOLD_AT_RISK"); risk && std::wstring_view(risk) == L"1")
		pbd::replay_pacing::withhold_at_risk.store(1, std::memory_order_relaxed);
	// OFF by default: wake the vblank thread when a replay credit is granted (pbd_replay_pacing.h).
	if (const auto notify = pbd::env::find_wide("PBD_REPLAY_VBLANK_NOTIFY"); notify && std::wstring_view(notify) == L"1")
		pbd::replay_pacing::notify_on_grant.store(1);
	if (const auto lead = pbd::env::find_wide("PBD_REPLAY_VBLANK_LEAD"); lead && std::wstring_view(lead) == L"1")
		pbd::replay_pacing::lead.store(1);
	if (const auto rounds = pbd::env::find_wide("PBD_ROUND_TRANSITIONS"); rounds && std::wstring_view(rounds) == L"1")
		m_storage->round_transitions = true;
	if (const auto lifecycle = pbd::env::find_wide("PBD_ROUND_LIFECYCLE"); m_storage->launch.direct() || (lifecycle && std::wstring_view(lifecycle) == L"1"))
		m_storage->round_lifecycle = true;
}

void pbd::rollback_session::configure_match_replay()
{
	if (const auto record = pbd::env::find_wide("PBD_MATCH_REPLAY_RECORD"); record && std::wstring_view(record) == L"1")
	{
		auto& s = *m_storage;
		s.match_replay.requested = true;
		// A held match is configured before it has a seed. Recheck these preconditions after its bind.
		s.match_replay.setup_error = nullptr;
		s.history.match_replay_observations.resize(s.history.samples.size());
		s.history.match_replay_observed.resize(s.history.samples.size());
		const auto text = [](const char* key)
		{
			const auto value = pbd::env::find_wide(key);
			const std::wstring wide(value ? value : L"");
			return std::string(wide.begin(), wide.end());
		};
		s.match_replay.recipe_sha256 = text("PBD_MATCH_REPLAY_RECIPE_SHA256");
		s.match_replay.state_sha256 = text("PBD_MATCH_REPLAY_STATE_SHA256");
		if (const auto path = pbd::env::find_wide("PBD_MATCH_REPLAY_CLEAN_STATE"); path && *path)
			s.match_replay.clean_state = std::filesystem::path(path);
		if (const auto sync = pbd::env::find_wide("PBD_SESSION_STAGE_SYNC"); m_storage->launch.stage_sync || (sync && std::wstring_view(sync) == L"1"))
			s.match_replay.stage_sync = true;
		if (!s.session.enabled || !s.round_lifecycle || s.replay.session_replay || !s.session.seed_set || !s.session.seed)
			s.match_replay.setup_error = "clean_session_required";
		else if (!replay_sha256_text(s.match_replay.recipe_sha256) || !replay_sha256_text(s.match_replay.state_sha256) ||
				 !s.match_replay.clean_state.is_absolute())
			s.match_replay.setup_error = "checkpoint_provenance_required";
	}
}

void pbd::rollback_session::configure_rolling()
{
	if (const auto delay = pbd::env::find_wide("PBD_TICK_ROLL_DELAY"); delay && *delay)
	{
		// Rolling corrections from a delayed authoritative ledger over ticks [begin, end).
		const auto number = [&](const char* name)
		{
			const auto text = pbd::env::find_wide(name);
			require(text && *text, "tick_rolling_parameter_missing");
			u32 value = 0;
			require(pbd::env::parse_u32_decimal(text, value) && value <= 0xffff, "tick_rolling_parameter_invalid");
			return value;
		};
		auto& s = *m_storage;
		s.roll_delay = number("PBD_TICK_ROLL_DELAY");
		s.roll_begin = number("PBD_TICK_ROLL_BEGIN");
		s.roll_end = number("PBD_TICK_ROLL_END");
		require(!s.checkpoint.ring.empty() && !s.roundtrip.active && !s.checkpoint.late_index, "tick_rolling_requires_ring_without_roundtrip_or_late_probe");
		require(s.roll_delay >= 1 && s.roll_delay <= 8 && s.roll_delay < s.checkpoint.ring.size() && s.roll_begin > s.checkpoint.tick_hold_index + 1 + s.roll_delay &&
					s.roll_begin + s.roll_delay <= s.roll_end && s.roll_end + s.roll_delay <= s.extent(),
			"tick_rolling_window_invalid");
		const auto path = (s.checkpoint.capture_directory / "tick-correction.jsonl").u8string();
		require(s.correction.file.open(std::string(path.begin(), path.end()), fs::write_new + fs::isfile), "cannot_create_tick_correction_exclusively");
		s.correction.active = true;
		s.rolling = true;
	}
}

void pbd::rollback_session::configure_correct_at()
{
	if (const auto at = pbd::env::find_wide("PBD_TICK_CORRECT_AT"); at && *at)
	{
		require(!m_storage->checkpoint.ring.empty() && !m_storage->roundtrip.active && !m_storage->rolling && !m_storage->peer.enabled, "tick_correction_requires_ring_without_roundtrip");
		const auto number = [&](const char* name, u32 max, const char* message)
		{
			const auto text = pbd::env::find_wide(name);
			require(text && *text, message);
			u32 value = 0;
			require(pbd::env::parse_u32_decimal(text, value) && value <= max, message);
			return value;
		};
		m_storage->correction.at = number("PBD_TICK_CORRECT_AT", 0xffff, "tick_correct_at_invalid");
		m_storage->correction.depth = number("PBD_TICK_CORRECT_DEPTH", 8, "tick_correct_depth_invalid");
		m_storage->correction.predict_raw = static_cast<u16>(number("PBD_TICK_PREDICT_RAW", 0xffff, "tick_predict_raw_invalid"));
		require(m_storage->correction.depth >= 1 && m_storage->correction.depth < m_storage->checkpoint.ring.size() &&
					m_storage->correction.at > m_storage->checkpoint.tick_hold_index + 1 + m_storage->correction.depth && m_storage->correction.at < m_storage->input_sequence.size() &&
					(!m_storage->checkpoint.late_index || m_storage->checkpoint.late_index > m_storage->correction.at),
			"tick_correction_window_invalid");
		const auto path = (m_storage->checkpoint.capture_directory / "tick-correction.jsonl").u8string();
		require(m_storage->correction.file.open(std::string(path.begin(), path.end()), fs::write_new + fs::isfile), "cannot_create_tick_correction_exclusively");
		m_storage->correction.active = true;
	}
}
#endif
