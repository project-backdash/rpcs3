#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_practice_frames.h"
#include "pbd_practice_start.h"

namespace start = pbd::practice::start;

void pbd::rollback_session::practice_steps::observe_frame()
{
	const auto pause = read_practice_pause();
	data.practice.native.ran = pbd::practice::controls::frame_ran(data.practice.boundary_counter_valid, data.practice.boundary_counter,
		pause.valid, pause.counter);
	data.practice.native.pause_valid = pause.valid;
	data.practice.native.flag = pause.paused;
	data.practice.native.menu_word = pause.valid && pause.menu != 0;
	data.practice.native.paused = pbd::practice::controls::native_paused(pause.paused, data.practice.native.ran);
	log_practice_select(index);
	// Gameplay runs after input conversion. F05C advances exactly once between entrances only for an unpaused Practice frame.
	const bool advancing = data.practice.history_pending && data.practice.boundary_counter_valid && pause.valid &&
	                       pause.counter == data.practice.boundary_counter + 1;
	const bool counter_jump = data.practice.history_pending && data.practice.boundary_counter_valid && pause.valid &&
	                          pause.counter != data.practice.boundary_counter && pause.counter != data.practice.boundary_counter + 1;
	data.practice.frame_advanced = advancing;
	if (counter_jump)
		data.practice.precision.reset();
	if (counter_jump && pbd::practice::lesson::current.capture.started)
		pbd::practice::lesson::current.capture.valid = pbd::practice::lesson::current.capture.active = false;
	update_input_history(advancing);
	discard_unplayed_input(pause, advancing);
	data.practice.history_pending = false;
	data.practice.record_word_valid = false;
	data.practice.play_before.valid = false;
	data.practice.boundary_counter_valid = pause.valid;
	data.practice.boundary_counter = pause.counter;
	data.practice.view.inputs = data.practice.history.inputs();
	update_damage(pause, advancing, counter_jump);
	// The panels follow the Practice fight dispatcher, hiding at K.O. and selection. This path runs only on fresh
	// boundaries, so the panels describe presented frames rather than correction re-execution.
	{
		u32 phase_mode = 0, phase_scene = 0, phase_pending = 0;
		data.practice.view.fight_live = read_word(pbd::t5dr::game_mode_address, phase_mode) &&
		                                read_word(pbd::t5dr::scene_address, phase_scene) &&
		                                read_word(0x10093230, phase_pending) && pbd::practice::practice_fight(phase_mode, phase_scene, phase_pending);
	}
	memory_boundary();
	pbd::practice::native::frame_boundary(pause.counter, pause.valid && data.practice.view.fight_live);
	for (const auto& event : pbd::practice::native::frame_events())
		practice_dummy_log(fmt::format("{\"type\":\"practice_frame_event\",\"boundary\":%u,\"counter\":%u,\"pc\":%u,\"side\":%u,\"move\":%u,\"cursor\":%u,\"queued\":%u,\"state\":%u,\"flags\":%u,\"window_start\":%u,\"window_end\":%u,\"target\":%u}\n",
			index, event.counter, event.pc, event.side, event.move, event.cursor, event.queued, event.state, event.flags,
			event.window_start, event.window_end, event.target));
	pbd::practice::native::frame_events_clear();
	publish_frame_data(pause);
	if (!data.practice.rewind_quiet)
		practice_dummy_log(fmt::format("{\"type\":\"practice_boundary\",\"index\":%u,\"boundary\":%u,\"counter\":%u,\"pause_read\":%s,\"paused\":%s,\"native_pause_state\":%u,\"native_pause_request\":%u,\"native_menu_state\":%u,\"session_mode\":%u,\"advancing\":%s}\n",
			index, index, pause.counter, json_bool(pause.valid), json_bool(pause.paused), pause.blocked, pause.request, pause.menu, pause.mode, json_bool(advancing)));
}

void pbd::rollback_session::practice_steps::update_movement_streak()
{
	{
		const u16 word = data.practice.history_words[0];
		const auto& side = data.practice.history_sides[0];
		if (const auto ended = data.practice.streak.frame(side ? pbd::practice::movement::oriented(word, *side) : word))
		{
			u32 p1_row = umax;
			const bool has_move = ended->what != pbd::practice::streak::kind::wavedash || !read_word(0x10093f50, p1_row) || p1_row > 0xff ||
			                      pbd::practice::movement::wavedash_character(p1_row);
			const bool shown = data.practice.view.movement_streaks && has_move;
			const char* kind = ended->what == pbd::practice::streak::kind::wavedash      ? "wavedash" :
			                   ended->what == pbd::practice::streak::kind::single_cancel ? "single_cancel" :
			                                                                               "backdash_cancel";
			practice_dummy_log(fmt::format("{\"type\":\"practice_streak\",\"boundary\":%u,\"kind\":\"%s\",\"count\":%u,\"per_second\":%.3f,\"cancel_frames\":%.1f,\"shown\":%s}\n",
				index, kind, ended->count, ended->per_second, ended->cancel_frames, json_bool(shown)));
			if (shown)
			{
				data.practice.view.toast = pbd::practice::streak::text(*ended);
				data.practice.menu.toast_until = session_clock::now() + std::chrono::milliseconds(pbd::practice::streak::shown_frames * 1000 / 60);
			}
		}
	}
}

void pbd::rollback_session::practice_steps::update_input_precision()
{
	namespace precision = pbd::practice::precision;
	namespace moves = pbd::practice::dummy_move;
	const auto& side = data.practice.history_sides[0];
	if (!side || data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready ||
		!pbd::practice::native::boundary_readable(ppu))
	{
		data.practice.precision.reset();
		return;
	}
	constexpr u32 actor = 0x10090040;
	const u32 bank = moves::native_word(actor + 0x50), allocation = moves::native_word(actor + 0xc4);
	precision::performed performed;
	if (bank >= 0x10000000 && bank <= 0x7ffffd00 &&
		pbd::move_list::table_member(bank, 0x1e0, 0x4c, allocation, 8192, moves::native_word) &&
		pbd::practice::frames::attack_window(moves::native_half(allocation + 0x44), moves::native_half(allocation + 0x46), moves::native_half(allocation + 0x24)))
	{
		performed = {allocation, moves::native_half(actor + 0x96), moves::native_half(allocation + 0x44),
			precision::move_name(moves::native_word(allocation), [](u32 address) -> u32
				{
					u8 value = 0;
					return vm::try_access(address, &value, sizeof(value), false) ? value : umax;
				})};
	}
	data.practice.precision.frame(pbd::practice::movement::oriented(data.practice.history_words[0], *side),
		moves::native_half(actor + 0x42), performed);
}

void pbd::rollback_session::practice_steps::discard_unplayed_input(const practice_pause_observation& pause, bool advancing)
{
	if (data.practice.play_before.valid && !advancing && data.practice.dummy.state == dummy_state::playing &&
		data.practice.dummy.preview == data.practice.play_before.preview)
	{
		const auto& before = data.practice.play_before;
		data.practice.dummy.have_play_slot = before.have_slot;
		data.practice.dummy.play_slot = before.slot;
		data.practice.dummy.play_pos = before.position;
		data.practice.dummy.wait = before.wait;
		data.practice.dummy.rng = before.rng;
		data.practice.dummy.cycles = before.cycles;
		data.practice.movement.pos = before.movement;
		data.practice.dummy.phase = before.phase;
		data.practice.dummy.gate = before.gate;
		data.practice.dummy.gap_over = before.gap_over;
		data.practice.dummy.neutral_run = before.neutral_run;
		data.practice.dummy.caps = before.caps;
		data.practice.dummy.moved = before.moved;
		data.practice.dummy.facing.caps = before.facing_caps;
		data.practice.dummy.move_gate = before.move_gate;
		data.practice.movement.started = before.movement_started;
		data.practice.dummy.facing.proven = before.facing_proven;
		data.practice.dummy.turn.frames = before.turn;
		data.practice.dummy.flow_given = before.flow_given;
		data.practice.dummy.play_first = before.play_first;
		data.practice.dummy.flow = before.flow;
		practice_dummy_log(fmt::format("{\"type\":\"dummy_preparation_discard\",\"boundary\":%u,\"source_boundary\":%u,\"position\":%u,\"pause_read\":%s,\"paused\":%s}\n",
			index, data.practice.history_index, before.position, json_bool(pause.valid), json_bool(pause.paused)));
	}
}

void pbd::rollback_session::practice_steps::update_input_history(bool advancing)
{
	if (advancing)
	{
		data.practice.history.sample(data.practice.history_index, data.practice.history_words);
		++data.practice.input_frame;
		// P1's own movement streaks, read toward and away from the opponent on the side P1 was read on. A wavedash
		// streak shows only for a fighter with a crouch dash (P1's character-table row); an unreadable row holds none back.
		update_movement_streak();
		update_input_precision();
		practice_capture_inputs();
		// The take keeps each word relative to the side DR read it with (practice_dummy_slot).
		if (data.practice.record_word_valid && data.practice.dummy.state == dummy_state::recording &&
			data.practice.dummy.pending.words.size() < 1800)
		{
			if (data.practice.dummy.pending.words.empty())
				data.practice.dummy.pending.side = data.practice.record_side;
			data.practice.dummy.pending.words.push_back(pbd::practice::movement::record_word(data.practice.record_word, data.practice.record_side));
		}
	}
}

void pbd::rollback_session::practice_steps::update_damage(const practice_pause_observation& pause, bool advancing, bool counter_jump)
{
	std::array<u32, 2> health{};
	const bool health_ok = advancing && read_word(0x10090040 + 0x6ac, health[0]) && read_word(0x10090910 + 0x6ac, health[1]) &&
	                       health[0] <= (1000u << 16) && health[1] <= (1000u << 16);
	// Once the health reader passes, the totals are readings (0 until the first hit) rather than "---".
	if (health_ok)
		for (auto& fighter : data.practice.view.fighters)
		{
			if (!fighter.total_damage)
				fighter.total_damage = 0u;
			if (!fighter.hits)
				fighter.hits = 0u;
			if (!fighter.combo_damage)
				fighter.combo_damage = 0u;
		}
	if (health_ok && data.practice.health_valid)
	{
		std::array<u32, 2> dealt{};
		for (u32 side = 0; side < 2; ++side)
			if (health[side] < data.practice.health_previous[side])
			{
				// A drop in one fighter's health is the other fighter's damaging hit.
				auto& attacker = data.practice.view.fighters[1 - side];
				const u32 damage = (data.practice.health_previous[side] - health[side] + 0xffff) >> 16;
				dealt[1 - side] = damage;
				attacker.damage = damage;
				attacker.total_damage = attacker.total_damage.value_or(0u) + damage;
				// Fighter 1 is the dummy: a hit for Offense's Action After a Hit or Block.
				if (side == 1)
					data.practice.dummy.hit = true;
			}
		// Hits and Combo Damage follow each attacker's latest combo. Only the dummy has a rest reading to end P1's.
		for (u32 attacker = 0; attacker < 2; ++attacker)
		{
			auto& combo = data.practice.combos[attacker];
			combo.frame(dealt[attacker], attacker == 0 ? data.practice.dummy.rest : std::nullopt);
			data.practice.view.fighters[attacker].hits = combo.hits;
			data.practice.view.fighters[attacker].combo_damage = combo.damage;
		}
	}
	if (health_ok)
	{
		data.practice.health_previous = health;
		data.practice.health_valid = true;
	}
	else if (advancing || !pause.valid || counter_jump)
		data.practice.health_valid = false;
}

void pbd::rollback_session::practice_steps::publish_frame_data(const practice_pause_observation& pause)
{
	{
		static thread_local std::array<std::array<u32, 2>, 2> previous{};
		const auto number = [](const auto& value)
		{
			return value ? std::to_string(*value) : std::string("null");
		};
		pbd::practice::native::frame_feedback(data.practice.view.fighters);
		const auto& frames = pbd::practice::native::frame_results();
		const auto& calculated = pbd::practice::native::frame_calculated();
		const auto& references = pbd::practice::native::frame_references();
		for (u32 side = 0; side < 2; ++side)
		{
			const auto& reading = frames[side];
			const auto& reached = calculated[side];
			auto& shown = data.practice.view.fighters[side];
			const bool current = reached.move != 0 && reached.move == reading.move;
			// Hit Properties belongs to the same contact as Hit / Block, so it shows and clears with it.
			shown.hit_level = current && reached.contacted ? pbd::practice::frames::hit_level_text(reached.level) : std::string_view{};
			shown.calculated = reading.move == references[side].allocation ? references[side] : pbd::practice::reference::result{};
			if (previous[side][0] == reading.revision && previous[side][1] == reached.revision)
				continue;
			previous[side] = {reading.revision, reached.revision};
			practice_dummy_log(fmt::format("{\"type\":\"practice_frame_data\",\"boundary\":%u,\"counter\":%u,\"side\":%u,\"startup_qualified\":true,\"advantage_qualified\":true,\"move\":%u,\"startup\":%s,\"on_block\":%s,\"on_hit\":%s,\"on_whiff\":%s,\"whiff_last_active\":%s,\"whiff_ready\":%s,\"contact\":%u,\"impact\":%s,\"attacker_ready\":%s,\"defender_ready\":%s,\"contact_flags\":%u,\"pending_reaction\":%u,\"defender_state\":%u,"
										   "\"calculated_move\":%u,\"calculated_contacted\":%s,\"calculated_contact\":%u,\"calculated_reaction\":%u,\"calculated_advantage\":%s,\"calculated_attacker_ready\":%s,\"calculated_defender_ready\":%s,\"calculated_label\":%u,\"calculated_level\":%u}\n",
				index, pause.counter, side, reading.move, number(reading.startup), number(reading.on_block),
				number(reading.on_hit), number(reading.on_whiff), number(reading.whiff_last_active), number(reading.whiff_ready), static_cast<u32>(reading.outcome), number(reading.impact), number(reading.attacker_ready), number(reading.defender_ready),
				reading.contact_flags, reading.pending_reaction, reading.defender_state,
				reached.move, json_bool(reached.contacted), static_cast<u32>(reached.outcome), reached.reaction, number(reached.advantage), number(reached.attacker_ready), number(reached.defender_ready),
				static_cast<u32>(reached.label), reached.level));
			const auto& reference = shown.calculated;
			practice_dummy_log(fmt::format("{\"type\":\"practice_reference_data\",\"boundary\":%u,\"side\":%u,\"move\":%u,\"startup\":%s,\"active\":%s,\"recovery\":%s,\"block_stun\":%s,\"hit_stun\":%s,\"on_block\":%s,\"on_hit\":%s}\n",
				index, side, reference.allocation, number(reference.startup), number(reference.active), number(reference.recovery),
				number(reference.block_stun), number(reference.hit_stun), number(reference.on_block), number(reference.on_hit)));
			// The readiness behind the text; "*" follows the number of the branch that was taken.
			std::string logged = shown.advantage_text;
			if (shown.advantage_marked_size)
				logged.insert(shown.advantage_marked_at + shown.advantage_marked_size, "*");
			practice_dummy_log(fmt::format("{\"type\":\"practice_frame_readiness\",\"boundary\":%u,\"side\":%u,\"move\":%u,\"text\":\"%s\",\"attacker_state\":%u,\"defender_state\":%u,"
										   "\"attacker_alt\":%s,\"defender_alt\":%s,\"alternative\":%s,\"whiff_alternative\":%s,\"defender_chooses\":%s,\"taken\":%u}\n",
				index, side, reached.move, logged, static_cast<u32>(reached.attacker_state), static_cast<u32>(reached.defender_state),
				number(reached.attacker_alt), number(reached.defender_alt), number(reached.alternative), number(reached.whiff_alternative),
				json_bool(reached.defender_chooses), static_cast<u32>(reached.taken)));
		}
		judge_punishment(pause);
	}
}

void pbd::rollback_session::practice_steps::judge_punishment(const practice_pause_observation& pause)
{
	const auto& calculated = pbd::practice::native::frame_calculated();
	const auto number = [](const auto& value)
	{
		return value ? std::to_string(*value) : std::string("null");
	};
	// Punishment practice (pbd::practice::punish): after the player blocks one of the dummy's attacks at -10 or worse, the
	// player's first attack to connect says whether it landed before the dummy could guard. It shows with the Frame Data
	// panels and is logged either way. While the player drives the dummy (ready to record, recording) the player's own
	// fighter stands idle, so nothing is judged.
	const bool player_drives_dummy = data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready;
	const auto judged = data.practice.view.fight_live && pause.valid && !player_drives_dummy ?
	                        data.practice.punish.boundary(calculated[0], calculated[1], pause.counter) :
	                        std::nullopt;
	if (judged)
	{
		const bool shown = data.practice.view.frame_data;
		practice_dummy_log(fmt::format("{\"type\":\"practice_punish\",\"boundary\":%u,\"counter\":%u,\"punished\":%s,\"advantage\":%d,\"startup\":%s,\"frames\":%u,\"blocked\":%s,\"shown\":%s}\n",
			index, pause.counter, json_bool(judged->outcome == pbd::practice::punish::verdict::punished), judged->advantage, number(judged->startup),
			judged->frames, json_bool(judged->blocked), json_bool(shown)));
		if (shown)
		{
			data.practice.view.toast = pbd::practice::punish::text(*judged);
			data.practice.menu.toast_until = session_clock::now() + std::chrono::milliseconds(pbd::practice::punish::shown_frames * 1000 / 60);
		}
	}
}
