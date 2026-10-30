#include "pbd_test.h"
#include "Emu/PBD/pbd_pick_transition.h"

using pbd::test::check;
using namespace pbd::pick_transition;

namespace
{
	observation stale(unsigned side = 0)
	{
		observation state;
		state.pinned = true;
		state.mode = 4;
		state.manager = 4;
		state.scene = 2;
		state.model_state = 3;
		state.actor = 0x10090040 + side * 0x8d0;
		state.model_actor = state.actor;
		state.side = side;
		state.built_row = 0x16;
		state.row = state.selected_row = 0x1a;
		state.fighter = state.row_fighter = 9;
		state.costume = 1;
		state.bone_count = 1;
		state.clips = {0x216a4564, 0x218911cc};
		return state;
	}
}

PBD_TEST(PbdPickTransition, RepairTable)
{
	for (unsigned side = 0; side < 2; ++side)
	{
		for (unsigned alternative = 0; alternative < 2; ++alternative)
		{
			auto state = stale(side);
			state.slot_flags = alternative ? 0x4000 : 0;
			const auto write = plan(state);
			check(write.address == state.actor + 0x878);
			check(write.clips == std::array<std::uint32_t, 2>{0, 0});
			state.clips[alternative] = 0;
			check(plan(state).address == 0);
		}
		auto devil = stale(side);
		devil.fighter = devil.row_fighter = 27;
		devil.row = devil.selected_row = 0x45;
		devil.costume = 1;
		check(plan(devil).address == devil.actor + 0x878);
		devil.row = devil.selected_row = 0x44;
		devil.costume = 0;
		check(plan(devil).address == devil.actor + 0x878);
	}
}

PBD_TEST(PbdPickTransition, PreservesEveryOtherState)
{
	using change = void (*)(observation&);
	constexpr change refused[] = {
		[](auto& s) { s.pinned = false; },
		[](auto& s) { s.mode = 10; },
		[](auto& s) { s.manager = 0; },
		[](auto& s) { s.manager = 10; },
		[](auto& s) { s.scene = 3; },
		[](auto& s) { s.scene = 12; },
		[](auto& s) { s.scene = 17; },
		[](auto& s) { s.model_state = 2; },
		[](auto& s) { s.side = 2; },
		[](auto& s) { s.actor += 4; },
		[](auto& s) { s.model_actor += 0x8d0; },
		[](auto& s) { s.built_row = 85; },
		[](auto& s) { s.row = s.selected_row = 85; },
		[](auto& s) { s.selected_row = 0x19; },
		[](auto& s) { s.built_row = s.row; },
		[](auto& s) { s.row_fighter = 8; },
		[](auto& s) { s.fighter = s.row_fighter = 8; },
		[](auto& s) { s.costume = 0; s.row = s.selected_row = 0x19; },
		[](auto& s) { s.costume = 2; s.row = s.selected_row = 0x1b; },
		[](auto& s) { s.bone_count = 0; },
		[](auto& s) { s.loaded_clip = true; },
		[](auto& s) { s.clips = {0, 0}; },
	};
	for (unsigned side = 0; side < 2; ++side)
		for (const auto alter : refused)
		{
			auto state = stale(side);
			alter(state);
			check(plan(state).address == 0);
		}
}
