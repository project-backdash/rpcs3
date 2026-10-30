#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::boundary_call::prepare_lobby_clean_start(pbd::lobby_set::clean_start& clean, u32& counter)
{
	std::array<be_t<u32>, 2> scene{};
	const auto identity = Emu.PBDGetCheckpointBootIdentity();
	clean.seed = data.session.seed;
	clean.generation = data.generation;
	clean.title_id = Emu.GetTitleID();
	clean.require_round_state = true;
	if (const auto* early = pbd::env::find("PBD_SPECTATOR_FROM_START"))
		clean.publish_from_start = std::string_view(early) == "1";
	if (data.lobby.match_spectator_from_start)
		clean.publish_from_start = *data.lobby.match_spectator_from_start;
	clean.start_skip = data.session.start_skip;
	clean.exit_gate = data.session.gate.enabled;
	clean.phase_delay = data.peer.phase_delay;
	clean.keyed_draws = pbd::fn_hook::keyed_draws_enabled();
	clean.ban_jinpachi = pbd::character_rule::banned(data.generation);
	clean.battle_clock_version = data.battle_clock.version;
	clean.menu_hash_mask = data.session.menu_hash_mask;
	if (identity)
		clean.checkpoint_sha256 = identity->sha256;
	if (const auto* recipe = pbd::env::find("PBD_MATCH_REPLAY_RECIPE_SHA256"))
		clean.recipe_sha256 = recipe;
	clean.verified = identity && identity->emulation_generation == data.generation && replay_sha256_text(identity->sha256) &&
	                 replay_sha256_text(clean.recipe_sha256) && data.next_index == data.start_index && !data.session.matches &&
	                 clean.title_id == pbd::t5dr::title_id &&
	                 vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false) &&
	                 scene[0] == 3u && scene[1] == 44u && read_word(0x1008f07c, counter) &&
	                 counter == (0x80000000u | (data.session.seed & 0x00ffffffu)) && data.session.generator_syncs && data.session.random_panel_syncs;
}

bool pbd::rollback_session::boundary_call::admit_lobby_selection()
{
	// Fixed picks use the authenticated stock resource setters, checked before their first native call.
	// The in-game countdowns and repicks also need the stock select's tables.
	if (data.lobby.preselect)
		return false;
	if (!pbd::hidden_selection::pinned_binary())
	{
		data.lobby.set->refuse("selection_binary_mismatch", "This game binary has no verified lobby selection driver.");
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}
	std::string selection_refusal;
	auto tables = pbd::hidden_selection::read_native_tables(&selection_refusal);
	if (!tables)
	{
		data.lobby.set->refuse("selection_table_unverified", "Native selection table refusal: " + selection_refusal);
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}
	if (data.game_overlay.panel.visible)
	{
		data.lobby.set->refuse("selection_menu_visible", "A visible game menu owns the native selection input.");
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}

	return false;
}
