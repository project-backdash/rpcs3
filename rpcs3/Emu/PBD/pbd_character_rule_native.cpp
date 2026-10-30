#include "stdafx.h"
#include "pbd_character_rule_native.h"
#include "pbd_character_rule.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_hidden_selection.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"
#include <atomic>

LOG_CHANNEL(pbd_character_log, "PBD Character rule");

namespace pbd::character_rule
{
	namespace
	{
		std::atomic<u64> rule_generation{0};
		std::atomic<bool> ban{false};
		std::optional<u32> word(u32 address)
		{
			be_t<u32> value{};
			return vm::try_access(address, &value, sizeof(value), false) ? std::optional<u32>{u32{value}} : std::nullopt;
		}
		u32 checked_word(u64 address)
		{
			return address <= 0xffffffffu ? word(static_cast<u32>(address)).value_or(~0u) : ~0u;
		}
		call selection_call(ppu_thread& owner, u64 record, u32 character, bool pinned)
		{
			return {rule_generation.load(), static_cast<u64>(Emu.GetEmulationIdentifier()), owner.id, owner.gpr[2],
				checked_word(t5dr::game_mode_address), checked_word(t5dr::scene_address), checked_word(t5dr::subscene_address),
				checked_word(0x10094140), owner.lr, character, record, checked_word(record), pinned};
		}
		action availability(ppu_thread& owner, bool can_skip)
		{
			const u64 record = owner.lr == 0xaf538 ? owner.gpr[11] : owner.gpr[31] - 4;
			const bool pinned = can_skip && pbd::hidden_selection::pinned_binary() && checked_word(0x59010) == 0x4e800020 &&
			                    checked_word(0xaf534) == 0x4bfa9acd && checked_word(0xb0b68) == 0x4bfa8499;
			const auto observed = selection_call(owner, record, jinpachi, pinned);
			const auto result = selection_predicate(rule::ban_jinpachi, observed);
			if (result == predicate::unverified)
			{
				pbd_character_log.error("Eligibility witness: generation %u/%u owner 0x%x toc 0x%x mode %u scene %u/%u grid %u "
					"link 0x%x record 0x%x cursor %u, skippable %u pinned %u",
					observed.generation, observed.active_generation, observed.owner, observed.toc, observed.mode,
					observed.scene, observed.subscene, observed.grid_mode, observed.link, observed.record,
					observed.cursor, can_skip, observed.pinned);
				return action::refused;
			}
			owner.gpr[3] = result == predicate::available ? 1 : 0;
			return action::skip;
		}
		action saved_fighter(ppu_thread& owner, bool can_skip)
		{
			const u64 side = owner.gpr[3], record = owner.gpr[31];
			const u32 row = side <= 1 ? checked_word(0x10093f50u + 4u * static_cast<u32>(side)) : ~0u;
			if (side > 1 || row > 84 || record > 0xffffffffu || !can_skip || !pbd::hidden_selection::pinned_binary() ||
				checked_word(t5dr::toc - 0x726c) != 0x10093f50 || checked_word(0xb16c4) != 0x4bfa9355 ||
				checked_word(0x5aa18) != 0x38000070 || checked_word(0x5aa54) != 0x80630004 ||
				checked_word(0x5aa70) != 0x68630001)
				return action::refused;
			const u32 address = 0x4403c0 + 0x24 * row;
			if (!vm::check_addr(address, vm::page_readable, 2))
				return action::refused;
			const u32 character = +vm::read16(address);
			const auto result = saved_character_predicate(rule::ban_jinpachi,
				selection_call(owner, record, character, true), static_cast<u32>(side));
			if (result == predicate::unverified || character > jinpachi)
				return action::refused;
			if (result == predicate::stock)
				return action::stock;
			owner.gpr[3] = 0;
			return action::skip;
		}
		void trace_selection(ppu_thread& owner, u64 generation)
		{
			if (!pbd_character_log.trace || generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
				owner.id != t5dr::main_ppu_id || get_current_cpu_thread() != &owner || owner.gpr[2] != t5dr::toc ||
				checked_word(t5dr::game_mode_address) != 4 || checked_word(t5dr::scene_address) != 3 ||
				checked_word(t5dr::subscene_address) != 44 || checked_word(0x10094140) != 1 || !pbd::hidden_selection::pinned_binary())
				return;
			const u64 unlock = (u64{checked_word(0x100934a0)} << 32) | checked_word(0x100934a4);
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 record = 0x10093f58u + side * 0x78u;
				pbd_character_log.trace("Select counter=%u side=%u cursor=%u panel=%u base=%u row=%u pick=%u state=%u "
										"unlock=%x seed=%x ban=%u",
					checked_word(0x1008f07c), side, checked_word(record),
					checked_word(record + 4), checked_word(record + 8), checked_word(record + 0x10),
					checked_word(record + 0x48), checked_word(record + 0x20) >> 24, unlock,
					checked_word(owner.gpr[13] - 0x7000), banned(generation) ? 1u : 0u);
			}
		}
	} // namespace

	void configure(u64 generation, bool ban_jinpachi) noexcept
	{
		rule_generation.store(0, std::memory_order_release);
		ban.store(ban_jinpachi, std::memory_order_release);
		rule_generation.store(generation, std::memory_order_release);
	}
	bool banned(u64 generation) noexcept
	{
		return generation && rule_generation.load(std::memory_order_acquire) == generation && ban.load(std::memory_order_acquire);
	}
	bool normalize(ppu_thread& owner, u64 generation) noexcept
	{
		try
		{
			if (!banned(generation))
			{
				trace_selection(owner, generation);
				return true;
			}
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || owner.id != t5dr::main_ppu_id ||
				get_current_cpu_thread() != &owner || owner.gpr[2] != t5dr::toc)
				return false;
			const auto scene = word(t5dr::scene_address);
			if (!scene)
				return false;
			if (*scene != 3)
				return true;
			if (word(t5dr::subscene_address) != 44u || word(t5dr::game_mode_address) != 4u || !pbd::hidden_selection::pinned_binary())
				return false;
			const auto tables = pbd::hidden_selection::read_native_tables();
			if (!tables)
				return false;
			struct change
			{
				u32 record;
				selection_plan plan;
			};
			std::array<change, 2> changes{};
			for (u32 side = 0; side < changes.size(); ++side)
			{
				const u32 record = 0x10093f58u + side * 0x78u;
				std::array<be_t<u32>, 5> selected{};
				be_t<u64> private_mask{};
				if (!vm::try_access(record, selected.data(), sizeof(selected), false) ||
					!vm::try_access(record + 0x28, &private_mask, sizeof(private_mask), false))
					return false;
				const auto plan = initial_selection(*tables, u32{selected[0]}, u32{selected[1]},
					u32{selected[2]}, u32{selected[4]}, u64{private_mask});
				if (!plan || !vm::check_addr(record, vm::page_writable, sizeof(selected)))
					return false;
				changes[side] = {record, *plan};
			}
			// Validate both records before either plan changes guest selection state.
			for (const auto& change : changes)
			{
				if (change.plan.move)
					vm::write32(change.record, change.plan.cursor);
				if (change.plan.preview)
				{
					vm::write32(change.record + 8, change.plan.base);
					vm::write32(change.record + 0x10, change.plan.row);
				}
				if (change.plan.reroute)
					vm::write32(change.record + 4, change.plan.panel);
			}
			trace_selection(owner, generation);
			return true;
		}
		catch (...)
		{
			pbd_character_log.error("Character selection normalization failed");
			return false;
		}
	}
	action event(ppu_thread& owner, u32 address, bool can_skip) noexcept
	{
		if (!banned(static_cast<u64>(Emu.GetEmulationIdentifier())))
			return action::stock;
		try
		{
			action result = action::stock;
			if (address == 0x59000 && owner.gpr[3] == jinpachi && (owner.lr == 0xaf538 || owner.lr == 0xb0b6c))
				result = availability(owner, can_skip);
			else if (address == 0x5aa18 && owner.lr == 0xb16c8)
				result = saved_fighter(owner, can_skip);
			if (result == action::refused)
				pbd_character_log.error("Character eligibility call is unverified at 0x%x", address);
			return result;
		}
		catch (...)
		{
			pbd_character_log.error("Character eligibility call failed at 0x%x", address);
			return action::refused;
		}
	}
} // namespace pbd::character_rule
