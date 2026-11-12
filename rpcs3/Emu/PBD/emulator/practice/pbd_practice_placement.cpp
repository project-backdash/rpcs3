#include "stdafx.h"
#include "pbd_practice_reset_internal.h"

namespace pbd::game_lifecycle
{
	using namespace detail;
	namespace
	{
		void rough_placement(ppu_thread& ppu)
		{
			// DR decides which fighter is drawn on the right from the camera's projection of their bones, and the reset's
			// camera initialization (81FE0) runs after wall_phase from bones the 14C4C(1) pose pass built. A pair written only at
			// wall_phase leaves those bones at the game's own spawn layout, so the camera and the screen-side rule (808D8) can
			// read the fighters on swapped sides (City Lights' Right Wall). Wall restarts therefore also write the rough pair at
			// reset_phase, as Centre and Remember do, so the pose pass builds bones there; the exact pair at wall_phase then
			// differs only by its distance from the wall, on the same line and facing. The exact write stays the authority: a
			// refused rough write leaves the restart to it alone.

			if (provisional_seen || phase_seen)
				return;
			provisional_seen = true;
			if (!reset_collision || !at_phase(ppu, reset_phase))
				return;
			try
			{
				// execute() read this collection moments ago inside the same native call; wall_phase reads it again.
				const auto rough = rough_wall_pair(*reset_collision, *active_reset);
				std::array<actor_placement, 2> written{};
				provisional_ok = rough && token_matches(*rough, active_reset->lifetime) && write_pair(*rough, written);
			}
			catch (...)
			{
				provisional_ok = false;
				placement_failure = "provisional_placement_exception";
			}
		}

	} // namespace
	void placement_phase(ppu_thread& ppu, u32 address) noexcept
	{
		if (!active_reset)
			return;
		const bool wall = wall_target(active_reset->target);
		if (wall && address == reset_phase)
		{
			rough_placement(ppu);
			return;
		}
		if (phase_seen)
			return;
		const u32 expected_phase = wall ? wall_phase : reset_phase;
		if (address != expected_phase)
			return;
		phase_seen = true;
		if (!at_phase(ppu, expected_phase))
			return;
		if (active_reset->target == placement::native_start)
		{
			const auto pair = actors();
			phase_ok = pair == std::array<u32, 2>{0x10090040, 0x10090910};
			for (u32 side = 0; side < 2 && phase_ok; ++side)
				phase_ok &= read_position(pair[side], phase_positions[side]);
			return;
		}
		placement_token destination;
		if (active_reset->target == placement::remembered || active_reset->target == placement::random)
		{
			if (!active_reset->remembered || !token_matches(*active_reset->remembered, active_reset->lifetime))
				return;
			if (active_reset->target == placement::random)
			{
				try
				{
					const auto current = read_collision(active_reset->lifetime, nullptr, true);
					if (!current || !reset_collision || !same_collision(*reset_collision, *current))
						return;
				}
				catch (...)
				{
					phase_ok = false;
					placement_failure = "random_placement_collision_exception";
					return;
				}
			}
			destination = *active_reset->remembered;
			destination.positions = pbd::practice::arranged(destination.positions, active_reset->variant);
		}
		else if (active_reset->target == placement::centre && centre_basis_matches())
		{
			destination.lifetime = active_reset->lifetime;
			destination.actors = actors();
			destination.positions = pbd::practice::arranged({actor_placement{static_cast<s32>(0xc47a0000u), 0, 0, 0x4000, 0x3fc90fdb},
																actor_placement{static_cast<s32>(0x447a0000u), 0, 0, -0x4000, 0xbfc90fdb}},
				active_reset->variant);
		}
		else if (wall && reset_collision)
		{
			try
			{
				phase_sides = read_sides();
				const auto current = read_collision(active_reset->lifetime, nullptr);
				if (!current || !same_collision(*reset_collision, *current))
					return;
				const auto exact = wall_destination(*current, *active_reset);
				if (!exact || !token_matches(*exact, active_reset->lifetime))
					return;
				destination = *exact;
			}
			catch (...)
			{
				phase_ok = false;
				placement_failure = "wall_placement_collision_exception";
				return;
			}
		}
		else
			return;
		// Centre and Remember follow the actor initializers. Wall clearance additionally waits for the same reset's
		// 14C4C(1) pose pass: before it, world bones can still describe the previous attack while canonical positions are new.
		// Both phases precede the remaining native camera/controller initialization; neither advances an extra game frame.
		phase_ok = write_pair(destination, phase_positions);
	}
	sides_sample read_sides() noexcept
	{
		using pbd::practice::native::word;
		sides_sample out;
		for (u32 side = 0; side < 2; ++side)
		{
			const u32 actor = side ? 0x10090910 : 0x10090040;
			auto& fighter = out.fighters[side];
			fighter.x = word(actor + 0x760);
			fighter.z = word(actor + 0x768);
			fighter.projected = word(actor + 0x6c8);
			fighter.right = pbd::practice::native::byte(actor + 0x1c3);
			fighter.move = word(actor + 0xc4);
			// Bone 0's world translation, where collision_body and the camera tracker (8570C) read it.
			const u32 owner = word(actor + 0x8a4);
			const u32 matrices = owner && owner != ~0u && !(owner & 3) ? word(owner + 0x10) : ~0u;
			if (matrices && matrices != ~0u && !(matrices & 15))
			{
				fighter.bone_x = word(matrices + 0x70);
				fighter.bone_z = word(matrices + 0x78);
			}
		}
		out.camera_mode = word(0x10099730 + 0x18c);
		out.camera_yaw = word(0x10099b10 + 0x2c);
		out.camera_frames = word(0x10099b10 + 0x48);
		for (u32 at = 0; at < 3; ++at)
		{
			out.eye[at] = word(0x10099b10 + at * 4);
			out.target[at] = word(0x10099b20 + at * 4);
		}
		out.flip_flag = pbd::practice::native::byte(0x1008ebd2);
		out.flip_mode = word(0x1009bca0);
		return out;
	}
} // namespace pbd::game_lifecycle
