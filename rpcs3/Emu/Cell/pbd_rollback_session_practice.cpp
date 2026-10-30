// Practice's dummy recording and its helpers: the slots, the take in progress, the sides and moves read from the
// game, and the dummy log; Practice's memory of its settings and Continue's select.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_practice_lesson_channel.h"
#include "../PBD/pbd_dummy_move_runtime.h"

u32 pbd::rollback_session::boundary_call::practice_dummy_saved()
{
	u32 count = 0;
	for (const auto& slot : data.practice.dummy.slots)
		count += !slot.frames.empty() || !slot.moves.empty();
	return count;
}

std::string pbd::rollback_session::boundary_call::practice_record_name()
{
	return fmt::format("Dummy Action %u", data.practice.dummy.slot_index + 1);
}

u32 pbd::rollback_session::boundary_call::practice_record_log_slot()
{
	return data.practice.dummy.slot_index + 1;
}

// The Practice Settings row (in Defense) of the slot whose take is being recorded.
u32 pbd::rollback_session::boundary_call::practice_record_row()
{
	return pbd::practice::practice_rows::first + data.practice.dummy.slot_index;
}

// Measures the neutral ends of the take in progress, inside the stop chord's tail.
void pbd::rollback_session::boundary_call::practice_measure_pending()
{
	data.practice.dummy.pending.leading = static_cast<u32>(pbd::practice::controls::leading_neutral_frames(practice_pending_body()));
	data.practice.dummy.pending.trailing = static_cast<u32>(pbd::practice::controls::trailing_neutral_frames(practice_pending_body()));
}

// Empties the take in progress (its words, neutral ends and stop-chord tail).
void pbd::rollback_session::boundary_call::practice_clear_pending()
{
	data.practice.dummy.pending.words.clear();
	data.practice.dummy.pending.leading = 0;
	data.practice.dummy.pending.trailing = 0;
	data.practice.dummy.pending.chord = 0;
}

bool pbd::rollback_session::boundary_call::practice_can_change_situation()
{
	// An unfinished recording belongs to one continuous situation; moving its anchor would splice two worlds into one take.
	return data.practice.native.pause_valid && data.session.in_match && data.practice.dummy.state != dummy_state::recording;
}

// Recorded actions play in Defense only; Offense playback is its Movement alone.
bool pbd::rollback_session::boundary_call::practice_dummy_available()
{
	if (data.practice.training_mode != pbd::practice::training_mode::defense)
		return false;
	std::array<unsigned, 8> weights{};
	for (u32 i = 0; i < weights.size(); ++i)
		if (practice_slot_playable(data.practice.dummy.slots[i]))
			weights[i] = data.practice.dummy.slots[i].frequency;
	return pbd::practice::controls::playback_available(weights, data.practice.play_mode, data.practice.dummy.slot_index);
}

// Defense has recorded actions, but every one is at Frequency 0, so none is ever chosen.
bool pbd::rollback_session::boundary_call::practice_dummy_all_zero()
{
	if (data.practice.training_mode != pbd::practice::training_mode::defense)
		return false;
	std::array<bool, 8> playable{};
	std::array<u32, 8> frequencies{};
	for (u32 i = 0; i < playable.size(); ++i)
	{
		playable[i] = practice_slot_playable(data.practice.dummy.slots[i]);
		frequencies[i] = data.practice.dummy.slots[i].frequency;
	}
	return pbd::practice::every_action_at_zero(playable, frequencies);
}

u64 pbd::rollback_session::boundary_call::practice_dummy_digest(const std::vector<u16>& frames)
{
	u64 hash = 1469598103934665603ull;
	for (const u16 word : frames)
	{
		hash ^= word & 0xff;
		hash *= 1099511628211ull;
		hash ^= word >> 8;
		hash *= 1099511628211ull;
	}
	return hash;
}

void pbd::rollback_session::boundary_call::practice_dummy_log(std::string text)
{
	if (data.practice.dummy.file)
		data.practice.dummy.file.write(text.data(), text.size());
}

void pbd::rollback_session::boundary_call::practice_dummy_publish()
{
	if (!data.live_stats || !data.practice.dummy.enabled)
		return;
	const u32 shown_slot = data.practice.dummy.state == dummy_state::playing && data.practice.dummy.have_play_slot ?
	                           data.practice.dummy.play_slot :
	                           data.practice.dummy.slot_index;
	const auto& slot = data.practice.dummy.slots[shown_slot];
	const u32 frames = data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::confirm ?
	                       static_cast<u32>(data.practice.dummy.pending.words.size()) :
	                   data.practice.dummy.state == dummy_state::playing ? data.practice.dummy.play_pos :
	                                                                       static_cast<u32>(practice_slot_frames(slot).size());
	data.live_pub.practice_dummy(data.session.in_match, static_cast<u32>(data.practice.dummy.state), shown_slot, frames,
		data.practice.dummy.choice,
		data.practice.dummy.menu_row, slot.frequency, slot.interval, practice_dummy_saved());
}

u32 pbd::rollback_session::boundary_call::practice_dummy_random()
{
	u32 x = data.practice.dummy.rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	data.practice.dummy.rng = x ? x : 0x6d2b79f5u;
	return data.practice.dummy.rng;
}

// Samples both fighters' screen sides from DR's own camera projection. The reader is valid only on the owner thread at
// the frame boundary (it checks both), so it runs once per Practice boundary and the result serves that boundary's
// input conversion and is kept with the inputs it recorded.
// DR's own input sides are read beside it: they are what the game converts the next words with, and they need no camera
// or render state, so they answer on every fight boundary.
void pbd::rollback_session::boundary_call::practice_read_sides()
{
	const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
	pbd::fighter_side::screen_sides sides;
	if (pbd::fighter_side::read(ppu, generation, sides))
		data.practice.sides = sides;
	else
		data.practice.sides.reset();
	pbd::fighter_side::input_sides input;
	if (pbd::fighter_side::read_input(generation, input))
		data.practice.input_sides = input;
	else
		data.practice.input_sides.reset();
	std::optional<std::array<bool, 2>> known;
	if (data.practice.input_sides)
		known = data.practice.input_sides->on_right;
	else if (data.practice.sides)
		known = data.practice.sides->on_right;
	if (known)
		for (u32 fighter = 0; fighter < 2; ++fighter)
			data.practice.last_sides[fighter] = (*known)[fighter] ? pbd::practice::movement::screen_side::right : pbd::practice::movement::screen_side::left;
}

// practice_reset_sides: what DR's camera and screen-side rule read after a wall restart, so a restart that draws P1 on
// the 2P side shows whether the bones, the projection or the camera decided it. One row from inside the reset (the wall
// phase, before the exact pair is written) and one per fresh boundary for the first few frames after it. Read-only.
void pbd::rollback_session::boundary_call::practice_sides_row(u32 index, u32 after, const char* at,
	const pbd::game_lifecycle::sides_sample& sample, bool provisional)
{
	const auto real = [](u32 bits)
	{
		const float value = std::bit_cast<float>(bits);
		return std::isfinite(value) ? fmt::format("%.1f", static_cast<double>(value)) : std::string("null");
	};
	std::string fighters;
	for (u32 side = 0; side < 2; ++side)
	{
		const auto& fighter = sample.fighters[side];
		fighters += fmt::format("%s{\"bone\":[%s,%s],\"position\":[%s,%s],\"projected\":%d,\"right\":%u,\"move\":%u}", side ? "," : "",
			real(fighter.bone_x), real(fighter.bone_z), real(fighter.x), real(fighter.z), static_cast<s32>(fighter.projected),
			fighter.right, fighter.move);
	}
	const std::string sides = after && data.practice.sides ?
	                              fmt::format("[%s,%s]", json_bool(data.practice.sides->on_right[0]), json_bool(data.practice.sides->on_right[1])) :
	                              std::string("null");
	const std::string input_sides = after && data.practice.input_sides ?
	                                    fmt::format("[%s,%s]", json_bool(data.practice.input_sides->on_right[0]), json_bool(data.practice.input_sides->on_right[1])) :
	                                    std::string("null");
	practice_dummy_log(fmt::format("{\"type\":\"practice_reset_sides\",\"request_id\":%u,\"boundary\":%u,\"after\":%u,\"at\":\"%s\",\"target\":%u,\"provisional\":%s,\"fighters\":[%s],\"on_right\":%s,\"input_on_right\":%s,\"camera\":{\"mode\":%u,\"yaw\":%u,\"frames\":%u,\"eye\":[%s,%s,%s],\"target\":[%s,%s,%s]},\"flip\":[%u,%u]}\n",
		data.practice.restart.sides_request, index, after, at, data.practice.restart.sides_target, json_bool(provisional), fighters, sides, input_sides,
		sample.camera_mode, sample.camera_yaw, sample.camera_frames, real(sample.eye[0]), real(sample.eye[1]), real(sample.eye[2]),
		real(sample.target[0]), real(sample.target[1]), real(sample.target[2]), sample.flip_flag, sample.flip_mode));
}

// Called after practice_read_sides at every Practice boundary. The restart's own boundary is passed over (no frame has
// run since the reset); after it, only boundaries whose frame really ran are logged.
void pbd::rollback_session::boundary_call::practice_sides_follow(u32 index)
{
	if (!data.practice.restart.sides_request)
		return;
	if (!data.session.in_match)
	{
		data.practice.restart.sides_request = 0;
		return;
	}
	if (!data.practice.restart.sides_armed)
	{
		data.practice.restart.sides_armed = true;
		return;
	}
	if (!data.practice.frame_advanced)
		return;
	practice_sides_row(index, ++data.practice.restart.sides_logged, "boundary", pbd::game_lifecycle::read_sides(), data.practice.restart.sides_provisional);
	if (data.practice.restart.sides_logged >= data.practice_sides_rows)
		data.practice.restart.sides_request = 0;
}

// Fighter 0 is P1, fighter 1 is P2 (the dummy). The side DR converts that fighter's next directions with; the camera
// projection stands in only when DR's own side could not be read.
std::optional<pbd::practice::movement::screen_side> pbd::rollback_session::boundary_call::practice_side_of(u32 fighter)
{
	const bool* right = data.practice.input_sides ? &data.practice.input_sides->on_right[fighter] :
	                    data.practice.sides       ? &data.practice.sides->on_right[fighter] :
	                                                nullptr;
	if (!right)
		return std::nullopt;
	return *right ? pbd::practice::movement::screen_side::right : pbd::practice::movement::screen_side::left;
}

// practice-inputs.jsonl: each port's word as the game consumed it, written when it changes, for a frame that really ran
// (a fresh boundary whose game frame advanced; never a re-executed or paused one). The words are after Practice's own
// filters (SELECT chords, menus, the button map), so they are exactly what each fighter was given.
void pbd::rollback_session::boundary_call::practice_capture_inputs()
{
	auto& capture = data.practice.inputs;
	if (!capture.active())
		return;
	for (u32 port = 0; port < 2; ++port)
	{
		const u16 word = data.practice.history_words[port];
		if (capture.last[port] == word)
			continue;
		capture.last[port] = word;
		capture.append(pbd::practice::movement::input_row(data.practice.input_frame, data.practice.history_index, port + 1, word,
			data.practice.history_sides[port], port ? pbd::practice::movement::screen_side::right : pbd::practice::movement::screen_side::left));
	}
	// A held direction can leave the last burst buffered indefinitely because it produces no further rows.
	// Persist it once per second of advancing gameplay, including when a bounded capture later closes the process.
	if (data.practice.input_frame % 60 == 0)
		capture.flush();
}

// A Restart or a return to the remembered moment starts a new stretch of the capture; the next frame writes both
// ports' words again so each stretch reads on its own.
void pbd::rollback_session::boundary_call::practice_capture_mark(const char* kind, u32 index)
{
	pbd::practice::native::frame_reset();
	if (pbd::practice::lesson::current.capture.started)
		pbd::practice::lesson::current.capture.valid = pbd::practice::lesson::current.capture.active = false;
	auto& capture = data.practice.inputs;
	if (!capture.active())
		return;
	capture.last = {};
	data.practice.movement.moves_logged = {};
	capture.append(fmt::format("{\"type\":\"%s\",\"frame\":%u,\"boundary\":%u}\n", kind, data.practice.input_frame, index));
}

// Rebuilds the loop for the current choice; a loop that changes starts again from its first pass.
void pbd::rollback_session::boundary_call::practice_movement_refresh()
{
	// Offense plays its Movement Technique on its own (pbd::practice::movement::technique_tape), or with a plain input as
	// Action After a Hit or Block, that input once after the counted contact; Defense plays its movement between actions.
	const bool offense = data.practice.training_mode == pbd::practice::training_mode::offense;
	data.practice.movement.input_sequence = offense && data.practice.offense.after == pbd::practice::after_contact::movement ?
	                                            pbd::practice::movement::reaction_sequence(data.practice.offense.reaction_input) :
	                                        offense ? pbd::practice::movement::technique_sequence(practice_active_movement(), data.practice.offense.wavedash_speed) :
	                                                  pbd::practice::movement::generate(practice_active_movement(), data.practice.movement.wavedash_speed);
	data.practice.movement.pos = 0;
	data.practice.dummy.move_gate = 0;
	data.practice.movement.started = false;
}

// A slot with moves from the move list plays one of them, picked at random each time: this reads the slot's next move
// from the loaded list as a take (pbd::practice::dummy_move::move_words). A move that cannot be read leaves the one before.
void pbd::rollback_session::boundary_call::practice_pick_slot_move(u32 slot_index)
{
	if (slot_index >= data.practice.dummy.slots.size())
		return;
	auto& slot = data.practice.dummy.slots[slot_index];
	if (slot.moves.empty())
	{
		slot.move_sequence.clear();
		return;
	}
	u32 rng = practice_dummy_random();
	if (auto words = pbd::practice::dummy_move::move_words(slot.moves, rng))
		slot.move_sequence = std::move(*words);
}

// Reads every slot's next move again once another move list is loaded (a new dummy character or costume, a new
// emulation), so the slots play moves of the dummy in the fight.
void pbd::rollback_session::boundary_call::practice_slot_moves_refresh()
{
	if (!pbd::practice::dummy_move::catalog_available() || pbd::practice::dummy_move::current.epoch == data.practice.menu.slot_moves_epoch)
		return;
	data.practice.menu.slot_moves_epoch = pbd::practice::dummy_move::current.epoch;
	for (u32 i = 0; i < data.practice.dummy.slots.size(); ++i)
	{
		data.practice.dummy.slots[i].move_sequence.clear();
		practice_pick_slot_move(i);
	}
}

// Held Practice menus and release waits leave selection input neutral.
u16 pbd::rollback_session::boundary_call::practice_select_word(u16 raw)
{
	const bool held = data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm ||
	                  data.practice.dummy.ignore_until_release || data.takeover.ignore_pad_until_release ||
	                  data.practice.restart.reset_request.has_value();
	return held ? u16{0} : raw;
}

// The saved game options' restore holds DR's pause as Practice's own while it waits for the pause and the native call.
bool pbd::rollback_session::boundary_call::practice_memory_pause()
{
	const auto* memory = session_runtime.m_practice_memory.get();
	return memory && memory->restore && memory->restore_requested;
}

// Practice's memory at a fresh boundary: a new fight brings its dummy's saved rows and slots and the saved game options, and
// changes are looked for every save_interval boundaries. The tables that name a dummy's base character are read at the select.
void pbd::rollback_session::practice_steps::memory_boundary()
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory)
		return;
	u32 scene = 0;
	recording_set_poll();
	if (!memory->tables && read_word(pbd::t5dr::scene_address, scene) && scene == 3)
		memory->tables = pbd::hidden_selection::read_native_tables();
	if (data.practice.view.fight_live && memory->applied_match != data.session.matches && data.practice.dummy.state == dummy_state::idle)
		memory_fight();
	memory_restore();
	if (index >= memory->next_check || index + practice_store::save_interval < memory->next_check)
	{
		memory->next_check = index + practice_store::save_interval;
		memory_save();
	}
}

// A fight's first live boundary. Restore Defaults' values are taken before anything is restored,
// a new dummy brings its own rows and slots, and saved game options that differ are restored through a silent pause.
void pbd::rollback_session::practice_steps::memory_fight()
{
	auto& memory = *session_runtime.m_practice_memory;
	if (!data.practice.menu.game_defaults)
		data.practice.menu.game_defaults = practice_game_values();
	const auto native = pbd::practice::native::inspect(true);
	const bool human_right = native.practice && native.initiating == 0x10090910;
	u32 dummy_row = 0;
	const bool read = read_word(human_right ? 0x10093f50 : 0x10093f54, dummy_row) &&
	                  dummy_row <= practice_store::max_character_row;
	const u32 character = read && memory.tables ? memory.tables->characters[dummy_row].character : ~0u;
	if (data.practice.dummy.enabled && character < practice_store::base_characters && memory.dummy != character)
		memory_dummy(character);
	const auto current = memory.saved.options && data.practice.menu.game_defaults ? practice_game_values() : std::nullopt;
	if (current && *current != *memory.saved.options)
	{
		memory.restore = pbd::practice::pack_game_options(*memory.saved.options);
		memory.restore_from = index;
		memory.restore_requested = memory.restore_queued = false;
	}
	// Set last: the previous dummy's save above must not read this fight's options as its own.
	memory.applied_match = data.session.matches;
	practice_dummy_log(fmt::format("{\"type\":\"practice_memory_fight\",\"boundary\":%u,\"match\":%u,\"pick_read\":%s,"
								   "\"dummy\":%d,\"restore_options\":%s}\n",
		index, data.session.matches, json_bool(read), memory.dummy ? static_cast<int>(*memory.dummy) : -1,
		json_bool(memory.restore.has_value())));
}

// Slots follow the dummy character: the live dummy's rows and slots are saved first, then the new dummy's come back (or the
// slots start empty), and an Offense reaction naming an empty slot becomes None.
void pbd::rollback_session::practice_steps::memory_dummy(u32 character)
{
	auto& memory = *session_runtime.m_practice_memory;
	memory.set_editor.reset();
	memory.load_set_requested.reset();
	if (memory.dummy)
		memory_save();
	const auto found = memory.library.find(character);
	const auto slots = found != memory.library.end() ? found->second : practice_store::empty_recordings(character);
	const auto saved = memory.saved.dummies.find(character);
	auto rows = saved != memory.saved.dummies.end() ? saved->second : practice_store::dummy_rows{};
	practice_store::drop_empty_reaction(rows.offense, slots);
	memory.sets.begin_character({slots, rows.defense});
	memory_apply(rows, slots);
	memory.dummy = character;
	practice_dummy_log(fmt::format("{\"type\":\"practice_memory_dummy\",\"boundary\":%u,\"character\":%u,"
								   "\"saved_slots\":%s,\"filled\":%u}\n",
		index, character, json_bool(found != memory.library.end()), practice_dummy_saved()));
}

void pbd::rollback_session::practice_steps::memory_apply(const practice_store::dummy_rows& rows, const practice_store::recordings& slots)
{
	data.practice.reactions.wakeup = rows.wakeup;
	data.practice.reactions.wakeup_control.reset();
	data.practice.reactions.wakeup_suspended = false;
	data.practice.reactions.throw_escape = rows.throw_escape;
	data.practice.reactions.throw_escape_control.reset();
	pbd::practice::dummy_move::current_settings = rows.reactions;
	// Only a technique is a Movement Technique; a plain input saved by an older version (a sidestep, a crouch) is Neutral.
	data.practice.offense.movement = pbd::practice::movement::is_technique(rows.offense.movement) ? rows.offense.movement :
	                                                                                                pbd::practice::movement::preset::none;
	data.practice.offense.wavedash_speed = rows.offense.wavedash_speed;
	// Moves are the reaction exactly when moves are chosen: a Moves row without any reads as None, and moves set from the
	// launcher's picker make the row Moves.
	const auto& reaction = rows.offense.reaction;
	data.practice.offense.after = rows.reactions.enabled()                             ? pbd::practice::after_contact::moves :
	                              reaction.kind == pbd::practice::after_contact::moves ? pbd::practice::after_contact::none :
	                                                                                     reaction.kind;
	data.practice.offense.reaction_slot = (std::min)(reaction.slot, pbd::practice::reaction_slots - 1);
	data.practice.offense.reaction_input = reaction.input;
	data.practice.offense.counted = rows.offense.counted;
	data.practice.offense.times = rows.offense.times;
	data.practice.offense.movement_off = false;
	data.practice.contact.watching = false;
	data.practice.contact.ready = false;
	data.practice.contact.reaction_count.reset();
	data.practice.movement.preset = rows.defense.movement;
	data.practice.movement.wavedash_speed = rows.defense.wavedash_speed;
	data.practice.movement.wavedash_end = rows.defense.wavedash_end;
	data.practice.movement.dash_end = rows.defense.dash_end;
	data.practice.interval = slots.interval;
	for (u32 i = 0; i < practice_store::slot_count; ++i)
	{
		auto& slot = data.practice.dummy.slots[i];
		const auto& saved = slots.slots[i];
		slot.frames.assign(saved.words.begin(), saved.words.end());
		slot.side.reset();
		slot.leading_neutral_frames = saved.leading_neutral;
		slot.trailing_neutral_frames = saved.trailing_neutral;
		slot.stop_chord_frames = saved.stop_chord;
		slot.trim_neutral = saved.trim;
		slot.enabled = saved.enabled;
		slot.name = saved.name;
		slot.frequency = saved.frequency;
		slot.interval = saved.interval;
		slot.moves = saved.moves;
		slot.move_sequence.clear();
	}
	data.practice.menu.slot_moves_epoch = ~0u;
	practice_movement_refresh();
}

pbd::practice_store::dummy_rows pbd::rollback_session::practice_steps::memory_rows()
{
	practice_store::dummy_rows rows;
	rows.wakeup = data.practice.reactions.wakeup;
	rows.throw_escape = data.practice.reactions.throw_escape;
	rows.reactions = pbd::practice::dummy_move::current_settings;
	rows.offense = {data.practice.offense.movement, data.practice.offense.wavedash_speed,
		{data.practice.offense.after, data.practice.offense.reaction_slot, data.practice.offense.reaction_input}, data.practice.offense.counted,
		data.practice.offense.times};
	rows.defense = {data.practice.movement.preset, data.practice.movement.wavedash_speed, data.practice.movement.wavedash_end, data.practice.movement.dash_end};
	return rows;
}

pbd::practice_store::recordings pbd::rollback_session::practice_steps::memory_slots(u32 character)
{
	auto out = practice_store::empty_recordings(character);
	out.interval = data.practice.interval;
	for (u32 i = 0; i < practice_store::slot_count; ++i)
	{
		const auto& slot = data.practice.dummy.slots[i];
		auto& saved = out.slots[i];
		saved.words = slot.frames;
		saved.leading_neutral = slot.leading_neutral_frames;
		saved.trailing_neutral = slot.trailing_neutral_frames;
		saved.stop_chord = slot.stop_chord_frames;
		saved.trim = slot.trim_neutral;
		saved.enabled = slot.enabled;
		saved.name = slot.name;
		saved.frequency = slot.frequency;
		saved.interval = slot.interval;
		saved.moves = slot.moves;
	}
	return out;
}

// Builds the profile and the live dummy's slots from the current settings and hands a change to the writer, which makes the text
// and writes it off this thread. The game's options count only once the fight's saved ones are back in place.
void pbd::rollback_session::practice_steps::memory_save()
{
	auto& memory = *session_runtime.m_practice_memory;
	auto profile = memory.saved;
	profile.mode = data.practice.training_mode == pbd::practice::training_mode::defense ? pbd::practice::training_mode::defense :
	                                                                                      pbd::practice::training_mode::offense;
	const auto& view = data.practice.view;
	profile.view = {view.command_history, view.attack_info, view.frame_data, view.control_hints, view.movement_streaks};
	profile.restart = practice_store::saved_restart(data.practice.restart.position);
	profile.dummy_back = data.practice.restart.variant.dummy_back;
	profile.switch_positions = data.practice.restart.variant.switched;
	profile.input_delay = data.practice.input_delay_frames;
	profile.guard = data.practice.reactions.guard.option;
	profile.counter_hit_random = data.practice.reactions.counter_hit.random;
	const auto values = memory.applied_match == data.session.matches && !memory.restore ? practice_game_values() : std::nullopt;
	if (values)
		profile.options = *values;
	if (memory.dummy)
		profile.dummies[*memory.dummy] = memory_rows();
	std::optional<practice_store::recordings> slots;
	if (memory.dummy && data.practice.dummy.enabled)
		slots = memory_slots(*memory.dummy);
	memory.autosave(std::move(profile), std::move(slots), pbd::set_channel::utc_now());
}

// The saved game options go back through the game's own commit while DR's pause is Practice's and no menu is drawn: a pause is
// asked for once the fight allows one, the restore is queued once the game has paused, and the fight resumes after it has run.
// One that cannot run within restore_limit boundaries, or outlives its fight, is given up.
void pbd::rollback_session::practice_steps::memory_restore()
{
	auto& memory = *session_runtime.m_practice_memory;
	if (!memory.restore)
		return;
	if (index - memory.restore_from > practice_store::restore_limit || !data.session.in_match)
	{
		memory_restore_done("not_run");
		return;
	}
	const bool calm = !data.practice_resume_settling() && !data.practice.restart.reset_request && data.practice.native.screen == 0 &&
	                  data.practice.native.pause_valid;
	if (!memory.restore_requested)
	{
		if (calm && !data.practice.menu.open && data.practice.dummy.state == dummy_state::idle && !data.practice.native.paused &&
			!data.practice.pause.requested && !data.practice.pause.resume_native)
		{
			memory.restore_requested = true;
			data.practice.pause.requested = true;
			data.practice.pause.injected = false;
		}
		return;
	}
	if (!memory.restore_queued)
	{
		if (calm && data.practice.native.paused && !data.practice.native.request)
		{
			queue_native(pbd::practice::native::operation::option_restore, *memory.restore);
			memory.restore_queued = true;
		}
		return;
	}
	if (!data.practice.native.request)
		memory_restore_done(nullptr);
}

void pbd::rollback_session::practice_steps::memory_restore_done(const char* reason)
{
	auto& memory = *session_runtime.m_practice_memory;
	const bool requested = memory.restore_requested;
	memory.restore.reset();
	memory.restore_requested = memory.restore_queued = false;
	// A menu the player opened meanwhile keeps the pause; otherwise the fight resumes through DR's own START.
	if (requested && !data.practice.menu.open)
	{
		data.practice.pause.requested = false;
		if (data.practice.native.paused)
			resume_native();
	}
	practice_dummy_log(fmt::format("{\"type\":\"practice_memory_options\",\"boundary\":%u,\"outcome\":\"%s\"}\n", index,
		reason ? reason : "restored"));
}
