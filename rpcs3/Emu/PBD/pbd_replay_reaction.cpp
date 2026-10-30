#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_carry_guard.h"
#include "pbd_t5dr_memory.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm.h"

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool condition_halfword(u32 actor, u32 offset, std::uint8_t& value)
		{
			be_t<u16> word{};
			if (!vm::try_access(actor + offset, &word, sizeof(word), false) || word > 1)
				return false;
			value = static_cast<std::uint8_t>(word);
			return true;
		}
		bool write_condition(u32 actor, u32 offset, std::uint8_t value)
		{
			be_t<u16> word = value;
			return value <= 1 && vm::try_access(actor + offset, &word, sizeof(word), true);
		}
	} // namespace
	bool read_actor_reaction_conditions(ppu_thread& ppu, u64 generation, actor_reaction_conditions& out) noexcept
	{
		try
		{
			constexpr std::array signatures{mem::reaction_conditions_update};
			if (!actor_context(ppu, generation, signatures))
				return false;
			actor_reaction_conditions sample;
			for (u32 side = 0; side < 2; ++side)
			{
				const auto actor = mem::actors + side * mem::actor_stride;
				if (!condition_halfword(actor, mem::actor_reaction_command_11e, sample.command_11e[side]) ||
					!condition_halfword(actor, mem::actor_reaction_command_22, sample.command_22[side]))
					return false;
			}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_reaction_conditions(
		ppu_thread& ppu, u64 generation, const actor_reaction_conditions& saved) noexcept
	{
		try
		{
			actor_reaction_conditions observed;
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!read_actor_reaction_conditions(ppu, generation, observed))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				const auto actor = mem::actors + side * mem::actor_stride;
				if (!write_condition(actor, mem::actor_reaction_command_11e, saved.command_11e[side]) ||
					!write_condition(actor, mem::actor_reaction_command_22, saved.command_22[side]))
					return false;
			}
			return read_actor_reaction_conditions(ppu, generation, observed) &&
			       observed.command_11e == saved.command_11e && observed.command_22 == saved.command_22;
		}
		catch (...)
		{
			return false;
		}
	}
	bool read_actor_last_reaction_code(ppu_thread& ppu, u64 generation, actor_last_reaction_code& out) noexcept
	{
		try
		{
			constexpr std::array signatures{mem::last_reaction_update};
			if (!actor_context(ppu, generation, signatures))
				return false;
			actor_last_reaction_code sample;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u16> code{};
				if (!vm::try_access(mem::actors + side * mem::actor_stride + mem::actor_last_reaction_code, &code,
						sizeof(code), false))
					return false;
				sample.codes[side] = code;
			}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_last_reaction_code(ppu_thread& ppu, u64 generation, const actor_last_reaction_code& saved) noexcept
	{
		try
		{
			actor_last_reaction_code observed;
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!read_actor_last_reaction_code(ppu, generation, observed))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u16> code = saved.codes[side];
				if (!vm::try_access(mem::actors + side * mem::actor_stride + mem::actor_last_reaction_code, &code,
						sizeof(code), true))
					return false;
			}
			return read_actor_last_reaction_code(ppu, generation, observed) && observed.codes == saved.codes;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
