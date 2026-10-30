#pragma once

#include "util/types.hpp"

// Test-only guest memory dumps at a game's match initializer (pc 0x159f0, r3 = 2), the anchor where a replay writes the
// values it carries from the recording. Snapshots taken at a game's first input come after its first frame, which may
// already have diverged; a dump at the anchor shows what the replay's memory still lacks before that frame runs. Both
// switches are off unless set, and neither changes a recording.
namespace pbd::anchor_dump
{
	// PBD_ANCHOR_DUMP=<folder>: writes the anchor's guest memory, the game's data block and the areas it allocates
	// from, to <folder>/anchor-<game>-<index>.bin, `index` being the lowest number from 1 not yet taken, so a live game
	// and its replay can share a folder. `side` names the run in the log.
	void write(u32 game, const char* side) noexcept;

	// Called by replay playback at the match initializer only, after its carry. With PBD_TEST_ANCHOR_PASTE=<dump>;
	// <begin>-<end>[,<begin>-<end>...] (hex, `end` excluded) it first copies those ranges from the dump into guest memory
	// and logs what each one changed, skipping and counting the bytes the dump lacks or the guest cannot write. Then it
	// writes the anchor dump, the memory the replay's first frame starts from. False when the request or the dump cannot
	// be read or a range wrote nothing, so a replay never runs on as if pasted when it was not.
	bool replay_anchor(u32 game) noexcept;

	// True when a replay must not start: a test switch is set without the replay's opt-in (pbd_replay_switches.h). The
	// log names the switch.
	bool replay_refuses_test_switches() noexcept;
} // namespace pbd::anchor_dump
