#pragma once

// The input hook's Practice memory for one run: the store it reads and writes (pbd_practice_store.h), the dummy whose rows
// and slots are live, and the saved game options waiting for their silent pause. It exists only
// when the launcher names a store for local Practice; every use is on the emulation owner thread.

#include "pbd_practice_store.h"
#include "pbd_practice_sets.h"
#include "../Cell/pbd_game_lifecycle.h"
#include "../Cell/pbd_hidden_selection.h"

#include <map>
#include <memory>
#include <optional>
#include <set>

namespace pbd::practice_store
{
	// Change checks run at most once in this many fresh boundaries; a change is written at the first check after it.
	inline constexpr u32 save_interval = 30;
	// A silent options restore that has not run in this many boundaries is given up and the fight resumes.
	inline constexpr u32 restore_limit = 300;

	// Restart Position as saved, and back. Remembered (tied to one stage load) and the round start are saved as Centre.
	inline restart_position saved_restart(pbd::game_lifecycle::placement value)
	{
		using pbd::game_lifecycle::placement;
		switch (value)
		{
		case placement::left_wall: return restart_position::left_wall;
		case placement::right_wall: return restart_position::right_wall;
		case placement::foreground_wall: return restart_position::foreground_wall;
		case placement::background_wall: return restart_position::background_wall;
		case placement::random: return restart_position::random;
		default: return restart_position::centre;
		}
	}
	inline pbd::game_lifecycle::placement live_restart(restart_position value)
	{
		using pbd::game_lifecycle::placement;
		constexpr std::array<placement, 6> live{placement::centre, placement::left_wall, placement::right_wall, placement::foreground_wall,
			placement::background_wall, placement::random};
		return live[static_cast<std::size_t>(value)];
	}
	// A new fight keeps the chosen Restart Position; only Remembered, whose position belongs to the last stage load, falls back to
	// Centre.
	inline pbd::game_lifecycle::placement kept_restart(pbd::game_lifecycle::placement value)
	{
		return value == pbd::game_lifecycle::placement::remembered ? pbd::game_lifecycle::placement::centre : value;
	}

	struct memory
	{
		explicit memory(writer::commit_function commit = commit_text)
			: files(std::move(commit))
		{
		}

		void autosave(profile current, std::optional<recordings> current_slots, std::string when)
		{
			if (current.options)
				current.options = restorable_options(*current.options);
			// Each future describes the current cache value. Consume it before updating the cache so an older success
			// cannot clear a newer change; failures stay dirty even after the active dummy changes.
			if (profile_write.valid() && profile_write.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
				profile_dirty = !profile_write.get();
			for (auto next = slot_writes.begin(); next != slot_writes.end();)
			{
				if (next->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				{
					++next;
					continue;
				}
				if (next->second.get())
					dirty_slots.erase(next->first);
				next = slot_writes.erase(next);
			}
			if (current != saved)
			{
				saved = std::move(current);
				profile_dirty = true;
				profile_write = {};
			}
			if (current_slots)
			{
				const auto character = current_slots->character;
				const auto found = library.find(character);
				if (found == library.end() || found->second != *current_slots)
				{
					library[character] = std::move(*current_slots);
					dirty_slots.insert(character);
					slot_writes.erase(character);
				}
			}
			if (profile_dirty && !profile_write.valid())
				profile_write = files.submit(profile_path(root), [value = saved, when]
					{
						return serialize(value, when);
					});
			for (const auto character : dirty_slots)
				if (!slot_writes.contains(character))
					slot_writes[character] = files.submit(recordings_path(root, character), [value = library.at(character), when]
						{
							return serialize(value, when);
						});
		}

		std::filesystem::path root;
		writer files;
		// Live settings and per-character slots, including changes whose disk writes are pending or failed.
		profile saved;
		std::map<u32, recordings> library;
		recording_sets sets;
		std::map<u32, u32> set_position;
		std::optional<std::pair<u32, u32>> load_set_requested;
		std::optional<recording_name_editor> set_editor;
		// The base character whose rows and slots are live, and the fight (session match) they were applied for.
		std::optional<u32> dummy;
		u32 applied_match = ~0u;
		u32 next_check = 0;
		std::optional<pbd::hidden_selection::tables> tables;
		// The saved game options, packed, while their silent pause is wanted (requested: the pause was asked for; queued: the
		// restore waits for the native call).
		std::optional<u32> restore;
		u32 restore_from = 0;
		bool restore_requested = false, restore_queued = false;

	private:
		bool profile_dirty = false;
		std::future<bool> profile_write;
		std::set<u32> dirty_slots;
		std::map<u32, std::future<bool>> slot_writes;
	};
} // namespace pbd::practice_store
