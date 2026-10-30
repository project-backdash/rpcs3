// The lobby at the boundary: its commands, the VS cards, the session wait and its Leave, the selection's sound and
// timer, the start of a lobby set, and a watcher's boundary: a standby's bind, the clean start and the catch-up pace.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_spectator_standby.h"
#include "../PBD/pbd_replay_standby.h"
#include "../PBD/pbd_spectator_boundary_steps.h"
#include "../PBD/pbd_lobby_command_steps.h"
#include "../PBD/pbd_rank_screens.h"

void pbd::rollback_session::boundary_call::lobby_poll_command()
{
	if (data.lobby.channel && data.lobby.channel->output_failed())
	{
		session_runtime.finish(output_failed);
		return;
	}
	if (!data.lobby.set || !data.lobby.set->healthy())
		return;
	if (data.lobby.set->complete())
	{
		// Local sealing proves neither delivery of our last segment nor the other result. Keep authenticated
		// menu retransmits alive until show_post_set acknowledges the service's agreement of both results.
		const auto sealed = data.lobby.set->sealed_game();
		if (sealed && sealed->terminal)
			data.peer.link.terminal_idle.store(true, std::memory_order_release);
	}
	if (data.lobby.pending_command)
		return;
	const auto now = session_clock::now();
	if (now < data.lobby.next_poll)
		return;
	data.lobby.next_poll = now + std::chrono::milliseconds(8);
	pbd::stall_metrics::scope measure_command("set_command_read", data.next_index, 0, true, 200'000);
	auto parsed = data.lobby.channel->next_command(data.lobby.channel->identity().game_number);
	if (!parsed.value)
	{
		if (parsed.error != "unavailable")
		{
			pbd_session_log.error("Lobby command failed: %s", parsed.error);
			data.lobby.set->refuse("invalid_command", "The lobby command could not be verified.");
			session_runtime.finish(guard_failed);
		}
		return;
	}
	using pbd::set_channel::command_kind;
	const auto& command = *parsed.value;
	// The next set against the same opponent (Join Battle) continues from the completed set's natural end exactly as the next
	// game of a set does: stock select or the verified Rematch resource load runs in the same session. The seed, options and
	// delay stay; the driver's and the channel's set identity and score start again.
	const bool next_set = command.kind == command_kind::next_set;
	if (command.kind == command_kind::continue_game || next_set)
	{
		lobby_command_steps{*this, command, next_set}.run();
		return;
	}
	if (command.kind == command_kind::stream_status)
	{
		if (!data.lobby.set->apply_stream_status(command))
			session_runtime.finish(guard_failed);
		return;
	}
	if (command.kind == command_kind::chat_overlay || command.kind == command_kind::vs_cards || command.kind == command_kind::rank_stakes)
	{
		lobby_display_command(command);
		return;
	}
	if ((command.kind == command_kind::show_post_set || command.kind == command_kind::choice_rejected) && !data.lobby.set->complete())
	{
		data.lobby.set->refuse("post_set_before_result", "The native game result must be confirmed before showing post-set choices.");
		session_runtime.finish(guard_failed);
		return;
	}
	// The PBD-MENU-1 file remains the only visible controller surface. Its snapshot is applied before this JSON ACK: the one
	// already shown, when it was read before its command, or a newer one (overlay_controls::apply_panel).
	if (const auto now = pbd::lobby_set::take_menu_command(std::move(*parsed.value), data.game_overlay.panel, data.lobby.pending_command,
			data.lobby.menu_generation))
	{
		pbd_session_log.notice("Lobby command %llu applied by in-game menu generation %llu, read before the command.",
			static_cast<unsigned long long>(now->serial), static_cast<unsigned long long>(data.game_overlay.panel.generation));
		lobby_menu_command(*now);
	}
}

// A show_post_set, choice_rejected or end_set whose panel has been applied (pbd::lobby_set::take_menu_command and
// menu_panel_applied): acknowledged, and an end_set ends the session.
void pbd::rollback_session::boundary_call::lobby_menu_command(const pbd::set_channel::command& command)
{
	using pbd::set_channel::command_kind;
	// The lobby ended the set (completed, a player left, the service was lost, a disputed result): the session ends as
	// lobby_set_ended, which the launcher shows as a neutral end, never as either player stopping it.
	if (command.kind == command_kind::end_set)
	{
		pbd_session_log.warning("Lobby set ended by the lobby (%s): the session ends as lobby_set_ended.", command.reason);
		// Fade this session to black while the launcher takes over.
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		pbd::set_notices::hand_off(data.generation, static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
		session_runtime.finish(lobby_set_ended);
	}
	if (!data.lobby.channel->acknowledge(command))
	{
		session_runtime.finish(guard_failed);
		return;
	}
	if (!data.lobby.set)
		return;
	const auto sealed = data.lobby.set->sealed_game();
	if (pbd::lobby_set::terminal_results_acknowledged(data.lobby.set->complete(), sealed && sealed->terminal, command.kind))
	{
		std::lock_guard lock(data.peer.rx.mutex);
		data.peer.link.auth.retire();
		data.peer.rx.queue.clear();
	}
}

// Service safe commands in order while a peer wait holds; an ending needs no menu panel.
bool pbd::rollback_session::boundary_call::lobby_wait_end()
{
	if (!session_runtime.m_active.load())
		return true;
	if (data.lobby.channel && data.lobby.channel->output_failed())
	{
		session_runtime.finish(output_failed);
		return true;
	}
	if (!data.lobby.channel || !data.lobby.set || !data.lobby.set->healthy())
		return false;
	const auto now = session_clock::now();
	if (now < data.lobby.next_poll)
		return false;
	data.lobby.next_poll = now + std::chrono::milliseconds(8);
	return pbd::lobby_set::poll_wait_commands(*data.lobby.channel, data.lobby.pending_command, [&](const pbd::set_channel::command& command)
		{
			if (command.kind == pbd::set_channel::command_kind::stream_status)
			{
				if (!data.lobby.set->apply_stream_status(command))
					session_runtime.finish(guard_failed);
			}
			else
				lobby_display_command(command);
			return session_runtime.m_active.load();
		},
		[&](const pbd::set_channel::command& command)
		{
			lobby_menu_command(command);
		},
		[&](const std::string& error)
		{
			pbd_session_log.error("Lobby command failed while waiting: %s", error);
			data.lobby.set->refuse("invalid_command", "The lobby command could not be verified.");
			session_runtime.finish(guard_failed);
		});
}

// Apply and acknowledge display-only commands for chat, card visibility, mode, fighter captions, stakes and placement counts.
void pbd::rollback_session::boundary_call::lobby_display_command(const pbd::set_channel::command& command)
{
	using pbd::set_channel::command_kind;
	// An invalid optional display field reads as left out; the first one is named so a launcher's mistake is found.
	if (!command.ignored.empty() && !data.lobby.ignored_logged)
	{
		data.lobby.ignored_logged = true;
		pbd_session_log.warning("Lobby command %llu: %s read as left out (invalid).", static_cast<unsigned long long>(command.serial),
			fmt::merge(command.ignored, ", "));
	}
	if (command.kind == command_kind::rank_stakes)
	{
		// The VS cards tag the stakes, and the notices show each placing side's count, from the next fresh boundary on
		// (publish_vs_cards, publish_set_notices).
		data.lobby.rank_stakes = command.stakes;
		data.lobby.placements = command.placements;
		if (!data.lobby.channel->acknowledge(command))
			session_runtime.finish(output_failed);
		return;
	}
	if (command.kind == command_kind::chat_overlay)
	{
		std::vector<pbd::corner_chat::message> additions;
		additions.reserve(command.additions.items.size());
		for (const auto& item : command.additions.items)
		{
			const auto* name = item.find("authorName");
			const auto* text = item.find("text");
			additions.push_back({item.find("messageId")->text, name ? name->text : std::string{},
				text && text->kind == pbd::set_channel::json_kind::string ? text->text : std::string{}});
		}
		// Drawing owns an immutable host snapshot only. A command is acknowledged after that snapshot is applied.
		if (!pbd::corner_chat::publish(static_cast<u64>(Emu.GetEmulationIdentifier()), command.visible, additions))
		{
			pbd_session_log.warning("The lobby chat could not be displayed; the match continues.");
			pbd::corner_chat::clear();
		}
		if (!data.lobby.channel->acknowledge(command))
		{
			pbd::corner_chat::clear();
			session_runtime.finish(output_failed);
		}
		return;
	}
	// Card visibility takes effect at the next fresh boundary; mode and fighter captions also feed the set notices.
	// Language and region remain parsed for wire compatibility.
	data.lobby.vs_mode = command.mode;
	data.lobby.vs_characters = command.characters;
	data.lobby.vs_cards_on = command.visible;
	// A set's start in a game held ready since before the launcher finished the notices' pictures (pbd::notice_art::retry).
	if (const auto art = pbd::notice_art::retry(std::chrono::steady_clock::now()))
		pbd_session_log.notice("Notice pictures: %u of %u read on a later look.", art->found, pbd::notice_art::picture_count);
	if (!command.visible)
		pbd::vs_cards::hide(data.generation);
	if (!data.lobby.channel->acknowledge(command))
		session_runtime.finish(output_failed);
}

// The VS cards (pbd::vs_cards) for the scene at this fresh boundary: the manifest's names, game number, score and rules, and
// the launcher's captions, plates and stakes as the notices show them. Display only, and it runs only where no re-execution
// does.
void pbd::rollback_session::boundary_call::publish_vs_cards(u32 scene)
{
	if (!data.lobby.channel || !data.lobby.vs_cards_on)
		return;
	// Outside the VS screens only the boundary that leaves them publishes (the hidden cards); a fight's boundaries cost nothing.
	const bool vs = pbd::vs_cards::vs_scene(scene);
	if (!vs && !data.lobby.vs_cards_shown)
		return;
	data.lobby.vs_cards_shown = vs;
	const auto& identity = data.lobby.channel->identity();
	const auto notices = pbd::set_notices::load();
	pbd::vs_cards::set_view set;
	set.names = {identity.left.name, identity.right.name};
	set.characters = data.lobby.vs_characters;
	if (notices.active && notices.generation == data.generation)
		set.plates = {notices.sides[0].shown, notices.sides[1].shown};
	set.local = identity.local_side == pbd::set_channel::side::left ? 0u : 1u;
	set.game_number = identity.game_number;
	set.first_to = identity.first_to_games;
	set.games = pbd::rank_stakes::score{identity.games.left, identity.games.right};
	if (data.lobby.vs_mode == pbd::set_channel::card_mode::ranked)
		set.stakes = data.lobby.rank_stakes;
	pbd::vs_cards::publish(data.generation, pbd::vs_cards::compose(set, scene));
}

// The Ranked notices (pbd::set_notices) for this fresh boundary: the manifest's names, ranks, game and score, the launcher's
// mode, captions, stakes and placement counts, the stake words this game writes, and the game's screen (the scene and, in a
// fight, the pending phase at 0x10093230, the word the recorder classifies, which also names whose intro plays). In a fight it
// also reads whether the game draws each plate, and at a knockout each side's change word (pbd::game_profile::change_address),
// which the game sets where it flips that side's plate. Display only: reads of guest memory, never a write, and only where no
// re-execution runs. The game's first VS frame lifts the loading cover, so the game's own VS screen shows
// (publish_game_overlay).
void pbd::rollback_session::boundary_call::publish_set_notices(u32 scene)
{
	if (!data.lobby.channel)
		return;
	pbd::set_notices::set_view set;
	u32 pending = ~0u;
	const bool fight = scene == 16u || scene == 17u;
	if (!fight || !read_word(0x10093230, pending))
		pending = ~0u;
	set.now = pbd::set_notices::screen_of(scene, pending);
	set.intro_side = pbd::set_notices::intro_side_of(scene, pending);
	if (set.now == pbd::set_notices::screen::knockout)
		for (u32 side = 0; side < 2; ++side)
		{
			u32 change = 0;
			set.promoted[side] = read_word(pbd::game_profile::change_address(side), change) && change == pbd::game_profile::change_promoted;
		}
	if (set.now == pbd::set_notices::screen::vs && !data.lobby.vs_seen)
	{
		data.lobby.vs_seen = true;
		publish_game_overlay();
	}
	if (set.now == pbd::set_notices::screen::round_intro || set.now == pbd::set_notices::screen::fight)
		data.lobby.fight_seen = true;
	using pbd::set_channel::card_mode;
	set.mode = data.lobby.vs_mode == card_mode::ranked ? pbd::set_notices::set_mode::ranked :
	           data.lobby.vs_mode == card_mode::quick  ? pbd::set_notices::set_mode::quick :
	                                                     pbd::set_notices::set_mode::lobby;
	const auto& identity = data.lobby.channel->identity();
	set.names = {identity.left.name, identity.right.name};
	set.characters = data.lobby.vs_characters;
	set.ranks = with_kept_promotions({identity.left.rank, identity.right.rank});
	set.placements = data.lobby.placements;
	set.local = identity.local_side == pbd::set_channel::side::left ? 0u : 1u;
	set.game_number = identity.game_number;
	set.first_to = identity.first_to_games;
	set.games = {identity.games.left, identity.games.right};
	set.replay = data.lobby.game_replay;
	set.stakes = data.lobby.rank_stakes;
	if (pbd::rank_screens::configured().screens)
		set.game_stakes = pbd::rank_screens::armed().stake;
	set.first_intro = !data.lobby.fight_seen;
	u32 mode = 0, players = 0, humans = 0;
	if (fight && read_word(pbd::t5dr::game_mode_address, mode) && read_word(0x1008f080, players) && read_word(0x1008f0b0, humans))
		set.plate_drawn = {pbd::game_profile::plate_drawn(mode, players, humans, 0), pbd::game_profile::plate_drawn(mode, players, humans, 1)};
	pbd::set_notices::publish(data.generation, pbd::set_notices::compose(set));
}

// The launcher's panel for the overlay: behind the loading cover while the lobby's hidden selection or direct load runs, up to
// this game's first VS frame, where the cover lifts so the game's own VS screen and the cards over it show.
void pbd::rollback_session::boundary_call::publish_game_overlay()
{
	const bool loading = (data.lobby.preselect && !data.lobby.battle_seen) || (data.lobby.selection && data.lobby.selection->hidden()) ||
	                     data.lobby.rematch.straight_to_vs();
	pbd::game_overlay::publish(loading && !data.lobby.vs_seen ? pbd::game_overlay::selection_card(data.game_overlay.panel) : data.game_overlay.panel);
}

// Both players' names and ranks in the game's own player records (pbd::game_profile), from which the game draws them under the
// life bars and on the VS screen. Only bytes that differ are written, so a repeat costs four reads and leaves the pages alone.
// False when a record could not be read or written.
bool pbd::rollback_session::boundary_call::store_game_profiles(const std::array<std::string_view, 2>& names,
	const std::array<u32, 2>& ranks)
{
	bool ok = true;
	for (u32 side = 0; side < 2; side++)
	{
		auto name = pbd::game_profile::encode_name(names[side]);
		auto held = name;
		u8 rank = pbd::game_profile::rank_byte(ranks[side]), held_rank = rank;
		const u32 name_at = pbd::game_profile::name_address(side), rank_at = pbd::game_profile::rank_address(side);
		if (!vm::try_access(name_at, held.data(), static_cast<u32>(held.size()), false) || !vm::try_access(rank_at, &held_rank, 1, false))
		{
			ok = false;
			continue;
		}
		if (held != name && !vm::try_access(name_at, name.data(), static_cast<u32>(name.size()), true))
			ok = false;
		if (held_rank != rank && !vm::try_access(rank_at, &rank, 1, true))
			ok = false;
	}
	return ok;
}

// A lobby set's players (lobby, Quick Match and Ranked alike) in the game's records, from the set's manifest, which both PCs hold
// alike. Called only where both PCs stand on the same boundary with nothing before it left to re-execute (the session start and
// each scene change's barrier), so both games hold the same bytes from the same frame on; the records are display only and
// outside the hashed regions, so neither rollback nor the sync check depends on them. Without a bound set (Practice, the game's
// own modes) nothing is written. As a fight's first screen begins, the log says whether the game draws each plate.
void pbd::rollback_session::boundary_call::write_game_profiles(u32 boundary, u32 scene)
{
	if (!data.lobby.channel)
		return;
	keep_promoted_ranks(boundary);
	const auto& set = data.lobby.channel->identity();
	if (!store_game_profiles({set.left.name, set.right.name}, {set.left.rank, set.right.rank}))
	{
		pbd_session_log.error("Lobby set: the players' names could not be written into the game's records at boundary %u.", boundary);
		return;
	}
	if (scene != 16u && scene != 17u)
		return;
	u32 mode = 0, players = 0, humans = 0;
	const bool words_read = read_word(pbd::t5dr::game_mode_address, mode) && read_word(0x1008f080, players) && read_word(0x1008f0b0, humans);
	const auto plate = [&](u32 side)
	{
		return words_read && pbd::game_profile::plate_drawn(mode, players, humans, side) ? "drawn" : "not drawn";
	};
	pbd_session_log.notice("Lobby set: game %u shows %s (%s, plate %s) and %s (%s, plate %s) under the life bars from boundary %u.",
		set.game_number, set.left.name, pbd::game_profile::rank_names[pbd::game_profile::rank_byte(set.left.rank)], plate(0),
		set.right.name, pbd::game_profile::rank_names[pbd::game_profile::rank_byte(set.right.rank)], plate(1), boundary);
}

// Plan from stakes agreed at the preceding barrier (local launcher stakes without a peer), before game_preparing records
// the values for spectators and replays. A new set has no stakes until the command following next_set supplies them.
pbd::rank_stakes::game_values pbd::rollback_session::boundary_call::plan_rank_screens(const pbd::set_channel::command& command, bool next_set)
{
	pbd::rank_stakes::game_values values;
	if (!pbd::rank_screens::configured().screens || !data.lobby.channel || !data.lobby.set)
		return values;
	const pbd::rank_stakes::score games{command.games.left, command.games.right};
	const u32 first_to = data.lobby.channel->identity().first_to_games;
	const auto& stakes = data.peer.enabled ? data.lobby.agreed_stakes : data.lobby.rank_stakes;
	if (stakes && !next_set)
		values = pbd::rank_stakes::plan(*stakes, games, first_to);
	else if (data.peer.enabled && data.lobby.rank_stakes && !next_set && pbd::rank_stakes::plan(*data.lobby.rank_stakes, games, first_to).any())
		pbd_session_log.warning("Lobby set: game %u has stakes here, but the two PCs did not hold the same stakes at the last scene "
								"barrier; it is written without them.",
			command.game_number);
	data.lobby.set->rank_screens(values.any() ? std::optional(values) : std::nullopt);
	return values;
}

// Hold these values until the next game's command, using the manifest ranks after any next_set update.
void pbd::rollback_session::boundary_call::arm_rank_screens(const pbd::rank_stakes::game_values& values)
{
	if (!pbd::rank_screens::configured().screens || !data.lobby.channel)
		return;
	const auto& set = data.lobby.channel->identity();
	pbd::rank_screens::arm(values, {set.left.rank, set.right.rank});
	if (values.any())
		pbd_session_log.notice("Lobby set: game %u is written with stakes %u/%u and PROMOTED TO %d/%d.", set.game_number,
			values.stake[0], values.stake[1], values.promote[0], values.promote[1]);
}

// Launcher updates arrive independently; compare their announcements at the shared barrier so the next command plans
// identical rank screens on both peers. Mismatched announcements suppress stakes until they agree.
void pbd::rollback_session::boundary_call::settle_rank_stakes(u32 segment)
{
	if (!pbd::rank_screens::configured().screens || !data.lobby.channel)
		return;
	const u32 own = data.session.segments.rank_stakes, other = data.session.segments.peer_rank_stakes[segment % 8];
	const auto agreed = pbd::rank_stakes::agreed_stakes(own, other);
	if (agreed != data.lobby.agreed_stakes)
		pbd_session_log.notice("Lobby set: from segment %u the next game is planned %s (stakes 0x%x here, 0x%x on the other PC).", segment,
			agreed ? "from the stakes both PCs hold" : "without stakes", own, other);
	data.lobby.agreed_stakes = agreed;
}

// Preserve promotions confirmed by the guest's rank and change fields when rewriting player records.
std::array<u32, 2> pbd::rollback_session::boundary_call::with_kept_promotions(std::array<u32, 2> ranks)
{
	if (!pbd::rank_screens::configured().screens)
		return ranks;
	const auto values = pbd::rank_screens::armed();
	for (u32 side = 0; side < 2; ++side)
	{
		u32 change = 0;
		u8 rank = 0;
		if (read_word(pbd::game_profile::change_address(side), change) && vm::try_access(pbd::game_profile::rank_address(side), &rank, 1, false))
			if (const auto kept = pbd::rank_stakes::kept_promotion(values, side, change, rank))
				ranks[side] = *kept;
	}
	return ranks;
}

// At a confirmed scene boundary, retain guest-confirmed promotions in the manifest until next_set supplies settled ranks.
void pbd::rollback_session::boundary_call::keep_promoted_ranks(u32 boundary)
{
	const auto& set = data.lobby.channel->identity();
	const std::array<u32, 2> held{set.left.rank, set.right.rank};
	const auto kept = with_kept_promotions(held);
	for (u32 side = 0; side < 2; ++side)
	{
		if (kept[side] == held[side])
			continue;
		data.lobby.channel->hold_rank(side ? pbd::set_channel::side::right : pbd::set_channel::side::left, kept[side]);
		pbd_session_log.notice("Lobby set: %s keeps the promoted rank %s from boundary %u.", side ? set.right.name : set.left.name,
			pbd::game_profile::rank_names[pbd::game_profile::rank_byte(kept[side])], boundary);
	}
}

// The Leave on the "Waiting for <name>'s game to load..." box (pbd::session_wait): a two-PC session offers it, a lobby set's
// ends the set with no result, any other session's ends the session.
pbd::session_wait::leave_kind pbd::rollback_session::boundary_call::session_wait_leave()
{
	if (!data.session.enabled || !data.peer.enabled)
		return pbd::session_wait::leave_kind::none;
	return data.lobby.set ? pbd::session_wait::leave_kind::set : pbd::session_wait::leave_kind::session;
}

// The player took that Leave at a sync point held for the other PC (boundary names it in the log). A lobby set writes the set
// event leave peer_wait, which the lobby ends with no result, and decides this end's leave so that finish() writes no
// leave_seat for it: waiting for a game that does not load is not giving up the seat. The session then ends as user_stopped,
// the same end as the leave chord.
void pbd::rollback_session::boundary_call::leave_session_wait(u32 boundary)
{
	if (data.lobby.set && !data.lobby.leave_decided.exchange(true))
	{
		if (data.lobby.set->leave_peer_wait())
			pbd_session_log.warning("Lobby set: the player left while waiting for the other PC at boundary %u; leave peer_wait written.", boundary);
		else
			pbd_session_log.error("Lobby set: the leave peer_wait event at boundary %u could not be written.", boundary);
	}
	else
		pbd_session_log.warning("The player left the session while waiting for the other PC at boundary %u.", boundary);
	session_runtime.finish(user_stopped);
}

// Mute the hidden selection through the black loading scenes, then restore the player's volume at the first VS frame.
// The menu hold shares this mute and leaves it intact during hidden selection; session termination restores it too.
// A visible repick keeps its own selection audio.
void pbd::rollback_session::boundary_call::lobby_selection_audio()
{
	const bool selecting =
		((data.lobby.selection && data.lobby.selection->hidden()) || data.lobby.rematch.straight_to_vs()) && !data.lobby.vs_seen;
	if (selecting && !data.lobby.selection_audio_muted)
	{
		if (!data.game_overlay.audio_muted)
		{
			pbd::player_volume::mute();
			data.game_overlay.audio_muted = true;
		}
		data.lobby.selection_audio_muted = true;
	}
	else if (!selecting && data.lobby.selection_audio_muted)
	{
		data.lobby.selection_audio_muted = false;
		if (data.game_overlay.audio_muted)
		{
			pbd::player_volume::restore();
			data.game_overlay.audio_muted = false;
		}
	}
}

// What the in-game select's countdowns read at a local sample: the flow scene and BOTH select records (the same view on both
// PCs at the same menu boundary). Read-only; a battle scene needs no record.
pbd::select_timer::view pbd::rollback_session::boundary_call::select_view()
{
	pbd::select_timer::view out;
	u32 scene = 0;
	if (!read_word(pbd::t5dr::scene_address, scene))
		return out;
	out.scene = scene;
	if (pbd::select_timer::battle_scene(scene))
	{
		out.verified = true;
		return out;
	}
	const auto left = pbd::hidden_selection::read_native_observation(0), right = pbd::hidden_selection::read_native_observation(1);
	if (!left || !right || !left->verified || !right->verified || left->scene != scene || right->scene != scene)
		return out;
	out.vs_mode = left->mode == 4 && left->subscene == 44 && left->grid_mode == 1;
	for (u32 role = 0; role < 2; ++role)
	{
		const auto& record = role ? *right : *left;
		out.state[role] = record.state;
		out.handicap[role] = record.handicap;
		if (record.state == pbd::select_timer::state_stage)
		{
			out.stage_open = true;
			out.stage_owner = record.stage_owner;
		}
	}
	out.verified = true;
	return out;
}

bool pbd::rollback_session::boundary_call::lobby_set_start()
{
	if (data.lobby.set && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && !data.lobby.start_attempted)
	{
		data.lobby.start_attempted = true;
		if (data.peer.link.auth.can_learn() && data.peer.link.auth_set_id != data.lobby.channel->identity().set_id)
		{
			data.lobby.set->refuse("peer_auth_set_mismatch", "The private authentication belongs to another set.");
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		pbd::lobby_set::clean_start clean;
		u32 counter = 0;
		prepare_lobby_clean_start(clean, counter);
		if (admit_lobby_selection())
			return true;
		const auto& manifest = data.lobby.channel->identity();
		if (data.lobby.preselect)
		{
			pbd::rematch::options rules;
			clean.direct_start = true;
			clean.selection_admitted = pbd::rematch::capture_options(ppu, rules, true) && rules.rounds == manifest.rounds_per_game &&
			                           data.lobby.rematch.arm_start({{manifest.left.row, manifest.right.row}, manifest.stage_id}, rules, data.generation, data.next_index);
			if (!clean.selection_admitted)
			{
				data.lobby.set->refuse("direct_start_unverified", "The agreed lobby fighters or rules are unavailable in this game.");
				session_runtime.finish(guard_failed);
				data.in_callback.store(false);
				return true;
			}
		}
		// In-game selection: from this clean character-select origin the players' own input reaches the stock select, and
		// the game's applied picks are read at the first round intro (pbd::lobby_set::driver).
		else
			clean.selection_admitted = true;
		if (!data.lobby.set->begin(data.next_index, counter, std::move(clean)))
		{
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		if (data.lobby.preselect)
			pbd::game_overlay::publish(pbd::game_overlay::selection_card(data.game_overlay.panel));
		else if (!data.lobby.select_timer || !data.lobby.select_timer->begin(manifest.selection.character_seconds, manifest.selection.stage_seconds, data.peer.role))
		{
			data.lobby.set->refuse("selection_timer_unavailable", "The in-game selection countdowns could not be armed.");
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		else
			pbd_session_log.notice("Lobby set: in-game selection from boundary %u (character %u s, stage %u s).",
				data.next_index, manifest.selection.character_seconds, manifest.selection.stage_seconds);
	}
	return false;
}

bool pbd::rollback_session::boundary_call::spectator_boundary()
{
	if (data.spectator.channel && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active)
		return spectator_boundary_steps{*this, *data.spectator.channel}.run();
	return false;
}

// This install's own clean character select at the session's first boundary, before any seed is applied: the checkpoint
// this process booted, the pinned game code and tables, the setup recipe, the scene, the rules and the button maps.
bool pbd::rollback_session::boundary_call::spectator_start_clean()
{
	std::array<be_t<u32>, 2> scene{};
	be_t<u8> rounds{};
	const auto identity = Emu.PBDGetCheckpointBootIdentity();
	const auto* recipe = pbd::env::find("PBD_MATCH_REPLAY_RECIPE_SHA256");
	return identity && identity->emulation_generation == data.generation && Emu.GetTitleID() == pbd::t5dr::title_id &&
	       pbd::hidden_selection::pinned_binary() && pbd::hidden_selection::read_native_tables() && recipe && *recipe &&
	       !data.next_index && !data.start_index && !data.session.matches &&
	       vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false) && scene[0] == 3u && scene[1] == 44u &&
	       vm::try_access(0x1008ebd4, &rounds, sizeof(rounds), false) && u8{rounds} == 3 && mapping_matches(0) &&
	       mapping_matches(1);
}

// Hidden catch-up runs at the pace of a correction's replayed ticks: frame limit off, draws skipped, and one virtual
// vblank per frame served at once (pbd::replay_pacing), so no frame waits for the vblank grid. The 960 Hz grid stays
// underneath for any wait a credit does not cover. A credit is a vblank like any other: the guest's handler and counters
// run as for a scheduled one, and every frame is still checked against the fighters' sync facts.
void pbd::rollback_session::boundary_call::spectator_pacing(bool fast)
{
	if (fast != data.spectator.fast)
	{
		data.spectator.fast = fast;
		// Raising vblank alone still leaves flips capped to the display refresh rate.
		g_disable_frame_limit = fast || data.spectator.saved_frame_limit_disabled;
		// Catch-up is hidden; preserve guest flips while omitting the scene's draw calls.
		pbd::replay_pacing::skip_draws.store(fast || data.spectator.saved_skip_draws, std::memory_order_release);
		const u32 rate = fast ? 960u : static_cast<u32>(data.spectator.saved_vblank);
		g_cfg.video.vblank_rate.set(rate);
		pbd::replay_pacing::vblank_request.store(rate, std::memory_order_release);
		pbd::replay_pacing::armed.store(fast || data.replay.vblank ? 1u : 0u, std::memory_order_release);
	}
	// This frame passed its vblank wait before the boundary; the credit serves the next one's.
	if (fast)
		pbd::replay_pacing::grant_now();
}

// What every watcher runs with: one session with the round lifecycle and the tick checks, the start skip, the exit gate
// and the second battle clock, and none of the fighters' or the tools' modes.
bool pbd::rollback_session::watcher_mode() const
{
	const auto& s = *m_storage;
	return s.session.enabled && s.round_lifecycle && s.checkpoint.tick && s.session.start_skip && s.battle_clock.version == 2 && s.session.gate.enabled &&
	       !s.peer.enabled && !s.correction.active && !s.rolling && !s.practice.rewind && !s.replay.session_replay &&
	       (!s.replay.controls || pbd::replay_standby::pbd_requested()) && !s.game_overlay.enabled &&
	       !pbd::set_channel::requested_directory();
}

// The input policy this process runs with, as a set's manifest declares one.
pbd::spectator_standby::policy pbd::rollback_session::watcher_policy() const
{
	const auto& s = *m_storage;
	return {s.session.start_skip, s.session.gate.enabled, pbd::fn_hook::keyed_draws_enabled(), s.battle_clock.version,
		s.session.menu_hash_mask};
}

void pbd::rollback_session::configure_spectator(const std::filesystem::path& root)
{
	auto& s = *m_storage;
	s.spectator.saved_vblank = g_cfg.video.vblank_rate.get();
	s.spectator.saved_frame_limit_disabled = g_disable_frame_limit.load();
	s.spectator.saved_skip_draws = pbd::replay_pacing::skip_draws.load(std::memory_order_relaxed);
	s.session.failstop = true;
	if (pbd::spectator_standby::requested())
	{
		// The set and its seed come with the bind; everything else is checked now, as for a watcher launched for a set.
		require(watcher_mode() && !s.session.seed_set, "spectator_standby_conflict");
		s.spectator.standby = root;
		return;
	}
	auto manifest = pbd::spectator::read_manifest(root);
	require(manifest.value.has_value(), "spectator_manifest_invalid");
	const char* refusal = accept_spectator(root, std::move(*manifest.value));
	require(!refusal, refusal);
}

const char* pbd::rollback_session::accept_spectator(const std::filesystem::path& root, pbd::spectator::manifest&& manifest)
{
	auto& s = *m_storage;
	if (!watcher_mode() || !s.session.seed_set || s.session.seed != manifest.seed)
		return "spectator_mode_conflict";
	if (!lobby_executable_matches(manifest.emulator))
		return "spectator_emulator_build_mismatch";
	if (const char* refusal = pbd::spectator_standby::policy_refusal(watcher_policy(), manifest))
	{
		if (refusal == pbd::spectator_standby::keyed_draws_refusal)
			pbd_session_log.error("%s", pbd::spectator::keyed_policy_refusal);
		return refusal;
	}
	pbd::character_rule::configure(s.generation, manifest.ban_jinpachi);
	if (!manifest.pbd_capsule.empty() &&
		!s.lobby.direct_match.pbd_configure_capsule_start(s.generation, !s.peer.enabled && !s.replay.session_replay && !s.practice.rewind,
			s.session.seed, manifest.pbd_source_game, manifest.pbd_capsule))
		return "spectator_capsule_initializer_refused";
	s.spectator.channel = std::make_unique<pbd::spectator::local_channel>(root, std::move(manifest));
	if (!s.spectator.channel->healthy())
		return "spectator_channel_invalid";
	if (const auto* trace = pbd::env::find("PBD_SPECTATOR_EXECUTED_TRACE"); trace && *trace)
	{
		const std::filesystem::path path(trace);
		if (!path.is_absolute())
			return "spectator_executed_trace_invalid";
		const auto utf8 = path.u8string();
		if (!s.spectator.execution_file.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
			return "spectator_executed_trace_collision";
	}
	return nullptr;
}

// A standby watcher's first boundary: prove the clean start, report it, and hold here until the launcher writes the set's
// manifest. Holding the first boundary is what the session start already does while a fighter waits for the other one.
// The bound seed then goes through the session start sync at this same boundary, applied exactly as for a watcher
// launched with it, and the watch goes on to its prepared report. True when the standby ended instead.
bool pbd::rollback_session::boundary_call::spectator_standby()
{
	namespace standby = pbd::spectator_standby;
	if (!data.spectator.standby || pc != pcs[0] || data.update_active || data.roundtrip.repeat_active)
		return false;
	const auto root = *std::exchange(data.spectator.standby, std::nullopt);
	const auto from = session_clock::now();
	const auto waited_ms = [&]()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(session_clock::now() - from).count());
	};
	const auto end = [&](const char* reason, u32 how)
	{
		return end_spectator_standby(root, from, reason, how);
	};
	const auto identity = Emu.PBDGetCheckpointBootIdentity();
	if (!spectator_start_clean() || executable_sha256().empty())
		return end("clean_start_unverified", guard_failed);
	const standby::ready ready{data.generation, ppu.id, identity->sha256, executable_sha256(),
		pbd::env::find("PBD_MATCH_REPLAY_RECIPE_SHA256"), session_runtime.watcher_policy()};
	if (!standby::publish(root / standby::ready_file, standby::ready_text(ready)))
		return end("report_publish_failed", guard_failed);
	pbd_session_log.notice("Spectator standby: clean character select proved; waiting for a set.");
	vm::temporary_unlock(ppu);
	if (pbd::replay_standby::pbd_requested())
	{
		auto binding = pbd::replay_standby::pbd_read(root);
		standby::bind_wait waiting;
		for (u32 polls = 1; !binding.value; ++polls)
		{
			if (pbd::replay_standby::pbd_shared() && std::filesystem::exists(root / "spectator-session.json"))
			{
				if (std::filesystem::exists(root / pbd::replay_standby::pbd_bind_file))
					return end("standby_bind_conflict", guard_failed);
				break;
			}
			if (!waiting.again(binding.error, session_clock::now()))
				return end(binding.error, guard_failed);
			if (pbd_emulation_stopping(ppu))
				return end("emulation_stopped", emulation_stopped);
			if (polls % 10 == 0)
				rsx::set_native_ui_flip();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			binding = pbd::replay_standby::pbd_read(root);
		}
		if (binding.value)
		{
			if (pbd::replay_standby::pbd_shared() && std::filesystem::exists(root / "spectator-session.json"))
				return end("standby_bind_conflict", guard_failed);
			if (const char* refusal = pbd::replay_standby::pbd_identity_refusal(*binding.value, ready))
				return end(refusal, guard_failed);
			if (data.session.seed_set || !data.lobby.direct_match.pbd_configure_file(data.generation,
											 session_runtime.watcher_mode(), binding.value->seed, binding.value->file, binding.value->replay))
				return end("replay_standby_file_refused", guard_failed);
			data.session.seed = binding.value->seed;
			data.session.seed_set = true;
			if (!pbd::replay_standby::pbd_bind_presentation(root, *binding.value))
				return end("replay_standby_presentation_conflict", guard_failed);
			if (!standby::publish(root / pbd::replay_standby::pbd_bound_file,
					pbd::replay_standby::pbd_bound_text(*binding.value)))
				return end("replay_standby_report_failed", guard_failed);
			pbd_session_log.notice("Replay standby: completed game bound after %u ms.", waited_ms());
			return false;
		}
		// Live watching owns the same held process, without local playback controls or rewind capture.
		data.replay.controls = false;
		data.replay.viewer_enabled = false;
		data.replay.takeover = false;
		data.replay.takeover_fast = false;
		data.rewind.enabled = false;
	}
	auto manifest = pbd::spectator::read_manifest(root);
	standby::bind_wait waiting;
	for (u32 polls = 1; !manifest.value; ++polls)
	{
		if (!waiting.again(manifest.error, session_clock::now()))
		{
			pbd_session_log.error("Spectator standby: the bound manifest was refused (%s).", manifest.error);
			return end("spectator_manifest_invalid", guard_failed);
		}
		if (pbd_emulation_stopping(ppu))
			return end("emulation_stopped", emulation_stopped);
		if (polls % 10 == 0)
			rsx::set_native_ui_flip();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		manifest = pbd::spectator::read_manifest(root);
	}
	// A standby starts without PBD_MATCH_SEED, so the first fight frame's reseed needs the bound set's seed here.
	if (!pbd::fn_hook::bind_match_seed(manifest.value->seed))
		return end("spectator_seed_bind_failed", guard_failed);
	data.session.seed = manifest.value->seed;
	data.session.seed_set = true;
	if (const char* refusal = session_runtime.accept_spectator(root, std::move(*manifest.value)))
		return end(refusal, guard_failed);
	if (pbd::replay_standby::pbd_shared() && !pbd::held_presentation::output().bound())
		return end("spectator_standby_presentation_conflict", guard_failed);
	pbd_session_log.notice("Spectator standby: bound to a set after %u ms; the start sync applies its seed.", waited_ms());
	return false;
}

bool pbd::rollback_session::boundary_call::end_spectator_standby(const std::filesystem::path& root, session_clock::time_point from, const char* reason, u32 how)
{
	const auto waited_ms = [&]()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(session_clock::now() - from).count());
	};
	namespace standby = pbd::spectator_standby;
	standby::publish(root / standby::end_file, standby::end_text(reason, waited_ms()));
	pbd_session_log.warning("Spectator standby ended unbound after %u ms: %s", waited_ms(), reason);
	session_runtime.finish(how);
	data.end_callback();
	return true;
}
