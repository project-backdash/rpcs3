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
		bool retained_context(ppu_thread& owner, u64 generation)
		{
			constexpr std::array signatures{mem::segment_delta_store, mem::segment_delta_copy,
				mem::prior_life_store, mem::reaction_scalar_store, mem::previous_blend_store,
				mem::previous_blend_trigger, mem::previous_blend_read};
			return actor_context(owner, generation, signatures);
		}
		bool retained_word(u32 address, u32& bits, bool write)
		{
			be_t<u32> word = bits;
			if (!vm::try_access(address, &word, sizeof(word), write))
				return false;
			if (!write)
				bits = word;
			return true;
		}
		bool delta_access(actor_segment_deltas& saved, bool write)
		{
			for (u32 side = 0; side < saved.ports.size(); ++side)
			{
				const u32 actor = mem::actors + side * mem::actor_stride;
				auto& port = saved.ports[side];
				if (!vector_access(actor + mem::actor_segment_delta, port.own_bits, write) ||
					!vector_access(actor + mem::actor_copied_segment_delta, port.copied_bits, write))
					return false;
			}
			return true;
		}
		bool scalar_access(actor_retained_scalars& saved, bool write)
		{
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 actor = mem::actors + side * mem::actor_stride;
				if (!retained_word(actor + mem::actor_prior_life, saved.prior_life[side], write) ||
					!retained_word(actor + mem::actor_reaction_scalar, saved.reaction_scalar[side], write) ||
					!retained_word(actor + mem::actor_previous_blend_weight, saved.previous_blend_weight[side], write))
					return false;
			}
			return true;
		}
		bool apply_scalar(u32 offset, const std::optional<actor_scalar_bits>& saved)
		{
			if (!saved)
				return true;
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 address = mem::actors + side * mem::actor_stride + offset;
				u32 word = saved->bits[side], observed = 0;
				if (!retained_word(address, word, true) || !retained_word(address, observed, false) ||
					observed != word)
					return false;
			}
			return true;
		}
	} // namespace
	bool read_actor_segment_deltas(ppu_thread& owner, u64 generation, actor_segment_deltas& out) noexcept
	{
		try
		{
			actor_segment_deltas sample;
			if (!retained_context(owner, generation) || !delta_access(sample, false))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_segment_deltas(ppu_thread& owner, u64 generation, const actor_segment_deltas& saved) noexcept
	{
		try
		{
			if (!owner_ready(owner, generation) || owner.cia != mem::match_initialize || owner.gpr[3] != 2 ||
				!retained_context(owner, generation))
				return false;
			auto copy = saved;
			actor_segment_deltas observed;
			return delta_access(copy, true) && read_actor_segment_deltas(owner, generation, observed) &&
			       observed.ports == saved.ports;
		}
		catch (...)
		{
			return false;
		}
	}
	bool read_actor_retained_scalars(ppu_thread& owner, u64 generation, actor_retained_scalars& out) noexcept
	{
		try
		{
			actor_retained_scalars sample;
			if (!retained_context(owner, generation) || !scalar_access(sample, false))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_retained_scalars(ppu_thread& owner, u64 generation, const game_values& saved) noexcept
	{
		try
		{
			if (!owner_ready(owner, generation) || owner.cia != mem::match_initialize || owner.gpr[3] != 2 ||
				!retained_context(owner, generation))
				return false;
			return apply_scalar(mem::actor_prior_life, saved.prior_life) &&
			       apply_scalar(mem::actor_reaction_scalar, saved.reaction_scalar) &&
			       apply_scalar(mem::actor_previous_blend_weight, saved.previous_blend_weight);
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
