#pragma once

// Private scenarios included inside practice_menu_test.cpp's anonymous namespace.
void movement_and_reaction_options()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// Wavedash Speed: 2 to 8 crouch dashes a second, slowest first, 3 the default.
	check(wavedash_speed_option(2) == 0 && wavedash_speed_option(8) == 6 && wavedash_speed_option(1) == 0 && wavedash_speed_option(99) == 6 &&
			  wavedash_speed_from_option(0) == 2u && wavedash_speed_from_option(6) == 8u && !wavedash_speed_from_option(7) &&
			  pbd::practice::movement::wavedash_speed_count == 7 && pbd::practice::movement::default_wavedash_speed == 3 &&
			  pbd::practice::movement::wavedash_speed_count <= max_options,
		"Wavedash Speed lists 2 to 8 a second");
	for (std::uint32_t option = 0; option < pbd::practice::movement::wavedash_speed_count; ++option)
		check(wavedash_speed_option(*wavedash_speed_from_option(option)) == option, "every Wavedash Speed option reads back as itself");
	{
		// Action After a Hit or Block: None (the default), the seven plain inputs, Select from Move List, then Recording.
		using pbd::practice::movement::reaction_input;
		check(static_cast<std::uint32_t>(after_contact::none) == 0 && static_cast<std::uint32_t>(after_contact::movement) == 1 &&
				  static_cast<std::uint32_t>(after_contact::action) == 2 && static_cast<std::uint32_t>(after_contact::moves) == 3 &&
				  after_contact_kinds == 4 && reaction_slots == dummy_rows::slot_count && reaction_first_input == 1 &&
				  reaction_moves_option == 8 && reaction_recording_option == 9 && after_contact_options == 10 && after_contact_options <= max_options,
			"Action After a Hit or Block lists None, seven inputs, Select from Move List and Recording");
		constexpr std::size_t input_count = 7;
		check(pbd::practice::movement::reaction_input_count == input_count, "seven plain inputs");
		for (std::uint32_t i = 0; i < input_count; ++i)
		{
			const auto reaction = reaction_from_option(reaction_first_input + i);
			check(reaction && reaction->kind == after_contact::movement && static_cast<std::uint32_t>(reaction->input) == i,
				"each input option is that input");
			check(reaction_option(*reaction) == reaction_first_input + i, "and reads back as itself");
		}
		check(reaction_from_option(0) == contact_reaction{} && reaction_option(contact_reaction{}) == 0, "None is the first option");
		check(reaction_from_option(reaction_moves_option)->kind == after_contact::moves && reaction_option({after_contact::moves}) == reaction_moves_option,
			"Select from Move List");
		check(reaction_from_option(reaction_recording_option)->kind == after_contact::action &&
				  reaction_option({after_contact::action, 6}) == reaction_recording_option,
			"every recorded Dummy Action is under Recording");
		check(!reaction_from_option(after_contact_options) && !reaction_from_option(~0u), "nothing past Recording");
		check(reaction_option({after_contact::none, 5, reaction_input::crouch}) == 0, "the slot and input only matter for their kinds");
	}
	check(static_cast<std::uint32_t>(contact_counted::blocks) == 0 && static_cast<std::uint32_t>(contact_counted::hits) == 1 &&
			  static_cast<std::uint32_t>(contact_counted::either) == 2 && contact_counted_count == 3,
		"Count lists Blocks, Hits, Hits or Blocks");
	check(min_reaction_times == 1 && max_reaction_times == 5, "After lists 1 to 5 times");
	check(restart_rows::restore == restart_rows::switch_positions + 1 && restart_rows::count == restart_rows::restore + 1,
		"Restart Settings ends with Restore Defaults");
	check(dummy_rows::wavedash_speed == dummy_rows::movement + 1 && dummy_rows::movement_end == dummy_rows::wavedash_speed + 1 &&
			  dummy_rows::save_recordings == dummy_rows::trim + 1 && dummy_rows::load_recordings == dummy_rows::save_recordings + 1 &&
			  dummy_rows::input_delay == dummy_rows::load_recordings + 1 && dummy_rows::count == dummy_rows::input_delay + 2,
		"Defense ends with Save and Load Recordings, Input Delay Frames, Restore Defaults; Wavedash Speed and End under the movement");
}

void training_modes_and_navigation()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// The end row is Wavedash End for a wavedash, Dash End for the two dash loops, and dimmed for anything else.
	{
		using pbd::practice::movement::preset;
		check(end_row_for(preset::wavedash) == end_row::wavedash && end_row_for(preset::forward_dash) == end_row::dash &&
				  end_row_for(preset::backdash_cancel) == end_row::dash && end_row_for(preset::backdash) == end_row::none &&
				  end_row_for(preset::none) == end_row::none && end_row_for(preset::jump_back) == end_row::none,
			"Dash End belongs to Continuous Forward Dash and Continuous Backdash only");
	}

	// Training modes: Offense, Defense, CPU in this order; each mode's rows follow Training Mode.
	check(static_cast<std::uint32_t>(training_mode::offense) == 0 && static_cast<std::uint32_t>(training_mode::defense) == 1 &&
			  static_cast<std::uint32_t>(training_mode::cpu) == 2 && training_mode_count == 3,
		"Training Mode lists Offense, Defense, CPU");
	check(practice_row_count(training_mode::offense) == 15 && practice_row_count(training_mode::defense) == 18 &&
			  practice_row_count(training_mode::cpu) == 1,
		"Offense has 15 rows including Wake-up and Throw Escape, Defense 18, and the unavailable CPU only Training Mode");
	check(!game_option_row(training_mode::offense, practice_rows::counted) && !game_option_row(training_mode::offense, practice_rows::times),
		"Count and After are not game options");
	check(!game_option_row(training_mode::offense, practice_rows::mode) && game_option_row(training_mode::offense, 1) == 0u &&
			  game_option_row(training_mode::offense, 5) == 4u && !game_option_row(training_mode::offense, practice_rows::throw_escape) &&
			  !game_option_row(training_mode::offense, practice_rows::movement) &&
			  !game_option_row(training_mode::offense, practice_rows::after_contact) && !game_option_row(training_mode::offense, practice_rows::restore) &&
			  !game_option_row(training_mode::defense, 1),
		"Offense rows 1..5 are the game's options, and only in Offense");
	check(!defense_row(training_mode::defense, practice_rows::mode) && defense_row(training_mode::defense, 1) == 0u &&
			  defense_row(training_mode::defense, 8) == 7u && defense_row(training_mode::defense, 9) == dummy_rows::interval &&
			  defense_row(training_mode::defense, 11) == dummy_rows::wavedash_speed && defense_row(training_mode::defense, 12) == dummy_rows::movement_end &&
			  defense_row(training_mode::defense, 14) == dummy_rows::save_recordings && defense_row(training_mode::defense, 15) == dummy_rows::load_recordings &&
			  defense_row(training_mode::defense, 16) == dummy_rows::input_delay &&
			  defense_row(training_mode::defense, 17) == dummy_rows::restore && !defense_row(training_mode::defense, 18) &&
			  !defense_row(training_mode::offense, 1) && !defense_row(training_mode::defense, ~0u),
		"Defense rows follow Training Mode, and only in Defense");
	check(display_rows::p1_history == 0 && display_rows::p2_history == 1 && display_rows::attack_info == 2 &&
			  display_rows::frame_data == 3 && display_rows::control_hints == 4 && display_rows::movement_streaks == 5 && display_rows::restore == 6 &&
			  display_rows::count == 7,
		"Display Settings lists P1 and P2 Command History, Attack Info, Frame Data, Control Hints, Movement Streaks, Restore Defaults");
	for (const auto count : {practice_row_count(training_mode::offense), practice_row_count(training_mode::defense), restart_rows::count,
			 display_rows::count})
	{
		for (std::uint32_t selected = 0; selected < count; ++selected)
		{
			const auto visible = pbd_visible_rows(count, selected, pbd::game_overlay::max_rows);
			check(visible.count <= pbd::game_overlay::max_rows && visible.count <= pbd::practice::mouse::layout{}.rows.size(),
				"every visible settings page fits the panel and pointer rows");
			check(visible.first + visible.selected == selected && visible.selected < visible.count,
				"scrolling keeps the selected page row and its pointer index");
		}
	}
	const auto last_defense = pbd_visible_rows(practice_row_count(training_mode::defense), 17, 15);
	check(last_defense.first == 3 && last_defense.count == 15 && last_defense.selected == 14,
		"the last Defense row scrolls three rows and remains the last visible pointer row");
	check(pbd_visible_rows(0, 0, 15).count == 0 && pbd_visible_rows(17, 16, 0).count == 0,
		"an empty page or zero capacity has no rows");

	// wrap_step_over passes over rows that cannot take focus, in both directions and when wrapping.
	const std::uint32_t skip = 1u << display_rows::frame_data;
	check(wrap_step_over(display_rows::attack_info, display_rows::count, true, skip) == display_rows::control_hints,
		"Down passes over a skipped row");
	check(wrap_step_over(display_rows::control_hints, display_rows::count, false, skip) == display_rows::attack_info,
		"Up passes over a skipped row");
	check(wrap_step_over(display_rows::restore, display_rows::count, true, skip) == display_rows::p1_history &&
			  wrap_step_over(display_rows::p1_history, display_rows::count, false, skip) == display_rows::restore,
		"the list still wraps at both ends");
	check(wrap_step_over(display_rows::frame_data, display_rows::count, true, skip) == display_rows::control_hints,
		"a highlight left on a skipped row (by the pointer) still moves on");
	check(wrap_step_over(2, 5, true, 0) == wrap_step(2, 5, true) && wrap_step_over(0, 5, false, 0) == wrap_step(0, 5, false),
		"without skipped rows the step is the ordinary wrap");
	check(wrap_step_over(1, 3, true, 0x7) == 1 && wrap_step_over(0, 0, true, 0) == 0, "a list with nothing reachable keeps its highlight");
}

void display_defaults_and_confirmation()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// Display defaults: both histories, Attack Info, Frame Data and the control hints are shown.
	const snapshot fresh{};
	check(fresh.command_history[0] && fresh.command_history[1] && fresh.attack_info && fresh.frame_data && fresh.control_hints &&
			  fresh.movement_streaks,
		"Display Settings must default to Display for every row");
	check(!fresh.confirm.open && fresh.confirm.focus == answer_no && fresh.list_legend.count == 0,
		"a fresh view has no question and no legend");

	check(answer_yes == 0 && answer_no == 1 && answer_count == 2, "confirmation action indices");
}

void selection_gates_and_loading()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// Character and Stage Selection: one press from an idle dummy or during playback (Defense's actions or Offense's
	// Movement, which stop first); a take being recorded, waiting to record or previewed refuses.
	check(selection_gate_for(dummy_phase::idle) == selection_gate::go, "an idle dummy goes to the selection at once");
	check(selection_gate_for(dummy_phase::playing) == selection_gate::stop_then_go, "playback stops, then the selection runs");
	check(selection_gate_for(dummy_phase::recording) == selection_gate::refuse &&
			  selection_gate_for(dummy_phase::ready) == selection_gate::refuse &&
			  selection_gate_for(dummy_phase::preview) == selection_gate::refuse,
		"a take in progress refuses the selection");
	// Only Defense's playback is named: Offense's Movement and reaction stop silently and start again on their
	// own in the new fight. Nothing is named when nothing stops.
	for (const auto phase : {dummy_phase::idle, dummy_phase::recording, dummy_phase::playing, dummy_phase::preview, dummy_phase::ready})
	{
		const auto gate = selection_gate_for(phase);
		check(selection_names_stop(training_mode::offense, gate) == false, "Offense's Movement stops for a selection without a word");
		check(selection_names_stop(training_mode::defense, gate) == (phase == dummy_phase::playing),
			"Defense names the stop exactly when its playback is the one stopping");
	}
}

void packed_game_options_and_prompts()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// The game's five options travel packed in one word; every value the menu can hold survives the round trip.
	std::uint32_t combinations = 0;
	for (std::uint32_t action = 0; action <= 10; ++action)
		for (std::uint32_t counter = 0; counter <= 1; ++counter)
			for (std::uint32_t capture = 0; capture <= 1; ++capture)
				for (std::uint32_t attack = 0; attack <= 2; ++attack)
					for (std::uint32_t analysis = 0; analysis <= 1; ++analysis)
					{
						const std::array<std::uint32_t, game_option_count> values{action, counter, capture, attack, analysis};
						const auto packed = pack_game_options(values);
						check(unpack_game_options(packed) == values && packed < (1u << 12), "packed game options must round-trip");
						++combinations;
					}
	check(combinations == 11 * 2 * 2 * 3 * 2, "every combination of the game's options was packed");
	check(pack_game_options({10, 1, 1, 2, 1}) == (10u | 1u << 4 | 1u << 6 | 2u << 8 | 1u << 10), "the packed layout must stay fixed");

	// The legend uses the same prompt groups as the control bar.
	prompt_bar legend;
	legend.add("{select}+{cross}", "Restart");
	legend.add("{select}+{r3}", "Remembered");
	legend.add("{select}+{dpad:u}/{dpad:d}", "Background/Foreground Wall");
	legend.add("{select}+{dpad:l}/{dpad:r}", "Left/Right Wall");
	legend.add("{r3}", "Remember");
	legend.add("{select}+{circle}", "Play/Stop Dummy (Defense)");
	check(legend.count == 6 && legend.count <= max_prompts && !legend.offers(prompt_action::confirm), "the in-game legend is six plain groups");
}

void dummy_action_shortcuts()
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// SELECT+Circle in the fight: ready starts the take and a recording ends it; Defense toggles playback; Offense with a
	// Movement turns the movement off and on.
	check(select_circle(training_mode::defense, dummy_phase::ready) == circle_chord::start_recording &&
			  select_circle(training_mode::offense, dummy_phase::ready) == circle_chord::start_recording,
		"ready to record: SELECT+Circle starts the take");
	check(select_circle(training_mode::defense, dummy_phase::idle) == circle_chord::start_playback &&
			  select_circle(training_mode::defense, dummy_phase::playing) == circle_chord::stop_playback,
		"in Defense SELECT+Circle starts playback and, pressed again, stops it");
	check(select_circle(training_mode::offense, dummy_phase::idle) == circle_chord::needs_defense &&
			  select_circle(training_mode::cpu, dummy_phase::idle) == circle_chord::needs_defense,
		"outside Defense SELECT+Circle never starts playback or a recording");
	check(select_circle(training_mode::offense, dummy_phase::playing) == circle_chord::stop_playback, "playback can always be stopped");
	check(select_circle(training_mode::defense, dummy_phase::recording) == circle_chord::stop_recording,
		"SELECT+Circle stops a take in progress, as START does");
	check(select_circle(training_mode::defense, dummy_phase::preview) == circle_chord::none, "a preview ignores SELECT+Circle");
	check(select_circle(training_mode::offense, dummy_phase::idle, true) == circle_chord::toggle_movement &&
			  select_circle(training_mode::offense, dummy_phase::playing, true) == circle_chord::toggle_movement,
		"in Offense with a Movement SELECT+Circle turns the movement off or on, moving or not");
	check(select_circle(training_mode::defense, dummy_phase::idle, true) == circle_chord::start_playback &&
			  select_circle(training_mode::defense, dummy_phase::playing, true) == circle_chord::stop_playback &&
			  select_circle(training_mode::cpu, dummy_phase::idle, true) == circle_chord::needs_defense,
		"Defense keeps its playback toggle whatever movement is chosen");
	check(select_circle(training_mode::offense, dummy_phase::recording, true) == circle_chord::stop_recording &&
			  select_circle(training_mode::offense, dummy_phase::ready, true) == circle_chord::start_recording,
		"a take is started and stopped the same way in either mode");
}
