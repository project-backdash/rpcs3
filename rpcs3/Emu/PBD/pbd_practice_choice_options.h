#pragma once

struct pbd::rollback_session::practice_choice_steps : practice_steps
{
	u32 row;
	practice_choice_list& out;
	std::optional<u32> defense;

	void run()
	{
		const auto* memory = session_runtime.m_practice_memory.get();
		if (data.practice.menu.dropdown_open && data.practice.menu.page == category::practice_settings &&
			data.practice.training_mode == pbd::practice::training_mode::defense &&
			row == pbd::practice::practice_rows::first + pbd::practice::dummy_rows::save_recordings &&
			row == data.practice.menu.row && memory && memory->set_editor && memory->dummy == memory->set_editor->character)
		{
			out = {memory->set_editor->choices(), memory->set_editor->focus};
			return;
		}

		if (fill_mode())
			return;

		// The Wavedash Speed rows: 2 to 8 crouch dashes a second, slowest first.

		defense = practice_defense_row(row);
		if (fill_training_choices())
			return;
		if (fill_reaction_choices())
			return;
		if (fill_contact_count_choices())
			return;
		if (fill_game_choices())
			return;
		if (fill_restart_choices())
			return;
		fill_defense_choices();
	}

	void add(std::string text, std::string help = {}, bool is_default = false, std::string note = {})
	{
		pbd::practice::menu_option option;
		option.text = std::move(text);
		option.help = std::move(help);
		option.is_default = is_default;
		option.note = is_default && note.empty() ? std::string("Default") : std::move(note);
		out.options.push_back(std::move(option));
	}

	void add_movements(pbd::practice::movement::preset current, const char* neutral_help)
	{
		u32 character = umax;
		const bool known = read_word(0x10093f54, character) && character <= 0xff;
		out = pbd::practice::menu::movement_choices(current, known ? std::optional<u32>{character} : std::nullopt, neutral_help);
	}

	void add_speeds(u32 current)
	{
		for (u32 speed = pbd::practice::movement::min_wavedash_speed; speed <= pbd::practice::movement::max_wavedash_speed; ++speed)
			add(pbd::practice::wavedash_speed_text(speed), pbd::practice::wavedash_speed_help(speed),
				speed == pbd::practice::movement::default_wavedash_speed);
		out.current = pbd::practice::wavedash_speed_option(current);
	}

	void add_dash_end_choices()
	{
		// Menu order is the dash_end order.
		using pbd::practice::movement::dash_end;
		add(pbd::practice::movement::dash_end_name(dash_end::into_move),
			"The dash runs straight into the action, whose own forward or back is the next tap: record ff+2 for ff+2.", true);
		add(pbd::practice::movement::dash_end_name(dash_end::stop_first),
			"The dash stops before the action, so its first forward or back is a fresh press: record f+4 for f+4.");
		out.current = std::min<u32>(static_cast<u32>(data.practice.movement.dash_end), pbd::practice::movement::dash_end_count - 1);
	}

	void add_wavedash_end_choices()
	{
		// Menu order is the wavedash_end order.
		using pbd::practice::movement::wavedash_end;
		add(pbd::practice::movement::wavedash_end_name(wavedash_end::crouch_dash),
			"The action comes straight out of the last crouch dash: record f+4 for a crouch dash f+4.", true);
		add(pbd::practice::movement::wavedash_end_name(wavedash_end::forward),
			"The last crouch dash ends with forward, then the action: record f+3 for a crouch dash f,f+3.");
		add(pbd::practice::movement::wavedash_end_name(wavedash_end::back),
			"The last crouch dash ends with back, then the action: record f+4 for a crouch dash b,f+4.");
		out.current = std::min<u32>(static_cast<u32>(data.practice.movement.wavedash_end), pbd::practice::movement::wavedash_end_count - 1);
	}

	bool fill_mode()
	{
		if (data.practice.menu.page == category::practice_settings && row == pbd::practice::practice_rows::mode)
		{
			// The training modes. The game's practice options have no CPU-controlled dummy (Dummy Action stops
			// at Controller), so CPU is listed but cannot be chosen yet.
			add("Offense", "The dummy follows the game's own options, such as Dummy Action, or a Movement Technique you choose.", true);
			add("Defense", "The dummy plays your recordings: set them up here, then {select}+{circle} starts and stops playback in the fight.");
			add("CPU", "The game's own AI controls the dummy. Coming in a later update.", false, "Coming soon");
			out.options.back().enabled = false;
			out.current = std::min<u32>(static_cast<u32>(data.practice.training_mode), pbd::practice::training_mode_count - 1);
			return true;
		}
		return false;
	}

	bool fill_training_choices()
	{
		if (data.practice.menu.page == category::practice_settings && pbd::practice::input_delay_row(data.practice.training_mode, row))
		{
			out = pbd::practice::menu::input_delay_choices(data.practice.input_delay_frames);
			return true;
		}
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::wakeup)
		{
			for (u32 option = 0; option < pbd::practice::pbd_wakeup_option_count; ++option)
				add(std::string(pbd::practice::pbd_wakeup_names[option]), "How the dummy gets up after a knockdown; unavailable choices use Stand Up.", option == 0);
			out.current = std::min<u32>(static_cast<u32>(data.practice.reactions.wakeup), pbd::practice::pbd_wakeup_option_count - 1);
			return true;
		}
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::throw_escape)
		{
			using pbd::practice::throw_escape_names;
			add(std::string(throw_escape_names[0]), "The dummy never breaks your throws.", true);
			add(std::string(throw_escape_names[1]), "The dummy breaks every throw that can be broken.");
			add(std::string(throw_escape_names[2]), "The dummy breaks about half of your throws, chosen at random.");
			out.current = std::min<u32>(static_cast<u32>(data.practice.reactions.throw_escape), pbd::practice::throw_escape_option_count - 1);
			return true;
		}
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::movement)
		{
			u32 character = umax;
			const bool known = read_word(0x10093f54, character) && character <= 0xff;
			out = pbd::practice::menu::technique_choices(data.practice.offense.movement, known ? std::optional<u32>{character} : std::nullopt);
			return true;
		}
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::wavedash_speed)
		{
			add_speeds(data.practice.offense.wavedash_speed);
			return true;
		}

		return false;
	}

	bool fill_reaction_choices()
	{
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::after_contact)
		{
			bool recorded = false;
			for (u32 i = 0; i < pbd::practice::reaction_slots; ++i)
				recorded = recorded || practice_slot_playable(data.practice.dummy.slots[i]);
			if (data.practice.menu.dropdown_open && data.practice.menu.dropdown_sub && row == data.practice.menu.row)
			{
				// Recording's own list: the Defense Dummy Action slots. An empty slot is listed, dimmed, with a pointer to where
				// it is recorded.
				u32 first = umax;
				for (u32 i = 0; i < pbd::practice::reaction_slots; ++i)
				{
					const auto& slot = data.practice.dummy.slots[i];
					const bool playable = practice_slot_playable(slot);
					const bool moves = !slot.moves.empty();
					const auto help = !playable ? std::string("Set up this Dummy Action first: Training Mode Defense, then this Dummy Action's row.") :
					                  moves     ? std::string("After the counted hit or block the dummy plays one of this slot's chosen moves once, at random when several are chosen.") :
					                              fmt::format("After the counted hit or block the dummy plays your recorded %s once, then goes back to Dummy Action.", practice_slot_name(i));
					const auto note = !playable ? std::string("Not Set") : moves ? (slot.moves.size() == 1 ? std::string("1 move") : fmt::format("%u moves", static_cast<u32>(slot.moves.size()))) :
					                                                               fmt::format("%.1f s", practice_slot_frames(slot).size() / 60.0);
					add(practice_slot_name(i), help, false, note);
					out.options.back().enabled = playable;
					if (playable && first == umax)
						first = i;
				}
				out.current = data.practice.offense.after == pbd::practice::after_contact::action ? data.practice.offense.reaction_slot :
				              first != umax                                                       ? first :
				                                                                                    0;
				return true;
			}
			// Menu order (pbd::practice::reaction_from_option): None, the plain movement inputs, Select from Move List, then
			// Recording, which opens the recorded Dummy Actions.
			using pbd::practice::movement::reaction_input;
			add("None", "After a hit or block the dummy carries on as before.", true);
			constexpr std::array<const char*, pbd::practice::movement::reaction_input_count> input_help{
				"After the counted hit or block the dummy backdashes once.",
				"After the counted hit or block the dummy sidesteps to its own left once.",
				"After the counted hit or block the dummy sidesteps to its own right once.",
				"After the counted hit or block the dummy sidewalks to its own left for a second.",
				"After the counted hit or block the dummy sidewalks to its own right for a second.",
				"After the counted hit or block the dummy holds down for one second.",
				"After the counted hit or block the dummy taps down for 8 frames and stands straight back up, a quick duck as a player does it."};
			for (u32 i = 0; i < pbd::practice::movement::reaction_input_count; ++i)
			{
				const auto input = static_cast<reaction_input>(i);
				add(pbd::practice::movement::reaction_input_name(input), input_help[i], false,
					input == reaction_input::crouch ? "1 second" : input == reaction_input::fuzzy_duck ? "8 frames" :
																										 "");
			}
			const auto& moves = pbd::practice::dummy_move::current_settings.after_hit.kind == pbd::practice::dummy_move::source::moves ?
			                        pbd::practice::dummy_move::current_settings.after_hit :
			                        pbd::practice::dummy_move::current_settings.after_guard;
			const std::size_t chosen = moves.kind == pbd::practice::dummy_move::source::moves ? moves.ids.size() : 0;
			const bool picker = pbd::practice::dummy_move::catalog_available();
			add("Select from Move List", picker ? std::string("After the counted hit or block the dummy does a move you choose from its move list, at random when you choose several.") : std::string("The move list is available once both fighters have loaded."),
				false, chosen == 1 ? std::string("1 move") : chosen ? fmt::format("%u moves", static_cast<u32>(chosen)) :
																	  std::string());
			out.options.back().enabled = picker;
			add("Recording", recorded ? std::string("After the counted hit or block the dummy plays one of your saved Dummy Actions once.") : std::string("Set up a Dummy Action first: Training Mode Defense, then a Dummy Action's row."),
				false, data.practice.offense.after == pbd::practice::after_contact::action ? practice_slot_name(data.practice.offense.reaction_slot) : std::string());
			out.options.back().enabled = recorded;
			out.current = pbd::practice::reaction_option({data.practice.offense.after, data.practice.offense.reaction_slot, data.practice.offense.reaction_input});
			return true;
		}
		return false;
	}

	bool fill_contact_count_choices()
	{
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::counted)
		{
			// Menu order is the contact_counted order.
			add(std::string(pbd::practice::counted_name(pbd::practice::contact_counted::blocks)), "Count only the attacks the dummy blocks.");
			add(std::string(pbd::practice::counted_name(pbd::practice::contact_counted::hits)), "Count only the attacks that hit the dummy.");
			add(std::string(pbd::practice::counted_name(pbd::practice::contact_counted::either)), "Count every attack the dummy blocks or is hit by.", true);
			out.current = std::min<u32>(static_cast<u32>(data.practice.offense.counted), pbd::practice::contact_counted_count - 1);
			return true;
		}
		if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
			row == pbd::practice::practice_rows::times)
		{
			for (u32 times = pbd::practice::min_reaction_times; times <= pbd::practice::max_reaction_times; ++times)
				add(times == 1 ? std::string("1 Time") : fmt::format("%u Times", times), times == 1 ? std::string("The dummy reacts once it recovers from a counted hit or block.") : fmt::format("The dummy reacts after %u counted hits or blocks, once it can act. Each hit of a string counts; the count starts again after the reaction.", times),
					times == pbd::practice::min_reaction_times);
			out.current = std::clamp(data.practice.offense.times, pbd::practice::min_reaction_times, pbd::practice::max_reaction_times) -
			              pbd::practice::min_reaction_times;
			return true;
		}

		return false;
	}

	bool fill_game_choices()
	{
		if (data.practice.menu.page == category::practice_settings && !defense)
		{
			// Offense: the game's own options. Their stock defaults are not established here, so no option claims to be one.
			const auto game = pbd::practice::game_option_row(data.practice.training_mode, row);
			if (!game)
				return true;
			row = *game;
			const auto& offsets = practice_option_offsets;
			if (row >= offsets.size())
				return true;
			const auto native = pbd::practice::native::inspect(true);
			u32 value = umax;
			if (!native.practice || !read_word(native.practice + offsets[row], value))
				return true;
			// While recording or playback routes the dummy (practice_dummy_route), show the action that will be restored.
			if (row == 0 && data.practice.native.controlled && value == data.practice.native.written[0])
				value = data.practice.native.action[0];
			if (row == 0)
			{
				// Guard All's guard choices are Guard All in the game, so the action and the choice together name the option.
				if (value > 10)
					return true;
				for (const auto& choice : pbd::practice::pbd_dummy_action_choices)
					add(std::string(choice.name), std::string(choice.help));
				out.current = pbd::practice::pbd_dummy_action_option(value, data.practice.reactions.guard.option).value_or(0);
			}
			else if (row == 3)
			{
				// Attack Data stores 1 or 2; an unset 0 opens on Type 1, which is what choosing it writes.
				if (value > 2)
					return true;
				add("Type 1");
				add("Type 2");
				out.current = value == 2 ? 1 : 0;
			}
			else
			{
				if (value > 1)
					return true;
				add("Off");
				add("On");
				// Counter Hit's Random sets the game's option itself before each contact (practice_guard_frame).
				if (row == 1)
					add("Random", "Each of your hits is a counter hit or not, chosen at random.");
				out.current = row == 1 && data.practice.reactions.counter_hit.random ? 2 : value;
			}
			return true;
		}
		return false;
	}

	bool fill_restart_choices()
	{
		if (data.practice.menu.page == category::restart_settings && row == pbd::practice::restart_rows::dummy_direction)
		{
			out = pbd::practice::menu::direction_choices(data.practice.restart.variant.dummy_back);
			return true;
		}
		if (data.practice.menu.page == category::restart_settings && row == pbd::practice::restart_rows::switch_positions)
		{
			out = pbd::practice::menu::switch_choices(data.practice.restart.variant.switched);
			return true;
		}
		if (data.practice.menu.page == category::restart_settings && row == pbd::practice::restart_rows::position)
		{
			std::array<bool, 7> available{};
			const auto lifetime = lifecycle_scope();
			for (std::size_t i = 0; i < practice_restart_choices.size(); ++i)
				available[i] = pbd::game_lifecycle::placement_available(practice_restart_choices[i], lifetime, data.practice.restart.placement);
			const auto found = std::find(practice_restart_choices.begin(), practice_restart_choices.end(), data.practice.restart.position);
			{
				out = pbd::practice::menu::restart_choices(found == practice_restart_choices.end() ?
															   0u :
															   static_cast<u32>(found - practice_restart_choices.begin()),
					available);
				return true;
			}
		}

		return false;
	}

	void fill_defense_choices()
	{
		if (defense && *defense < pbd::practice::dummy_rows::slot_count)
		{
			// Each slot offers Recording (record a take for it), Choose from Move List (moves from the dummy's own list, one
			// picked at random each time when several are on) and Off.
			if (data.practice.dummy.state == dummy_state::recording)
				return;
			const auto& slot = data.practice.dummy.slots[*defense];
			const bool filled = !slot.frames.empty(), moves = !slot.moves.empty();
			const double seconds = pbd::practice::controls::playback_frames(pbd::practice::controls::take_body(slot.frames, slot.stop_chord_frames),
									   slot.leading_neutral_frames, slot.trailing_neutral_frames, slot.trim_neutral)
			                           .size() /
			                       60.0;
			add("Recording", filled ? fmt::format("Control the dummy and record with {select}+{circle}; the %.1f s action stays until you keep a new one.", seconds) : std::string("Control the dummy, then {select}+{circle} records up to 30 seconds; {start} or {select}+{circle} stops."),
				false, filled ? fmt::format("Replaces %.1f s", seconds) : std::string{});
			const bool list = pbd::practice::dummy_move::catalog_available();
			add("Choose from Move List", list ? std::string("Choose moves from the dummy's move list; with several on, the dummy picks one at random each time.") : std::string("The move list is available once both fighters have loaded."),
				false, moves ? (slot.moves.size() == 1 ? std::string("1 move") : fmt::format("%u moves", static_cast<u32>(slot.moves.size()))) : std::string());
			out.options.back().enabled = list;
			add("Off", filled || moves ? "Clear this slot." : "Leave this slot empty.", true);
			out.current = moves ? 1 : filled ? 0 :
			                                   2;
		}
		else if (defense && *defense == pbd::practice::dummy_rows::interval)
		{
			add("Short", "Half a second between actions.");
			add("Normal", "One second between actions.");
			add("Slow", "Two seconds between actions.");
			add("Shuffle", "A random gap of 0.5 to 2 seconds, so there is no rhythm to read.", true);
			out.current = std::min<u32>(data.practice.interval, 3);
		}
		else if (defense && (*defense == pbd::practice::dummy_rows::save_recordings || *defense == pbd::practice::dummy_rows::load_recordings))
		{
			out = recording_set_choices(*defense == pbd::practice::dummy_rows::load_recordings);
			return;
		}
		else if (defense && *defense == pbd::practice::dummy_rows::movement)
			add_movements(data.practice.movement.preset, "The dummy stands still whenever it is not playing an action.");
		else if (defense && *defense == pbd::practice::dummy_rows::wavedash_speed)
			add_speeds(data.practice.movement.wavedash_speed);
		else if (defense && *defense == pbd::practice::dummy_rows::movement_end &&
				 pbd::practice::end_row_for(data.practice.movement.preset) == pbd::practice::end_row::dash)
		{
			add_dash_end_choices();
		}
		else if (defense && *defense == pbd::practice::dummy_rows::movement_end)
		{
			add_wavedash_end_choices();
		}
		else if (defense && *defense == pbd::practice::dummy_rows::trim)
		{
			add("On", "Each recording plays from your first input to your last.", true);
			add("Off", "Each recording keeps the waits before your first input and after your last.");
			out.current = data.practice.dummy.slots[0].trim_neutral ? 0 : 1;
		}
		else if (data.practice.menu.page == category::display_settings)
		{
			out = pbd::practice::menu::display_choices(row, data.practice.view);
			return;
		}
		return;
	}
};
