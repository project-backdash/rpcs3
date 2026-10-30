#pragma once

#include "Emu/PBD/pbd_env.h"

#include <atomic>
#include <cstdlib>
#include <string_view>

// Whether this process runs a session: an online set, Practice, a replay or spectating. Every path that starts one sets
// PBD_SESSION=1, and the latch refuses the Practice, replay, spectator and lobby-set modes without it, so this one
// variable answers for all of them. Its value is cached on the first call.
//
// In a session the game runs on two PCs in lockstep (or plays recorded inputs that must replay exactly). Host controls
// that act on one PC only (pause, savestates, restart, the frame limit, the Home Menu) check this, because acting on them
// would end the session, desynchronize the two games or change pacing on one side.
namespace pbd::session_mode
{
	inline bool active() noexcept
	{
		static const bool value = []
		{
			const char* text = pbd::env::find("PBD_SESSION");
			return text && std::string_view(text) == "1";
		}();
		return value;
	}

	// Offline play: local Versus from the setup's VS character select checkpoint, with no session (PBD_OFFLINE_PLAY=1, never
	// beside PBD_SESSION=1). The launcher sets no other PBD switch for it, so the input hook never arms and the game keeps
	// its own speed; this answers for the in-game menu, the second local player's port and Versus (pbd_versus.h).
	inline bool offline_play_requested(const char* offline, const char* session) noexcept
	{
		return offline && std::string_view(offline) == "1" && !(session && std::string_view(session) == "1");
	}

	inline bool offline_play() noexcept
	{
		static const bool value =
			offline_play_requested(pbd::env::find("PBD_OFFLINE_PLAY"), pbd::env::find("PBD_SESSION"));
		return value;
	}

	// The player closed the game window (its close button, Alt+F4 or the window menu). The game window sets this just before it
	// asks the emulator to stop, so the update latch can tell that deliberate close from every other stop: a lobby set reads it
	// as the player leaving the seat. A crash, a guard failure, a peer's end or a stop the launcher asks for never sets it.
	inline std::atomic<bool> window_closed{false};
	inline void note_window_closed() noexcept
	{
		window_closed.store(true, std::memory_order_release);
	}
	inline bool window_closed_by_player() noexcept
	{
		return window_closed.load(std::memory_order_acquire);
	}
} // namespace pbd::session_mode
