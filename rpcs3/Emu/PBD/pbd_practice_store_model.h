#pragma once

// Practice remembers its last settings between launches. Two kinds of file sit in the launcher's store directory:
// profile.json (pbd-practice-profile-1) holds the player's own settings and each dummy character's
// Offense and Defense rows; recordings/<base character>/autosave.json (pbd-practice-recordings-1) holds that dummy's
// eight Dummy Action slots and its playback interval. Every value is clamped to what the menu offers when it is read,
// so a hand-edited or older file can only select a setting the menu could have selected. Nothing here touches guest
// memory or the game's random state: these are host settings, applied by the input hook's own code.

#include "pbd_json_reader.h"
#include "pbd_practice_guard.h"
#include "pbd_practice_input_delay.h"
#include "pbd_practice_wakeup.h"
#include "pbd_practice_throw_escape.h"
#include "pbd_dummy_move_store.h"
#include "Emu/PBD/pbd_practice_presentation.h"

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pbd::practice_store
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;

	inline constexpr std::string_view profile_schema = "pbd-practice-profile-1";
	inline constexpr std::string_view recordings_schema = "pbd-practice-recordings-1";
	inline constexpr u32 slot_count = 8, max_take_words = 1800, max_slot_name_bytes = 32;
	// The character table's rows and the game's base characters (costumes share one).
	inline constexpr u32 max_character_row = 84, base_characters = 35;
	inline constexpr u32 default_interval = 3, max_interval = 3;
	// Node and string budgets for the reader: a slot's words are 4,800 base64 characters at most.
	inline constexpr std::size_t max_nodes = 8192, max_string_bytes = 8192;

	// Restart Position as saved. Remembered is tied to one stage load, so it is saved as Centre.
	enum class restart_position : std::uint8_t
	{
		centre,
		left_wall,
		right_wall,
		foreground_wall,
		background_wall,
		random
	};
	inline constexpr std::array<std::string_view, 6> restart_names{"centre", "left_wall", "right_wall", "foreground_wall",
		"background_wall", "random"};

	struct display
	{
		std::array<bool, 2> command_history{true, true};
		bool attack_info = true, frame_data = true, control_hints = true, movement_streaks = true;
		bool operator==(const display&) const = default;
	};
	// Offense's own dummy rows. reaction is Action After a Hit or Block; a Moves reaction's moves are dummy_rows::reactions.
	struct offense_rows
	{
		pbd::practice::movement::preset movement = pbd::practice::movement::preset::none;
		u32 wavedash_speed = pbd::practice::movement::default_wavedash_speed;
		pbd::practice::contact_reaction reaction;
		pbd::practice::contact_counted counted = pbd::practice::contact_counted::either;
		u32 times = pbd::practice::min_reaction_times;
		bool operator==(const offense_rows&) const = default;
	};
	struct defense_rows
	{
		pbd::practice::movement::preset movement = pbd::practice::movement::preset::none;
		u32 wavedash_speed = pbd::practice::movement::default_wavedash_speed;
		pbd::practice::movement::wavedash_end wavedash_end = pbd::practice::movement::wavedash_end::crouch_dash;
		pbd::practice::movement::dash_end dash_end = pbd::practice::movement::dash_end::into_move;
		bool operator==(const defense_rows&) const = default;
	};
	struct dummy_rows
	{
		offense_rows offense;
		defense_rows defense;
		pbd::practice::pbd_wakeup_option wakeup = pbd::practice::pbd_wakeup_option::stand_up;
		pbd::practice::throw_escape_option throw_escape = pbd::practice::throw_escape_option::none;
		practice::dummy_move::settings reactions;
		bool operator==(const dummy_rows&) const = default;
	};
	using game_options = std::array<u32, pbd::practice::game_option_count>;
	inline game_options restorable_options(game_options options) noexcept
	{
		// Zero is an uninitialized Attack Data value; committing it hides the game's HUD.
		if (options[3] == 0)
			options[3] = 1;
		return options;
	}
	struct last_pick
	{
		u32 player_row = 0, dummy_row = 0, stage = 0;
		bool operator==(const last_pick&) const = default;
	};
	struct profile
	{
		pbd::practice::training_mode mode = pbd::practice::training_mode::offense;
		display view;
		restart_position restart = restart_position::centre;
		// Restart Settings' Dummy Direction (Back) and Switch Positions, and Input Delay Frames (0 is Not Set).
		bool dummy_back = false, switch_positions = false;
		u32 input_delay = 0;
		std::optional<game_options> options;
		// Practice's own choices on top of two game options: Guard All's guard choice (Dummy Action) and Counter Hit's Random.
		pbd::practice::pbd_guard_option guard = pbd::practice::pbd_guard_option::game;
		bool counter_hit_random = false;
		// Keyed by the dummy's base character.
		std::map<u32, dummy_rows> dummies;
		std::optional<last_pick> pick;
		bool operator==(const profile&) const = default;
	};
	// One Dummy Action slot: the take's opponent-relative words and how playback uses them, and the moves chosen for it from
	// the move list (ids in the dummy's loaded list, ascending), which it plays instead while there are any.
	struct slot
	{
		std::vector<u16> words;
		u32 leading_neutral = 0, trailing_neutral = 0, stop_chord = 0;
		bool trim = true, enabled = true;
		std::string name;
		u32 frequency = 1, interval = 1;
		std::vector<u32> moves;
		bool operator==(const slot&) const = default;
	};
	struct recordings
	{
		u32 character = 0;
		u32 interval = default_interval;
		std::array<slot, slot_count> slots{};
		bool operator==(const recordings&) const = default;
	};

} // namespace pbd::practice_store
