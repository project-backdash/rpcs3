#pragma once

#include "pbd_practice_display_model.h"

namespace pbd::practice
{
	// What clicking a control-bar group does: the same as pressing its button. Groups the pointer cannot act on stay none.
	enum class prompt_action : std::uint8_t
	{
		none,
		confirm,
		back,
		resume,
		frequency
	};
	// A pointer target's value is the action, plus this flag on the second key of a pair ("{l1}/{r1}": R1 raises the frequency).
	inline constexpr std::uint32_t prompt_second_key = 0x100;

	// One control-bar group: keys as glyph tokens ("{select}+{cross}", "{l1}/{r1}", "{dpad:ud}") and the action's label.
	struct prompt
	{
		std::string keys;
		std::string label;
		prompt_action action = prompt_action::none;
	};

	struct prompt_bar
	{
		std::array<prompt, max_prompts> items{};
		std::uint32_t count = 0;
		void add(std::string keys, std::string label, prompt_action action = prompt_action::none)
		{
			if (count < items.size())
				items[count++] = {std::move(keys), std::move(label), action};
		}
		bool offers(prompt_action action) const noexcept
		{
			if (action == prompt_action::none)
				return false;
			for (std::uint32_t i = 0; i < count && i < items.size(); i++)
				if (items[i].action == action)
					return true;
			return false;
		}
	};

	// Ready: the player controls the dummy before SELECT+Circle starts the take.
	enum class dummy_phase : std::uint8_t
	{
		idle,
		recording,
		playing,
		preview,
		ready
	};
	// The dummy recorder's state in the input hook; the live-stats block publishes these numbers.
	enum class dummy_state : std::uint32_t
	{
		idle = 0,
		recording = 1,
		confirm = 2, // "Record this movement?": keep the take, record again or exit
		playing = 3, // a saved take or a preview plays on the dummy
		ready = 5    // the player controls the dummy; nothing is recorded until SELECT+Circle
	};
	enum class menu_layout : std::uint8_t
	{
		list,
		decision
	};

	// A value row's choices, opened with Confirm. The option in effect is aligned with its row; the highlighted one drives the
	// help line. Notes ("Default", "Replaces 4.2 s") sit at the right edge of the option. The longest list, Wake-up's fifteen
	// choices, sets the size: fifteen 30-pixel options fill the list area (dropdown_top), and an option past the last one
	// shown could be neither seen nor chosen.
	inline constexpr std::size_t max_options = 15;
	static_assert(after_contact_options <= max_options && pbd::practice::movement::technique_count <= max_options, "Offense's lists fit the option list");
	// A recorded Dummy Action's frequency weight: 0 keeps it recorded but never chosen.
	inline constexpr std::uint32_t min_frequency = 0, max_frequency = 10;

	// Whether actions are recorded but every one is at Frequency 0, so playback can never choose one.
	inline bool every_action_at_zero(const std::array<bool, 8>& playable, const std::array<std::uint32_t, 8>& frequencies) noexcept
	{
		bool any = false;
		for (std::size_t i = 0; i < playable.size(); ++i)
		{
			if (!playable[i])
				continue;
			if (frequencies[i])
				return false;
			any = true;
		}
		return any;
	}

	// Defense rows, counted from practice_rows::first: the eight action slots, then the playback settings and Input Delay
	// Frames. Playback itself has no row; SELECT+Circle starts and stops it in the fight. Wavedash Speed shapes a wavedash; the
	// end row is Wavedash End (pbd::practice::movement::wavedash_end) for a wavedash and Dash End
	// (pbd::practice::movement::dash_end) for a dash loop. Both sit under Movement Between Actions and are dimmed for any
	// other movement.
	namespace dummy_rows
	{
		inline constexpr std::uint32_t slot_count = 8, interval = 8, movement = 9, wavedash_speed = 10, movement_end = 11, trim = 12,
									   save_recordings = 13, load_recordings = 14, input_delay = 15, restore = 16, count = 17;
	}
	// Which end the end row sets for a movement.
	enum class end_row : std::uint8_t
	{
		none,
		wavedash,
		dash
	};
	inline constexpr end_row end_row_for(pbd::practice::movement::preset kind) noexcept
	{
		using pbd::practice::movement::preset;
		return kind == preset::wavedash ? end_row::wavedash : kind == preset::forward_dash || kind == preset::backdash_cancel ? end_row::dash :
		                                                                                                                        end_row::none;
	}

	// The Wavedash Speed row's options, slowest first: 2 to 8 crouch dashes a second.
	inline constexpr std::uint32_t wavedash_speed_option(std::uint32_t speed) noexcept
	{
		return pbd::practice::movement::clamp_wavedash_speed(speed) - pbd::practice::movement::min_wavedash_speed;
	}
	inline constexpr std::optional<std::uint32_t> wavedash_speed_from_option(std::uint32_t option) noexcept
	{
		if (option >= pbd::practice::movement::wavedash_speed_count)
			return std::nullopt;
		return pbd::practice::movement::min_wavedash_speed + option;
	}
	inline std::string wavedash_speed_text(std::uint32_t speed)
	{
		return std::to_string(pbd::practice::movement::clamp_wavedash_speed(speed)) + " per second";
	}
	// The option's help: how often a crouch dash starts, from the tape's own passes.
	inline std::string wavedash_speed_help(std::uint32_t speed)
	{
		const auto rhythm = pbd::practice::movement::wavedash_passes(speed);
		std::uint32_t shortest = rhythm.passes[0].cycle(), longest = shortest;
		for (std::size_t i = 1; i < rhythm.count; ++i)
		{
			shortest = (std::min)(shortest, rhythm.passes[i].cycle());
			longest = (std::max)(longest, rhythm.passes[i].cycle());
		}
		const std::string every = shortest == longest ? std::to_string(shortest) : std::to_string(shortest) + " or " + std::to_string(longest);
		const bool lightdash = pbd::practice::movement::clamp_wavedash_speed(speed) == pbd::practice::movement::max_wavedash_speed;
		return "One crouch dash every " + every + " frames" + (lightdash ? ", each input one frame: a lightdash." : ".");
	}
	static_assert(dummy_rows::slot_count == reaction_slots, "Offense reacts with any Defense Dummy Action slot");

	// Practice Settings' row count in each training mode. CPU, while unavailable, shows only the Training Mode row.
	inline constexpr std::uint32_t practice_row_count(training_mode mode) noexcept
	{
		return mode == training_mode::defense ? practice_rows::first + dummy_rows::count :
		       mode == training_mode::offense ? practice_rows::count :
		                                        practice_rows::first;
	}
	struct pbd_page_window
	{
		std::uint32_t first = 0, count = 0, selected = 0;
	};
	inline constexpr pbd_page_window pbd_visible_rows(std::uint32_t rows, std::uint32_t selected, std::uint32_t capacity) noexcept
	{
		if (!rows || !capacity)
			return {};
		selected = std::min(selected, rows - 1);
		const auto first = selected >= capacity ? selected - capacity + 1 : 0;
		return {first, std::min(rows - first, capacity), selected - first};
	}

	// The Defense row (a dummy_rows value) shown at a Practice Settings row, if that row belongs to the Defense setup.
	inline constexpr std::optional<std::uint32_t> defense_row(training_mode mode, std::uint32_t row) noexcept
	{
		if (mode != training_mode::defense || row < practice_rows::first || row - practice_rows::first >= dummy_rows::count)
			return std::nullopt;
		return row - practice_rows::first;
	}

	// Whether a Practice Settings row is Input Delay Frames, which both training modes list and share.
	inline constexpr bool input_delay_row(training_mode mode, std::uint32_t row) noexcept
	{
		return mode == training_mode::offense ? row == practice_rows::input_delay :
		                                        mode == training_mode::defense && row == practice_rows::first + dummy_rows::input_delay;
	}

	// The game option (0..4) shown at a Practice Settings row, if that row is one of Offense's game options.
	inline constexpr std::optional<std::uint32_t> game_option_row(training_mode mode, std::uint32_t row) noexcept
	{
		if (mode != training_mode::offense || row < practice_rows::first || row - practice_rows::first >= practice_rows::game_options)
			return std::nullopt;
		return row - practice_rows::first;
	}

	// What SELECT+Circle does in the fight. Ready to record: start the take; recording: stop it, as START does (taken
	// before any other chord, in either order: pbd::practice::controls::record_stop). In Defense it starts playback and,
	// pressed again, stops it. In Offense with a Movement chosen it turns the dummy's movement off and on again, whether or
	// not it is moving at that moment. Playback is always stoppable; elsewhere the chord only explains where playback lives.
	enum class circle_chord : std::uint8_t
	{
		none,
		start_recording,
		stop_recording,
		start_playback,
		stop_playback,
		toggle_movement,
		needs_defense
	};
	inline constexpr circle_chord select_circle(training_mode mode, dummy_phase phase, bool offense_movement = false) noexcept
	{
		const bool toggles = mode == training_mode::offense && offense_movement;
		switch (phase)
		{
		case dummy_phase::ready: return circle_chord::start_recording;
		case dummy_phase::recording: return circle_chord::stop_recording;
		case dummy_phase::playing: return toggles ? circle_chord::toggle_movement : circle_chord::stop_playback;
		case dummy_phase::idle: return mode == training_mode::defense ? circle_chord::start_playback :
			                           toggles                        ? circle_chord::toggle_movement :
			                                                            circle_chord::needs_defense;
		case dummy_phase::preview: break;
		}
		return circle_chord::none;
	}

	// Character and Stage Selection leave the fight through the game's own selection. An idle dummy goes at once; playback
	// (Defense's actions or Offense's Movement) stops first, as SELECT+Circle stops it, so one press is enough. A take being
	// recorded, waiting to record or previewed is the player's unfinished work and refuses.
	enum class selection_gate : std::uint8_t
	{
		go,
		stop_then_go,
		refuse
	};
	inline constexpr selection_gate selection_gate_for(dummy_phase phase) noexcept
	{
		switch (phase)
		{
		case dummy_phase::idle: return selection_gate::go;
		case dummy_phase::playing: return selection_gate::stop_then_go;
		case dummy_phase::recording:
		case dummy_phase::preview:
		case dummy_phase::ready: break;
		}
		return selection_gate::refuse;
	}

	// Whether the selection's row and question say that the playback stops. Offense's Movement and its reaction are settings:
	// they stop without a word and start again on their own in the new fight. Defense's recorded actions stay stopped until
	// SELECT+Circle, so the player is told.
	inline constexpr bool selection_names_stop(training_mode mode, selection_gate gate) noexcept
	{
		return gate == selection_gate::stop_then_go && mode != training_mode::offense;
	}

	// Character selection opens the native selector; stage selection retains the current fighters.
	inline std::string selection_help(bool characters, bool names_stop)
	{
		return std::string(names_stop ? "Stop the dummy's playback and pick " : "Pick ") +
		       (characters ? "new characters without leaving Practice." : "another stage with the same fighters.");
	}

	struct menu_option
	{
		std::string text;
		std::string note;
		std::string help;
		bool is_default = false;
		// An unavailable option is listed, dimmed, and Confirm on it does nothing.
		bool enabled = true;
	};

	struct dropdown
	{
		bool open = false;
		std::uint32_t row = 0;     // visible row the list belongs to
		std::uint32_t current = 0; // option in effect
		std::uint32_t focus = 0;   // highlighted option
		std::uint32_t count = 0;
		std::array<menu_option, max_options> options{};
	};

	inline constexpr float menu_panel_top = 112.f;

	// Top of the option stack on the 1280x720 canvas: the option in effect lines up with its row where the panel allows,
	// otherwise the stack is pushed inside the panel's list area (y 153..608).
	inline float dropdown_top(float row_top, std::uint32_t current, std::uint32_t count) noexcept
	{
		const float height = 30.f * static_cast<float>(count);
		const float wanted = row_top - 1.f - 30.f * static_cast<float>(current);
		return (std::max)(153.f, (std::min)(608.f - height, wanted));
	}

	// One L1/R1 step of a dummy slot's frequency weight, clamped to 0..10.
	inline std::uint32_t step_frequency(std::uint32_t value, bool up) noexcept
	{
		value = std::clamp(value, min_frequency, max_frequency);
		return up ? (std::min)(max_frequency, value + 1) : value > min_frequency ? value - 1 :
		                                                                           min_frequency;
	}

	// Moves a highlight through n entries with wrap-around (menus, dropdowns and dialogs all wrap).
	inline std::uint32_t wrap_step(std::uint32_t value, std::uint32_t count, bool down) noexcept
	{
		if (!count)
			return 0;
		value %= count;
		return down ? (value + 1) % count : (value + count - 1) % count;
	}

	// As wrap_step, but passes over rows that cannot take focus (bit n of skip set for row n). When every row is skipped
	// the highlight stays where it was.
	inline std::uint32_t wrap_step_over(std::uint32_t value, std::uint32_t count, bool down, std::uint32_t skip) noexcept
	{
		const auto skipped = [&](std::uint32_t row)
		{
			return row < 32 && ((skip >> row) & 1u) != 0;
		};
		std::uint32_t next = value;
		for (std::uint32_t i = 0; i < count; i++)
		{
			next = wrap_step(next, count, down);
			if (!skipped(next))
				return next;
		}
		return count ? value % count : 0;
	}

	// Questions the pause menu asks before acting. Each is a Yes / No dialog stacked over the menu and opens with No
	// highlighted, so a second press of Cross cannot act by accident.
	enum class confirm_kind : std::uint8_t
	{
		none,
		restore_defaults,
		character_selection,
		stage_selection,
		exit_practice,
		load_recordings,
		save_recordings
	};
	inline constexpr std::uint32_t answer_yes = 0, answer_no = 1, answer_count = 2;
	inline constexpr std::array<const char*, answer_count> answers{"Yes", "No"};

	struct confirm_text
	{
		std::string message;
		std::string detail;
	};

	// The dialog's message for a question asked from the category in view (Restore Defaults names its category, and in
	// Practice Settings the training mode whose rows it resets). playback_stops: Yes on a selection stops the dummy's playback
	// (selection_names_stop).
	inline confirm_text confirm_message(confirm_kind kind, std::uint32_t page, training_mode mode = training_mode::offense, bool playback_stops = false)
	{
		switch (kind)
		{
		case confirm_kind::restore_defaults:
			if (page == categories::practice_settings && mode == training_mode::defense)
				return {"Restore the default Defense settings?", "Recorded actions are kept."};
			if (page == categories::practice_settings && mode == training_mode::offense)
				return {"Restore the default Offense settings?", {}};
			if (page == categories::restart_settings)
				return {"Restore the default Restart Settings?", "A remembered position is kept."};
			if (page == categories::display_settings)
				return {"Restore the default Display Settings?", {}};
			return {};
		case confirm_kind::character_selection: return {"Go to Character Selection?", playback_stops ? "The dummy's playback stops." : ""};
		case confirm_kind::stage_selection: return {"Load this stage?", playback_stops ? "The dummy's playback stops." : ""};
		case confirm_kind::exit_practice: return {"Exit Practice?", "You return to the launcher."};
		case confirm_kind::load_recordings: return {"Replace the current recordings?", "The current actions and playback settings have unsaved changes."};
		case confirm_kind::save_recordings: return {"Replace this saved recording set?", "Its name, actions and playback settings will be replaced."};
		case confirm_kind::none: break;
		}
		return {};
	}

	// What the renderer draws for an open question: the message, an optional detail line and the Yes / No options.
	struct confirm_dialog
	{
		bool open = false;
		std::string message;
		std::string detail;
		std::uint32_t focus = answer_no;
	};

	// Restore Defaults writes the game's five practice options in one native commit, so their values travel packed in
	// one word: Dummy Action (0..10) in four bits, then Counter Hit, Game Input Display, Game Attack Data and Game Hit
	// Analysis in two bits apiece (none goes above 2).
	inline constexpr std::size_t game_option_count = 5;
	inline constexpr std::array<std::uint32_t, game_option_count> game_option_maximum{10, 1, 1, 2, 1};
	inline std::uint32_t pack_game_options(const std::array<std::uint32_t, game_option_count>& values) noexcept
	{
		std::uint32_t packed = values[0] & 0xfu;
		for (std::size_t i = 1; i < game_option_count; i++)
			packed |= (values[i] & 0x3u) << (4 + 2 * (i - 1));
		return packed;
	}

	inline std::array<std::uint32_t, game_option_count> unpack_game_options(std::uint32_t packed) noexcept
	{
		std::array<std::uint32_t, game_option_count> values{};
		values[0] = packed & 0xfu;
		for (std::size_t i = 1; i < game_option_count; i++)
			values[i] = (packed >> (4 + 2 * (i - 1))) & 0x3u;
		return values;
	}

	// Two-column pause menu: the category rail on the left and the focused category's list on the right. On the rail,
	// Up/Down change category (its list is shown at once), Right or Cross move focus into the list, and Circle resumes.
	// A category that cannot be entered ignores Right and Cross. In the list, Circle or Left hand focus
	// back to the rail; everything else belongs to the list itself.
	enum class rail_action : std::uint8_t
	{
		none,
		moved,
		entered,
		resume
	};

	struct column_focus
	{
		bool rail = true;
		std::uint32_t page = 0;
		std::uint32_t row = 0;
	};

	inline rail_action rail_step(column_focus& focus, std::uint16_t pressed, std::uint32_t pages, bool enterable = true) noexcept
	{
		if (!focus.rail)
			return rail_action::none;
		if (pressed & 0x0020)
			return rail_action::resume;
		if (pressed & 0x5000)
		{
			focus.page = wrap_step(focus.page, pages, (pressed & 0x4000) != 0);
			focus.row = 0;
			return rail_action::moved;
		}
		if (enterable && (pressed & (0x2000 | 0x0040)))
		{
			focus.rail = false;
			return rail_action::entered;
		}
		return rail_action::none;
	}

	inline bool list_back(std::uint16_t pressed) noexcept
	{
		return (pressed & (0x0020 | 0x8000)) != 0;
	}

} // namespace pbd::practice
