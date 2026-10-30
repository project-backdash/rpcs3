#pragma once

// The player's volume. Host mutes (replay fast-forward, pause and speed-up, spectator catch-up and hidden playback, the in-game
// menu hold, the hidden lobby selection) set the game's volume to 0 and later restore it. They all restore to this one value,
// so a change the player makes while a mute is on (in the in-game Settings menu) is what comes back, instead of the volume
// that was saved when the mute began. One lock covers a mute and a player change, so a change made during a mute never
// unmutes the game early.

#include "Emu/system_config.h"

#include <algorithm>
#include <mutex>

namespace pbd::player_volume
{
	struct state_t
	{
		std::mutex mutex;
		int player = -1;
		bool muted = false;
	};

	inline state_t& state()
	{
		static state_t value;
		return value;
	}

	namespace detail
	{
		// The configured volume the first time anything asks. The launcher writes the player's volume into the config before the
		// game starts, and every mute asks here before it writes 0, so the muted 0 is never taken for the player's choice.
		inline int player(state_t& s)
		{
			if (s.player < 0)
				s.player = std::clamp(static_cast<int>(g_cfg.audio.volume.get()), 0, 200);
			return s.player;
		}
	} // namespace detail

	inline int get()
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		return detail::player(s);
	}

	inline void mute()
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		detail::player(s);
		s.muted = true;
		g_cfg.audio.volume.set(0);
	}

	inline void restore()
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		s.muted = false;
		g_cfg.audio.volume.set(detail::player(s));
	}

	// The mutes that are decided afresh at every boundary (replay speed and pause, spectator catch-up) pass their current answer.
	inline void apply(bool muted)
	{
		if (muted)
			mute();
		else
			restore();
	}

	// The player's choice: heard at once unless a host mute is on, in which case the mute's end brings it in.
	inline void set(int value)
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		s.player = std::clamp(value, 0, 200);
		if (!s.muted)
			g_cfg.audio.volume.set(s.player);
	}
} // namespace pbd::player_volume
