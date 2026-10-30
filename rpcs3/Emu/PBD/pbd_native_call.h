#pragma once

#include "../Cell/pbd_practice_native.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "../Cell/pbd_game_lifecycle.h"
#include "../Cell/pbd_practice_walls.h"
#include "pbd_practice_floor.h"
#include "pbd_practice_paint.h"
#include "pbd_practice_position.h"
#include "pbd_practice_frames.h"
#include "pbd_practice_reference.h"
#include "Emu/PBD/pbd_move_readiness.h"
#include "pbd_dummy_move_runtime.h"
#include "pbd_practice_commands.h"
#include "pbd_env.h"
#include "pbd_t5dr_memory.h"
#include "pbd_guest_call.h"
#include "pbd_owner_flags.h"
#include "pbd_stage_rule.h"
#include "../Cell/PPUThread.h"
#include "Crypto/sha256.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace pbd::practice::native
{
	struct frame_state
	{
		pbd::practice::frames::tracker tracker;
		pbd::practice::frames::calculator calculator;
		std::array<pbd::practice::reference::result, 2> references;
		u64 generation = 0;
		std::optional<u32> counter;
		struct terminal_window
		{
			u32 move = 0, start = 0, end = 0;
		};
		std::array<terminal_window, 2> windows{};
	};
	struct frame_observer : frame_state
	{
		std::array<pbd::practice::frames::event, 64> observations{};
		u32 observation_count = 0;
		void observe(u32 pc, unsigned side, u32 flags = 0, u32 first = 0, u32 last = 0, u32 target = 0);
	};
	inline constexpr u32 scratch_bytes = 0x8000, linkage_bytes = 0x100;
	extern thread_local bool calling;
	u32 word(u32 address) noexcept;
	u32 halfword(u32 address) noexcept;
	u32 byte(u32 address) noexcept;
	bool supported() noexcept;
	bool body_matches(u32 address, u32 bytes, const char* expected);
	bool preserved_call(ppu_thread& ppu, u32 entry, u32 r3, u32 r4, u32 staged_address = 0,
		const std::array<be_t<u32>, 8>* staged_settings = nullptr, u32* result_r3 = nullptr);
} // namespace pbd::practice::native
