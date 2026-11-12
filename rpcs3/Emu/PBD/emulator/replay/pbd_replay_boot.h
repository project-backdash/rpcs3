#pragma once

#include <cstdint>

namespace pbd::replay_boot
{

	struct observation
	{
		std::uint32_t mode = 0, scene = 0, subscene = 0;
		std::uint64_t elapsed_ms = 0;
		bool readable = false;
	};
	// Waiting leaves title/menu input to the ordinary game. A battle is never a fresh replay entry.
	inline const char* refusal(const observation& value) noexcept
	{
		if (!value.readable)
			return "replay_start_unreadable";
		if (value.scene == 16 || value.scene == 17)
			return "replay_start_in_battle";
		if (value.elapsed_ms >= 120000)
			return "replay_start_timeout";
		return nullptr;
	}
	// The setup takes its VS checkpoint only once the select screen has held for this many polls: the screen's own loading
	// finishes meanwhile, and the game's resource request is refused while it runs. A replay waits as long before it starts.
	constexpr std::uint32_t select_hold_boundaries = 120;
	inline bool ready(const observation& value) noexcept
	{
		return !refusal(value) && value.mode == 4 && value.scene == 3 && value.subscene == 44;
	}

} // namespace pbd::replay_boot
