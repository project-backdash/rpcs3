#include "stdafx.h"
#include "pbd_stage_rule_native.h"
#include "pbd_stage_rule.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_hidden_selection.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"
#include <atomic>

LOG_CHANNEL(pbd_stage_log, "PBD Stage rule");

namespace pbd::stage_rule
{
	namespace
	{
		std::atomic<u64> applied_generation{0};
		u32 word(u32 address)
		{
			be_t<u32> value{};
			return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
		}
	} // namespace

	bool applied(u64 generation) noexcept
	{
		return generation && applied_generation.load(std::memory_order_acquire) == generation;
	}

	bool apply(ppu_thread& owner, u64 generation) noexcept
	{
		try
		{
			if (applied(generation))
				return true;
			if (!generation || generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
				owner.id != t5dr::main_ppu_id || get_current_cpu_thread() != &owner || owner.gpr[2] != t5dr::toc)
				return false;
			u8 value = 0;
			if (!vm::try_access(unlock_byte, &value, 1, false))
				return false;
			const observation seen{pbd::hidden_selection::pinned_binary(), word(save_object_slot), word(unlock_pointer),
				word(unlock_setting), value};
			const auto next = unlocked(seen);
			if (!next || !vm::check_addr(unlock_byte, vm::page_writable, 1))
			{
				pbd_stage_log.error("Save witness: pinned %u slot 0x%x pointer 0x%x setting 0x%x", seen.pinned,
					seen.save_slot, seen.pointer, seen.setting);
				return false;
			}
			vm::write8(unlock_byte, *next);
			applied_generation.store(generation, std::memory_order_release);
			pbd_stage_log.notice("Original stages available: unlock byte 0x%02x -> 0x%02x", value, *next);
			return true;
		}
		catch (...)
		{
			pbd_stage_log.error("Original stages could not be made available");
			return false;
		}
	}
} // namespace pbd::stage_rule
