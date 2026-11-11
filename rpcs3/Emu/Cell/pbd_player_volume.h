#pragma once

// The player's volume. Host mutes (replay fast-forward, pause and speed-up, spectator catch-up and hidden playback, the in-game
// menu hold, the hidden lobby selection) set the game's volume to 0 and later restore it. They all restore to this one value,
// so a change the player makes while a mute is on (in the in-game Settings menu) is what comes back, instead of the volume
// that was saved when the mute began. One lock covers a mute and a player change, so a change made during a mute never
// unmutes the game early.

#include "Emu/system_config.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_player_audio.h"

#include <algorithm>
#include <mutex>

namespace pbd::player_volume
{
	struct state_t
	{
		std::mutex mutex;
		int player = -1;
		bool muted = false;
		bool player_muted = []
		{
			const char* settings = pbd::env::find("PBD_GAME_SETTINGS_JSON");
			return pbd::player_audio::startup_muted(settings ? settings : "");
		}();
		std::uint64_t launcher_revision = 0;
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
		detail::player(s);
		s.muted = false;
		g_cfg.audio.volume.set(pbd::player_audio::audible_volume(s.player, s.player_muted, false));
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
		detail::player(s);
		s.player = std::clamp(value, 0, 200);
		g_cfg.audio.volume.set(pbd::player_audio::audible_volume(s.player, s.player_muted, s.muted));
	}

	struct snapshot
	{
		int volume;
		bool muted;
		std::uint64_t revision;
	};

	inline snapshot read()
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		return {detail::player(s), s.player_muted, s.launcher_revision};
	}

	// A single authenticated command changes both choices under the same lock as temporary host mutes.
	inline void set(int volume, bool muted, std::uint32_t revision)
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		s.player = std::clamp(volume, 0, 100);
		s.player_muted = muted;
		s.launcher_revision = revision;
		g_cfg.audio.volume.set(pbd::player_audio::audible_volume(s.player, s.player_muted, s.muted));
	}

	inline int output()
	{
		auto& s = state();
		std::lock_guard lock(s.mutex);
		// Output can run during hidden preparation, before the game receives the player's configured volume.
		return pbd::player_audio::audible_volume(static_cast<int>(g_cfg.audio.volume.get()), s.player_muted, s.muted);
	}
} // namespace pbd::player_volume
