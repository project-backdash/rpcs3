#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_carry_guard.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm.h"

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool segment_context(ppu_thread& owner, u64 generation)
		{
			constexpr std::array signatures{mem::bone_segment_start, mem::bone_segment_end};
			if (!actor_context(owner, generation, signatures))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				u8 count = 0;
				if (!vm::try_access(
						mem::actors + side * mem::actor_stride + mem::actor_bone_count, &count, sizeof(count), false) ||
					count > 4)
					return false;
			}
			return true;
		}
		bool segment_access(actor_bone_segments& saved, bool write)
		{
			for (u32 side = 0; side < saved.ports.size(); ++side)
			{
				for (u32 index = 0; index < saved.ports[side].size(); ++index)
				{
					const u32 address = mem::actors + side * mem::actor_stride + mem::actor_bone_segments +
					                    index * mem::bone_segment_bytes;
					auto& segment = saved.ports[side][index];
					if (!vector_access(address, segment.start_bits, write) ||
						!vector_access(address + 12, segment.end_bits, write))
						return false;
				}
			}
			return true;
		}
	} // namespace
	bool read_actor_bone_segments(ppu_thread& owner, u64 generation, actor_bone_segments& out) noexcept
	{
		try
		{
			actor_bone_segments sample;
			if (!segment_context(owner, generation) || !segment_access(sample, false))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_bone_segments(ppu_thread& owner, u64 generation, const actor_bone_segments& saved) noexcept
	{
		try
		{
			if (!owner_ready(owner, generation) || owner.cia != mem::match_initialize || owner.gpr[3] != 2 ||
				!segment_context(owner, generation))
				return false;
			auto copy = saved;
			actor_bone_segments observed;
			return segment_access(copy, true) && read_actor_bone_segments(owner, generation, observed) &&
			       observed.ports == saved.ports;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
