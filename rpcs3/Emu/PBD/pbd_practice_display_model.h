#pragma once

// Practice display data is owned by the emulation boundary. The renderer receives a coherent host-only copy and never reads
// pads or guest memory. Keeping this view separate from the menu file contract lets local Practice share its existing renderer.

#include "pbd_dummy_move.h"
#include "pbd_env.h"
#include "pbd_practice_reference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "pbd_movement_observation.h"

namespace pbd::practice
{
	inline constexpr std::size_t history_size = 16;
	inline constexpr std::size_t max_prompts = 8;
	inline constexpr std::size_t max_pages = 8;
	inline constexpr std::uint16_t gameplay_mask = 0xf0ff;
	// Pad reads can occur on guest threads other than the boundary owner; the focus gate neutralizes both ports there too.
	inline std::atomic<bool> input_claim{false};

	// Categories follow the rail order.
	// Recording setup lives inside Practice Settings (Training Mode: Defense).
	namespace categories
	{
		inline constexpr std::uint32_t practice_settings = 0, restart_settings = 1, display_settings = 2, move_list = 3,
									   character_selection = 4, stage_selection = 5, settings = 6, exit_practice = 7, count = 8;
	}
	static_assert(categories::count <= max_pages);

	// Practice Settings starts with the Training Mode row. Offense lists the game's own dummy options; Defense lists
	// the dummy recording setup and lets SELECT+Circle start and stop playback in the fight; CPU would hand the dummy to the
	// game's own AI, which the game's practice options do not offer, so it is listed but unavailable.
	enum class training_mode : std::uint8_t
	{
		offense,
		defense,
		cpu
	};
	inline constexpr std::uint32_t training_mode_count = 3;

	// Every settings category ends with a Restore Defaults row; Cross on it asks before resetting that category.
	// Practice Settings rows: Training Mode, then the mode's own rows from `first`. Offense: the game's five options, the
	// dummy's Wake-up and Throw Escape, its Movement and Wavedash Speed, its Action After a Hit or Block with the Count and
	// After rows that say when it comes, Input Delay Frames, then Restore Defaults.
	namespace practice_rows
	{
		inline constexpr std::uint32_t mode = 0, first = 1, game_options = 5, wakeup = first + game_options, throw_escape = wakeup + 1,
									   movement = throw_escape + 1, wavedash_speed = movement + 1,
									   after_contact = wavedash_speed + 1, counted = after_contact + 1, times = counted + 1,
									   input_delay = times + 1, restore = input_delay + 1, count = restore + 1;
	}

	// Offense's Action After a Hit or Block: what the dummy does once it has recovered from the hit or block the Count and
	// After rows count. Between reactions the dummy follows the game's Dummy Action; the Movement Technique plays only with
	// None. Movement: one plain movement input (pbd::practice::movement::reaction_input), played once (block a move, then
	// backdash away: whiff-punish practice). Action: one recorded Dummy Action (a Defense slot), played once (block three
	// times, then counterattack). Moves: one of the moves chosen from the move list, at random when several, started on the
	// first frame the game lets the dummy act (pbd::practice::dummy_move).
	enum class after_contact : std::uint8_t
	{
		none,
		movement,
		action,
		moves
	};
	inline constexpr std::uint32_t after_contact_kinds = 4;
	// The row's options: None, the plain inputs in reaction_input order, Select from Move List, then Recording, which opens
	// the recorded Dummy Actions as a list of their own.
	inline constexpr std::uint32_t reaction_slots = 8, reaction_first_input = 1,
								   reaction_moves_option = reaction_first_input + pbd::practice::movement::reaction_input_count,
								   reaction_recording_option = reaction_moves_option + 1, after_contact_options = reaction_recording_option + 1;

	struct contact_reaction
	{
		after_contact kind = after_contact::none;
		std::uint32_t slot = 0;                                                                            // the Dummy Action slot (0..7) when kind is action
		pbd::practice::movement::reaction_input input = pbd::practice::movement::reaction_input::backdash; // when kind is movement
		friend constexpr bool operator==(const contact_reaction&, const contact_reaction&) = default;
	};
	// The reaction an option of the main list stands for. Recording's slot comes from its own list (slot 0 here).
	inline constexpr std::optional<contact_reaction> reaction_from_option(std::uint32_t option) noexcept
	{
		if (option >= after_contact_options)
			return std::nullopt;
		if (option == 0)
			return contact_reaction{};
		if (option < reaction_moves_option)
			return contact_reaction{after_contact::movement, 0, static_cast<pbd::practice::movement::reaction_input>(option - reaction_first_input)};
		return contact_reaction{option == reaction_moves_option ? after_contact::moves : after_contact::action};
	}
	// The main list's option for a reaction: the one it was chosen from.
	inline constexpr std::uint32_t reaction_option(const contact_reaction& value) noexcept
	{
		switch (value.kind)
		{
		case after_contact::none: return 0;
		case after_contact::movement:
			return reaction_first_input + (std::min)(static_cast<std::uint32_t>(value.input), pbd::practice::movement::reaction_input_count - 1);
		case after_contact::moves: return reaction_moves_option;
		case after_contact::action: return reaction_recording_option;
		}
		return 0;
	}

	inline constexpr std::uint32_t contact_counted_count = 3;
	inline constexpr std::string_view counted_name(contact_counted value) noexcept
	{
		return value == contact_counted::blocks ? "Blocks" : value == contact_counted::hits ? "Hits" :
		                                                                                      "Hits or Blocks";
	}
	// Each applied hit or block counts, including contacts inside a string. Once due, the reaction waits for recovery;
	// extra contacts extend that wait. A reaction, Restart, round start or changed setting starts the count again.
	struct reaction_counter
	{
		std::uint32_t seen = 0;
		bool due = false;
		void applied(bool hit, contact_counted counted, std::uint32_t times, std::uint32_t number = 1) noexcept
		{
			times = std::clamp(times, min_reaction_times, max_reaction_times);
			if (!due && counts(counted, hit))
			{
				seen += (std::min)(number, times - (std::min)(seen, times));
				due = seen >= times;
			}
		}
		bool ready() noexcept
		{
			if (!due)
				return false;
			reset();
			return true;
		}
		void reset() noexcept
		{
			seen = 0;
			due = false;
		}
	};

	// Restart Settings rows: the restart itself, where it puts both fighters, Remember Position, then what every restart
	// changes about that pair (pbd::game_lifecycle::placement_variant): the dummy's direction and Switch Positions.
	namespace restart_rows
	{
		inline constexpr std::uint32_t restart = 0, position = 1, remember = 2, dummy_direction = 3, switch_positions = 4, restore = 5,
									   count = 6;
	}

	// Display Settings rows: each side's command history, the Attack Info and Frame Data panels (one switch
	// each), the in-game control bar and the movement streak pop-up (pbd::practice::streak).
	namespace display_rows
	{
		inline constexpr std::uint32_t p1_history = 0, p2_history = 1, attack_info = 2, frame_data = 3, control_hints = 4, movement_streaks = 5,
									   restore = 6, count = 7;
	}
	enum class contact : std::uint8_t
	{
		unknown,
		hit,
		block
	};

	// A history row keeps the pad word as the game read it, so its arrows are the screen directions the stick was pushed:
	// forward is Left for a fighter on the right side. Replay and Analyze draw their columns the same way.
	struct command
	{
		std::uint16_t mask = 0;
		std::uint32_t frames = 0;
	};

	struct history
	{
		std::array<command, history_size> rows{};
		std::uint32_t count = 0;
	};

	// One fighter's attacks as the panels show them: fighters[0] holds what P1 did to P2, fighters[1] what P2 did to P1.
	struct feedback
	{
		// Empty values mean that no reader has established this fact; the panels show "---" for them. An animation cursor is
		// never frame advantage.
		// The last damaging hit: the opponent's health drop over one advancing frame.
		std::optional<std::uint32_t> damage;
		// Damage since the last restart. It starts at 0 on the first frame the health reader passes, so a 0 is a reading too.
		std::optional<std::uint32_t> total_damage;
		// The damaging hits and the damage of the latest combo (combo_tracker), like the game's own combo counter: a hit after
		// the combo ended starts the count again.
		std::optional<std::uint32_t> hits;
		std::optional<std::uint32_t> combo_damage;
		std::optional<std::uint32_t> startup;
		std::optional<std::int32_t> on_block, on_hit, on_whiff;
		// What the latest contact led to when it has no plain advantage ("Launch", "KND", "Juggle", "Wall", "Throw"; empty
		// otherwise): pbd::practice::frames::label_text, a static string.
		std::string_view result_label;
		// The latest contact's hit level ("High", "Mid", "Low", "Special Mid", "Special Low", "Unblockable", "Throw"; empty
		// when not read): pbd::practice::frames::hit_level_text, a static string.
		std::string_view hit_level;
		pbd::practice::reference::result calculated;
		contact outcome = contact::unknown;
		// The Frame Advantage text from both fighters' readiness ("+16 Crumple (-14)", pbd::practice::frames::advantage_text)
		// and the characters of the branch that was taken, drawn in the highlight colour. Empty when only a plain value is
		// known; the panel then shows that value, or result_label when there is none.
		std::string advantage_text;
		std::size_t advantage_marked_at = 0, advantage_marked_size = 0;
	};

	// One fighter's latest combo for Attack Info: the hits and damage of a run of hits the opponent has not recovered from
	// between. The defender's rest reading ends it once the defender is back on a resting stance (a fresh reading is never
	// trusted inside the input delay); without a reading, a pause longer than combo_gap_frames between two hits does, a little
	// longer than a juggle's longest wait for its next hit. A reading that never shows rest ends it after combo_cap_frames
	// without a hit, so one stuck reading cannot merge separate combos. A blocked attack leaves health alone, so it neither
	// counts nor ends a combo. The values stay until a hit after the combo starts the next one.
	inline constexpr std::uint32_t combo_gap_frames = 40, combo_rest_frames = 4, combo_cap_frames = 180;
	struct combo_tracker
	{
		bool active = false;
		std::uint32_t hits = 0, damage = 0, since_hit = 0;

		// One advancing frame: the damage the attacker dealt on it (0: none) and the defender's rest reading.
		void frame(std::uint32_t dealt, std::optional<bool> defender_rest) noexcept
		{
			if (dealt)
			{
				if (!active)
					*this = {true, 0, 0, 0};
				++hits;
				damage += dealt;
				since_hit = 0;
				return;
			}
			if (!active)
				return;
			++since_hit;
			if (since_hit > combo_cap_frames || (defender_rest ? *defender_rest && since_hit >= combo_rest_frames : since_hit > combo_gap_frames))
				active = false;
		}
		void reset() noexcept
		{
			*this = {};
		}
	};

	// A value no reader has established prints as three dashes.
	inline constexpr std::string_view unknown_value = "---";

	// A panel line: the label at the left, the value right-aligned. marked_size characters of the value from marked_at
	// are drawn in the highlight colour. A value too wide to sit beside the label is drawn beside short_label instead,
	// when the line has one; the value itself is never cut.
	struct panel_line
	{
		std::string_view label;
		std::string value;
		std::size_t marked_at = 0, marked_size = 0;
		std::string_view short_label{};
	};
	inline constexpr std::size_t attack_info_lines = 6, frame_data_lines = 2;
	inline constexpr std::size_t frame_reference_lines = 0, frame_observed_lines = frame_data_lines - frame_reference_lines;

	// Practice keeps pending phase 17 throughout a fight; VS uses phase 5. Phase 17's pause bit in the table at 0x1000126C
	// enables the native pause step (0x1D164 -> 0x4E3CC). Require the battle scene too: loading already uses phase 17 before
	// the arena exists. Other modes, scenes and phases hide the panels.
	inline constexpr std::uint32_t practice_mode = 10, practice_fight_scene = 17, practice_fight_phase = 17;
	inline bool practice_fight(std::uint32_t mode, std::uint32_t scene, std::uint32_t pending) noexcept
	{
		return mode == practice_mode && scene == practice_fight_scene && pending == practice_fight_phase;
	}

} // namespace pbd::practice
