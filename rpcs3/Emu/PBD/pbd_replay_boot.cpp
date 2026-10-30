#include "stdafx.h"
#include "pbd_replay_boot.h"
#include "Emu/Cell/pbd_rollback_session_boundary.h"
#include "pbd_replay_standby.h"
#include "Emu/Cell/timers.hpp"

namespace
{
	// Consecutive VS select boundaries seen by the replay of this latch generation (pbd::replay_boot::select_hold_boundaries).
	struct select_hold
	{
		u64 generation = ~0ull;
		u32 boundaries = 0;
	} g_select_hold;
} // namespace

bool pbd::rollback_session::boundary_call::wait_for_replay_boot()
{
	const bool held_checkpoint = pbd::replay_standby::pbd_requested();
	if ((!data.lobby.direct_match.enabled() && !held_checkpoint) || data.session.start_synced)
		return false;
	if (pc != pcs[0])
	{
		data.in_callback.store(false);
		return true;
	}
	// A held replay starts at the loaded checkpoint before it has a file. Its input witness must be captured here,
	// just as for the fresh replay below; configure_local_modes defers these checks for both replay launch modes.
	if (!held_checkpoint)
	{
		pbd::replay_boot::observation observed;
		observed.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(session_clock::now() - data.origin).count();
		observed.readable = read_word(pbd::t5dr::game_mode_address, observed.mode) &&
		                    read_word(pbd::t5dr::scene_address, observed.scene) &&
		                    read_word(pbd::t5dr::subscene_address, observed.subscene);
		if (const auto reason = pbd::replay_boot::refusal(observed))
		{
			pbd_session_log.error("Fresh replay start refused: %s", reason);
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		if (!pbd::replay_boot::ready(observed))
		{
			g_select_hold.boundaries = 0;
			data.in_callback.store(false);
			return true;
		}
		if (g_select_hold.generation != data.generation)
			g_select_hold = {data.generation, 0};
		if (++g_select_hold.boundaries < pbd::replay_boot::select_hold_boundaries)
		{
			data.in_callback.store(false);
			return true;
		}
		pbd_session_log.notice("Fresh replay start: VS select held for %u boundaries after %u ms", g_select_hold.boundaries,
			observed.elapsed_ms);
		// A fresh boot restores no timebase, so the battle clock has no anchor yet. The replay's guest time starts here, at VS
		// select, as a lobby game's starts at its VS checkpoint; the guest's reads before this point belong to the boot and its
		// menus, outside the replay.
		if (data.battle_clock.enabled && !data.battle_clock.anchor_set)
		{
			data.battle_clock.anchor = get_timebased_time();
			data.battle_clock.anchor_set = true;
			data.battle_clock.early_read = false;
		}
	}
	try
	{
		// ELF initialization observed the title's initial memory. Bind the host checks to this loaded or fresh select;
		// no recorded input or guest counter is written while reaching it.
		session_runtime.check_start_state();
		return false;
	}
	catch (const session_error& error)
	{
		pbd_session_log.error("Fresh replay start refused: %s", error.message);
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}
	catch (...)
	{
		pbd_session_log.error("Fresh replay start refused: replay_start_exception");
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}
}
