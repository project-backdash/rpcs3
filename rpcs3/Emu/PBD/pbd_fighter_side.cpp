#include "stdafx.h"
#include "pbd_native_call.h"

namespace pbd::fighter_side
{
	bool read(const ppu_thread& owner, u64 generation, screen_sides& out) noexcept
	{
		try
		{
			const auto native_word = pbd::practice::native::word;
			const u32 mode = native_word(pbd::t5dr::game_mode_address), scene = native_word(pbd::t5dr::scene_address),
					  draw = native_word(0x10098f90);
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
				get_current_cpu_thread() != &owner || !pbd::practice::native::boundary_ready(owner) ||
				(mode != 4 && mode != 10) || (scene != 16 && scene != 17) ||
				native_word(pbd::t5dr::toc - 0x1c88) != 0x10091ab0 ||
				native_word(0x10091ab0) != 0x10090040 || native_word(0x10091ab4) != 0x10090910 ||
				native_word(pbd::t5dr::toc - 0x1c18) != 0x10098f90 ||
				draw == ~0u || (draw & 0xc0) != 0xc0)
				return false;
			// 28AB40 loads the camera at80E34(0), multiplies each fighter's world transform,
			// divides by homogeneous W and writes the projected X at actor+6C8. 808D8 compares it.
			thread_local u64 verified_generation = ~u64{0};
			if (verified_generation != generation)
			{
				if (!pbd::practice::native::body_matches(0x28ab40, 0x180,
						"b512df8237bf41386abae956e1fb9a6b4662bbca6cefd2f12637ea401dd86b57") ||
					!pbd::practice::native::body_matches(0x808d8, 0x24,
						"d3c85c3e148fc65edfbeff07de5b6c29a2bcc07a51391668d247f58303c5040c"))
					return false;
				verified_generation = generation;
			}
			screen_sides sample;
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 actor = side ? 0x10090910 : 0x10090040;
				const u32 model = native_word(actor + 0x8a4);
				be_t<s32> x{};
				if (!model || model == ~0u || !vm::try_access(actor + 0x6c8, &x, sizeof(x), false))
					return false;
				sample.projected_x[side] = x;
			}
			if (sample.projected_x[0] == sample.projected_x[1] ||
				generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return false;
			sample.on_right = {sample.projected_x[0] > sample.projected_x[1], sample.projected_x[1] > sample.projected_x[0]};
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	// How DR turns a pad word's Left / Right into forward / back. The fighter input routine converts the directions through a
	// table of nine pad words per side (TOC-0x561C, numpad order 1..9) indexed by the fighter's byte +0x1C4 (0xED1FC loads it,
	// 0xED09C indexes the table): side 0 has forward (6) = Right, side 1 has forward = Left. The battle update copies +0x1C2
	// into +0x1C4 for both fighters (0x808FC, called at 0x1550C) before any fighter reads its input, and 0x808D8 sets +0x1C2
	// and +0x1C3 to whether the fighter's projected screen X (+0x6C8) is at least the other's. So +0x1C2 at a frame boundary is
	// exactly the side the next frame converts the word given at this boundary with, one frame of camera lag included, which
	// the projection read above can only approximate.
	bool read_input(u64 generation, input_sides& out) noexcept
	{
		try
		{
			const auto native_word = pbd::practice::native::word;
			const auto native_byte = pbd::practice::native::byte;
			const u32 mode = native_word(pbd::t5dr::game_mode_address), scene = native_word(pbd::t5dr::scene_address);
			if (!pbd::practice::native::supported() || generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
				(mode != 4 && mode != 10) || (scene != 16 && scene != 17) ||
				native_word(pbd::t5dr::toc - 0x7ef0) != 0x10091ab0 ||
				native_word(0x10091ab0) != 0x10090040 || native_word(0x10091ab4) != 0x10090910)
				return false;
			thread_local u64 verified_generation = ~u64{0};
			if (verified_generation != generation)
			{
				constexpr u32 table = 0x1000765c;
				constexpr std::array<u16, 18> directions{0xc000, 0x4000, 0x6000, 0x8000, 0, 0x2000, 0x9000, 0x1000, 0x3000,
					0x6000, 0x4000, 0xc000, 0x2000, 0, 0x8000, 0x3000, 0x1000, 0x9000};
				if (!pbd::practice::native::body_matches(0x808d8, 0x24, "d3c85c3e148fc65edfbeff07de5b6c29a2bcc07a51391668d247f58303c5040c") ||
					!pbd::practice::native::body_matches(0x808fc, 0x14, "d7da991367817bce59d1f84db1eb40e6be40dcae4afbe8fb9fbce60b2e79904c") ||
					!pbd::practice::native::body_matches(0x15504, 0xc, "6eaf0453e8870e45ceff9af6eb947c65ed35f47acd78d97f65b550019e8f1f54") ||
					!pbd::practice::native::body_matches(0xed09c, 0x40, "f1a0af66022e6a0c79ae106ce547f007142333cb407c16a03625ed4e73f7f13e") ||
					!pbd::practice::native::body_matches(0xed1ec, 0x20, "5a7ca4bdb6adf81ea63279e263149373c3b2cddf56b18e64babc9e7214970d6f") ||
					native_word(pbd::t5dr::toc - 0x561c) != table)
					return false;
				for (u32 at = 0; at < directions.size(); ++at)
					if (pbd::practice::native::halfword(table + at * 2) != directions[at])
						return false;
				verified_generation = generation;
			}
			input_sides sample;
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 value = native_byte((side ? 0x10090910 : 0x10090040) + 0x1c2);
				if (value > 1)
					return false;
				sample.on_right[side] = value == 1;
			}
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::fighter_side
