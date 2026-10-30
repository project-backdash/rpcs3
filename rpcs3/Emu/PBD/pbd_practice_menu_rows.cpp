// Practice menu rows and their published state.
#include "stdafx.h"
#include "pbd_practice_menu_builder.h"

namespace category = pbd::practice::categories;
namespace practice_rows = pbd::practice::practice_rows;
using pbd::practice::prompt_action;
using pbd::practice::training_mode;

void pbd::rollback_session::practice_steps::native_menu::decision_rows()
{
	auto& bar = data.practice.view.menu_prompts;
	// "Record this movement?": Yes keeps the take in its slot, Record again goes back to the ready state, and
	// Discard drops it. Yes and Discard return to the Defense rows of Practice Settings.
	const auto slot_name = practice_record_name();
	const auto& slot = practice_record_slot();
	const auto recording = pbd::practice::controls::playback_frames(pbd::practice::controls::take_body(slot.frames, slot.stop_chord_frames),
		slot.leading_neutral_frames, slot.trailing_neutral_frames, slot.trim_neutral);
	const std::string replaces = slot.frames.empty() ? std::string{} : fmt::format(", replacing its %.1f s recording", recording.size() / 60.0);
	data.practice.view.dialog_title = "Record this movement?";
	row("Yes", "", !data.practice.dummy.pending.words.empty(),
		fmt::format("Save this %.1f s take to %s%s.", data.practice.dummy.pending.words.size() / 60.0, slot_name, replaces));
	row("Record again", "", data.practice.native.pause_valid, "Discard this take and record a new one.");
	row("Exit without recording", "", true, fmt::format("Discard this take and keep %s as it was.", slot_name));
	panel.selected = data.practice.dummy.choice;
	bar.add("{cross}", "Confirm", prompt_action::confirm);
}

void pbd::rollback_session::practice_steps::native_menu::offense_options()
{
	// The game's own practice options. They stay locked while the dummy records or plays, the Offense
	// Movement included (it drives the dummy through the game's Controller action meanwhile).
	const bool idle = data.practice.dummy.state == dummy_state::idle;
	const std::string locked = data.practice.dummy.state == dummy_state::playing && practice_dummy_reacting() ?
	                               std::string("The dummy is playing its reaction: let it finish, or {select}+{circle} in the fight, first.") :
	                           data.practice.dummy.state == dummy_state::playing ?
	                               std::string("The dummy is playing its Movement: set Movement to Neutral, or {select}+{circle} in the fight, first.") :
	                               busy;
	const auto game_option = [&](u32 option, const char* label, const char* help)
	{
		choice_row(practice_rows::first + option, label, idle, idle ? std::string(help) : locked);
	};
	game_option(0, "Dummy Action", "What the dummy does between your attacks.");
	game_option(1, "Counter Hit", "Make the hits you land count as counter hits.");
	game_option(2, "Game Input Display", "Show the game's own input display.");
	game_option(3, "Game Attack Data", "Show the game's own attack data.");
	// An unset Attack Data (0) shows as Not Set; its dropdown still opens on Type 1, the value it writes.
	if (rows.back().detail == "Type 1")
	{
		const auto game = pbd::practice::native::inspect(true);
		u32 attack = umax;
		if (game.practice && read_word(game.practice + 0x14, attack) && attack == 0)
			rows.back().detail = "Not Set";
	}
	game_option(4, "Game Hit Analysis", "Show the game's own hit analysis.");
	choice_row(practice_rows::wakeup, "Wake-up", true,
		"How the dummy gets up after a knockdown. Unavailable choices use Stand Up; Random includes Stay Down.");
	choice_row(practice_rows::throw_escape, "Throw Escape", true,
		"Whether the dummy breaks your throws. Back throws can't be broken.");
}

void pbd::rollback_session::practice_steps::native_menu::offense_reaction()
{
	// The dummy's own movement technique, which replaces Dummy Action while it plays; toward and away are always
	// relative to the player. With an Action After a Hit or Block the dummy follows Dummy Action between its
	// reactions, so the technique waits, dimmed, for None.
	reacts = data.practice.offense.after != pbd::practice::after_contact::none;
	const std::string reaction_waits = "The dummy follows Dummy Action between its reactions: set Action After a Hit or Block to None to use a Movement Technique.";
	choice_row(practice_rows::movement, "Movement Technique", !reacts, reacts ? reaction_waits : "The technique the dummy keeps doing on its own instead of Dummy Action; {select}+{circle} turns it off and on in the fight.");
	rows.back().gap = 8;
	const bool offense_wavedash = pbd::practice::movement::is_wavedash(data.practice.offense.movement);
	choice_row(practice_rows::wavedash_speed, "Wavedash Speed", offense_wavedash && !reacts, reacts ? reaction_waits : offense_wavedash ? "How many crouch dashes a second the dummy's wavedash makes." :
																																		  "Choose Wavedash for Movement Technique first.");
	// The row shows the reaction itself: the input, the recorded Dummy Action or the chosen moves.
	reaction_name = practice_reaction_name();
	choice_row(practice_rows::after_contact, "Action After a Hit or Block", true,
		"What the dummy does once it recovers from the hits or blocks counted below: a movement, moves from its move list, or one of your recorded Dummy Actions.");
	rows.back().detail = reaction_name;
	// Count and After say when the reaction comes; with None they do nothing, so they are dimmed.
	choice_row(practice_rows::counted, "Count", reacts, reacts ? "Which of your attacks count toward the dummy's reaction: blocked ones, ones that hit, or both. Each contact counts separately." : "Choose an Action After a Hit or Block first.");
	choice_row(practice_rows::times, "After", reacts, reacts ? "How many counted hits or blocks bring the reaction, once the dummy can act. Each hit of a string counts; the count starts again after each reaction and on a Restart." : "Choose an Action After a Hit or Block first.");
	choice_row(practice_rows::input_delay, "Input Delay Frames", true, input_delay_help);
	const bool settled = practice_dummy_settled();
	restore_row(settled && data.practice.menu.game_defaults.has_value(), !settled                         ? busy :
																		 data.practice.menu.game_defaults ? std::string("Return the game's practice options to the values Practice started with, Movement Technique to Neutral, Action After a Hit or Block to None, Count to Hits or Blocks, After to 1 Time and Input Delay Frames to Not Set.") :
																											std::string("The game's practice options can't be read right now."));
}

void pbd::rollback_session::practice_steps::native_menu::offense_header()
{
	// When the reaction comes, in the header's words: "after each block", "after every 3 hits".
	const auto counted_words = [&](bool plural)
	{
		using pbd::practice::contact_counted;
		return data.practice.offense.counted == contact_counted::blocks ? (plural ? "blocks" : "block") :
		       data.practice.offense.counted == contact_counted::hits   ? (plural ? "hits" : "hit") :
		                                                                  (plural ? "hits or blocks" : "hit or block");
	};
	const std::string when = data.practice.offense.times <= 1 ? fmt::format("after each %s", counted_words(false)) :
	                                                            fmt::format("after every %u %s", data.practice.offense.times, counted_words(true));
	// What the dummy does now, under the list header.
	if (data.practice.dummy.enabled && reacts)
		data.practice.view.header = data.practice.offense.movement_off ?
		                                fmt::format("Reaction off \xC2\xB7 {select}+{circle} turns %s back on in the fight", reaction_name) :
		                                fmt::format("%s %s \xC2\xB7 {select}+{circle} turns it off", reaction_name, when);
	else if (data.practice.dummy.enabled && !data.practice.movement.input_sequence.empty())
	{
		const char* name = pbd::practice::movement::preset_name(data.practice.offense.movement);
		data.practice.view.header = data.practice.offense.movement_off ?
		                                fmt::format("Movement off \xC2\xB7 {select}+{circle} turns %s back on in the fight", name) :
		                                fmt::format("%s \xC2\xB7 {select}+{circle} turns it off in the fight", name);
	}
}

void pbd::rollback_session::practice_steps::native_menu::defense_rows()
{
	// Recording slots: each row opens Record / Select from Move List / Not Set, a filled slot shows its
	// length and carries a frequency weight that L1/R1 change on the focused row. Playback, started and
	// stopped with SELECT+Circle in the fight, picks among filled slots by that weight. The menu cannot
	// open while a take is being recorded or waits to start.
	namespace dummy_rows = pbd::practice::dummy_rows;
	u32 recorded = 0;
	for (u32 i = 0; i < dummy_rows::slot_count; ++i)
	{
		const auto& slot = data.practice.dummy.slots[i];
		// A slot shows its recording's length, or its moves from the move list: the move's name, or how many
		// it picks from at random.
		const bool moves = !slot.moves.empty();
		const bool filled = !slot.frames.empty() || moves;
		recorded += filled;
		const double seconds = practice_slot_frames(slot).size() / 60.0;
		row(practice_slot_name(i), moves ? practice_moves_label(slot.moves) : filled ? fmt::format("%.1f s", seconds) :
																					   "Not Set",
			data.practice.native.pause_valid,
			filled ? std::string("{l1}/{r1} sets how often the dummy picks this action; at 0 it is never picked.") :
					 std::string("Choose Recording to record an action for the dummy, or Choose from Move List."),
			!filled);
		if (filled)
			rows.back().frequency = std::clamp<u32>(slot.frequency, pbd::practice::min_frequency, pbd::practice::max_frequency);
	}
	// Fifteen rows fill the panel, so the playback settings sit straight under the slots.
	choice_row(practice_rows::first + dummy_rows::interval, "Action Interval", true, "Time between the dummy's actions.");
	// The movement presets follow the dummy's side: toward and away are always relative to the player.
	choice_row(practice_rows::first + dummy_rows::movement, "Movement Between Actions", true,
		"What the dummy does whenever it is not playing or recovering from an action; with no action recorded, {select}+{circle} loops it alone (not Backdash or a sidestep).");
	// Wavedash Speed only shapes a wavedash; the end row is the wavedash's or a dash loop's end. Both are
	// dimmed for any other movement.
	const auto end_kind = pbd::practice::end_row_for(data.practice.movement.preset);
	const bool wavedash = end_kind == pbd::practice::end_row::wavedash;
	choice_row(practice_rows::first + dummy_rows::wavedash_speed, "Wavedash Speed", wavedash, wavedash ? "How many crouch dashes a second the dummy's wavedash makes." : "Choose Wavedash for Movement Between Actions first.");
	if (end_kind == pbd::practice::end_row::dash)
		choice_row(practice_rows::first + dummy_rows::movement_end, "Dash End", true,
			"How the dash goes into each action: straight into it, or stopping first so the action's forward or back is not read as a dash.");
	else
		choice_row(practice_rows::first + dummy_rows::movement_end, "Wavedash End", wavedash, wavedash ? "How the wavedash ends as it goes into each action: straight from the crouch dash, or with a forward or back tap." : "Choose Wavedash, Continuous Backdash or Continuous Forward Dash for Movement Between Actions first.");
	choice_row(practice_rows::first + dummy_rows::trim, "Trim Neutral", true, "Play each recording from your first input to your last.");
	choice_row(practice_rows::first + dummy_rows::save_recordings, "Save Recordings", true, "Name and save the eight actions and playback settings for this dummy character.");
	choice_row(practice_rows::first + dummy_rows::load_recordings, "Load Recordings", true, "Load a named set of actions and playback settings for this dummy character.");
	choice_row(practice_rows::first + dummy_rows::input_delay, "Input Delay Frames", true, input_delay_help);
	restore_row(true, "Return the interval, movement, wavedash speed, wavedash and dash ends, trim, frequencies and input delay to their defaults; recordings are kept.");
	const bool playing = data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview;
	data.practice.view.header = recorded && practice_dummy_all_zero() ?
	                                fmt::format("%u of 8 slots recorded \xC2\xB7 every one at Frequency 0, so none plays", recorded) :
	                            playing                                        ? fmt::format("%u of 8 slots recorded \xC2\xB7 Playing", recorded) :
	                            recorded                                       ? fmt::format("%u of 8 slots recorded \xC2\xB7 {select}+{circle} plays them in the fight", recorded) :
	                            practice_movement_alone()                      ? std::string("0 of 8 slots recorded \xC2\xB7 {select}+{circle} plays the movement in the fight") :
	                            !data.practice.movement.input_sequence.empty() ? fmt::format("0 of 8 slots recorded \xC2\xB7 %s plays only before a recorded action",
																					 pbd::practice::movement::preset_name(data.practice.movement.preset)) :
	                                                                             std::string("0 of 8 slots recorded");
}

void pbd::rollback_session::practice_steps::native_menu::restart_rows()
{
	constexpr std::array<const char*, 8> positions{"Round Start", "Remembered", "Centre", "Left Wall",
		"Right Wall", "Foreground Wall", "Background Wall", "Random"};
	const bool position_ready = situation_ready && pbd::game_lifecycle::placement_available(data.practice.restart.position, lifecycle_scope(), data.practice.restart.placement);
	const char* position = positions[static_cast<u32>(data.practice.restart.position)];
	row("Restart", situation_ready ? position : "Unavailable", situation_ready,
		!situation_ready ? "Restart isn't available right now." :
		position_ready   ? "Restart the fight now at the Restart Position." :
						   "Remember a position first, or choose another Restart Position.");
	choice_row(pbd::practice::restart_rows::position, "Restart Position", true, "Where Restart puts both fighters.");
	row("Remember this moment", situation_ready ? (remembered_ready ? "Stored" : "Not Set") : "Unavailable", situation_ready,
		!situation_ready ? "Remembering isn't available right now." :
		r3_remembers     ? "Keep the current fight state in memory ({r3} in game). Use {select}+{r3} to return to it." :
						   "Keep the current fight state in memory. Use {select}+{r3} to return to it.",
		!remembered_ready);
	choice_row(pbd::practice::restart_rows::dummy_direction, "Dummy Direction", true,
		"Which way the dummy faces after every Restart.");
	choice_row(pbd::practice::restart_rows::switch_positions, "Switch Positions", true,
		"Whether you and the dummy trade places at every Restart.");
	restore_row(true,
		"Set Restart Position to Centre, Dummy Direction to Front and Switch Positions off; a remembered position is kept.");
	// In-game chords, under the list: the restarts, Remember, and the dummy chord (Defense's playback toggle, or
	// in Offense with a Movement chosen, the movement's).
	auto& legend = data.practice.view.list_legend;
	legend.add("{select}+{cross}", "Restart");
	legend.add("{select}+{r3}", "Back to it");
	legend.add("{select}+{dpad:u}/{dpad:d}", "Background/Foreground Wall");
	legend.add("{select}+{dpad:l}/{dpad:r}", "Left/Right Wall");
	if (r3_remembers)
		legend.add("{r3}", "Remember");
	const bool offense = data.practice.training_mode == pbd::practice::training_mode::offense;
	legend.add("{select}+{circle}", offense && data.practice.offense.after != pbd::practice::after_contact::none ? "Reaction On/Off" :
									offense && !data.practice.movement.input_sequence.empty()                    ? "Movement On/Off" :
																												   "Play/Stop Dummy (Defense)");
	panel.selected = data.practice.menu.row;
}

void pbd::rollback_session::practice_steps::native_menu::display_rows()
{
	namespace display_rows = pbd::practice::display_rows;
	choice_row(display_rows::p1_history, "P1 Command History", true, "Show P1's inputs at the left edge of the screen.");
	choice_row(display_rows::p2_history, "P2 Command History", true, "Show P2's inputs at the right edge of the screen.");
	choice_row(display_rows::attack_info, "Attack Info", true, "Show each side's last hit, its latest combo's hits and damage, and the session's damage during the fight.");
	choice_row(display_rows::frame_data, "Frame Data", true, "Show each side's Startup and Frame Advantage, with the outcome for a launch, knockdown, wall hit or throw, and whether your attack punished one you blocked.");
	choice_row(display_rows::control_hints, "Control Hints", true, "Show the control bar at the bottom of the screen during the fight.");
	choice_row(display_rows::movement_streaks, "Movement Streaks", true,
		"Grade each backdash cancel; after 3 or more wavedashes or cancels in a row, show how many and how fast.");
	restore_row(true, "Display both command histories, Attack Info, Frame Data and the control bar again.");
	panel.selected = data.practice.menu.row;
}

void pbd::rollback_session::practice_steps::native_menu::selection_rows()
{
	if (page == category::character_selection)
	{
		const auto gate = practice_selection_gate();
		row("Go to Character Selection", "", gate != pbd::practice::selection_gate::refuse,
			gate == pbd::practice::selection_gate::refuse ? busy :
															pbd::practice::selection_help(true, pbd::practice::selection_names_stop(data.practice.training_mode, gate)));
		panel.selected = 0;
	}
	else if (page == category::stage_selection)
	{
		const auto gate = practice_selection_gate();
		for (u32 at = 0; at < pbd::practice::stage::row_count; ++at)
			row(std::string(pbd::practice::stage::row_name(at)), "", gate != pbd::practice::selection_gate::refuse,
				gate == pbd::practice::selection_gate::refuse ? busy : "Load this stage with the same fighters.");
		panel.selected = data.practice.menu.row;
	}
	else if (page == category::move_list)
	{
		const auto gate = practice_selection_gate();
		row("Open Move List", "", data.practice.native.pause_valid && gate != pbd::practice::selection_gate::refuse,
			gate == pbd::practice::selection_gate::refuse ? busy : "Open your character's command list; any dummy playback stops. Close the list to return here.");
		panel.selected = 0;
	}
	else if (page == category::settings)
	{
		// One action row: the in-game Settings menu (also Esc) opens over this menu and returns to it when closed.
		row("Open Settings", "", true, "Change the volume, resolution, window, VSync, audio output and display (Esc in game).");
		panel.selected = 0;
	}
	else
	{
		row("Exit Practice", "", true, "Leave Practice and return to the launcher (hold {start}+{select} in game).");
		panel.selected = 0;
	}
}

void pbd::rollback_session::practice_steps::native_menu::training_rows()
{
	// The Training Mode row first; the rows after it belong to the mode in view.
	namespace practice_rows = pbd::practice::practice_rows;
	using pbd::practice::training_mode;
	choice_row(practice_rows::mode, "Training Mode", data.practice.native.pause_valid,
		"Offense: the game's own dummy options and a Movement Technique. Defense: actions you record for the dummy, played with {select}+{circle}.");
	if (data.practice.training_mode == training_mode::offense)
	{
		offense_options();
		offense_reaction();
		offense_header();
	}
	else if (data.practice.training_mode == training_mode::defense)
		defense_rows();
	panel.selected = data.practice.menu.row;
}
