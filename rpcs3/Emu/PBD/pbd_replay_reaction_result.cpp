#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_carry_guard.h"
#include "pbd_replay_result_reference.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm.h"

LOG_CHANNEL(pbd_result_log, "PBD_REPLAY_RESULT");

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool result_context(ppu_thread& owner, u64 generation)
		{
			constexpr std::array signatures{mem::reaction_groups_load, mem::reaction_record_members_load,
				mem::reaction_result_store, mem::reaction_conditions_update, mem::com_payload_relocation};
			return actor_context(owner, generation, signatures);
		}
		bool read_word(u32 address, u32& value)
		{
			be_t<u32> word{};
			if (!vm::try_access(address, &word, sizeof(word), false))
				return false;
			value = word;
			return true;
		}
		bool result_catalog(u32 bank, u32 kind, replay_resource::catalog& table)
		{
			const auto spec = replay_result::member(kind);
			return spec.stride && read_word(bank + spec.catalog_offset, table.start) &&
			       read_word(bank + spec.catalog_offset + 4, table.count) &&
			       read_word(bank + spec.catalog_offset + 8, table.next) && replay_result::valid_catalog(kind, table);
		}
		bool resolve_result(u32 side, const reaction_result_ref& saved, u32& value)
		{
			value = 0;
			if (!saved.kind)
				return !saved.index && !saved.bank_style;
			if (saved.kind == 4)
			{
				const auto address = replay_result::resolve_bank_offset(saved, read_bank_bases(side));
				u32 mapped = 0;
				if (!address || !read_word(*address, mapped))
					return false;
				value = *address;
				return true;
			}
			u32 bank = 0;
			u16 style = 0;
			replay_resource::catalog table;
			if (!read_opponent_com_bank(side, bank, style) ||
				!result_catalog(bank, saved.kind == 3 ? 2 : saved.kind, table))
				return false;
			if (saved.kind == 3)
			{
				const auto address = replay_result::resolve_prefix(saved, bank, table, style);
				u32 mapped = 0;
				if (!address || !read_word(*address, mapped))
					return false;
				value = *address;
				return true;
			}
			const auto address = replay_result::resolve_word(saved, table, style);
			return address && read_word(*address, value);
		}
		// A word left from the previous game's image at this slot names no member of the image loaded now.
		bool identify_bank_offset(u32 side, u32 value, reaction_result_ref& out)
		{
			const auto bases = read_bank_bases(side);
			if (const auto offset = replay_bank_offset::identify(value, bases, replay_bank_offset::result_alignment))
			{
				const reaction_result_ref ref{4, offset->byte_offset, offset->bank_style, offset->base};
				u32 resolved = 0;
				if (resolve_result(side, ref, resolved) && resolved == value)
				{
					pbd_result_log.notice("Result 0x%x (side %u) kept as bankOffset %s+0x%x style %u", value, side,
						replay_bank_offset::name(ref.base), ref.index, ref.bank_style);
					out = ref;
					return true;
				}
			}
			pbd_result_log.error("Result is outside loaded COM members, prefix and banks: side=%u value=0x%x "
								 "opposing=0x%x/%u own=0x%x/%u",
				side, value, bases.opposing.base, bases.opposing.style, bases.own.base, bases.own.style);
			return false;
		}
		bool identify_result(u32 side, u32 value, reaction_result_ref& out)
		{
			out = {};
			if (!value)
				return true;
			u32 bank = 0;
			u16 style = 0;
			if (!read_opponent_com_bank(side, bank, style))
			{
				pbd_result_log.error("Opponent bank unreadable: side=%u value=0x%x", side, value);
				return false;
			}
			for (u32 kind = 1; kind <= 2; ++kind)
			{
				replay_resource::catalog table;
				if (!result_catalog(bank, kind, table))
				{
					pbd_result_log.error(
						"Result catalog invalid: side=%u kind=%u bank=0x%x start=0x%x count=%u next=0x%x", side, kind,
						bank, table.start, table.count, table.next);
					return false;
				}
				for (u32 index = 0; index < table.count; ++index)
				{
					const reaction_result_ref ref{kind, index, style};
					const auto address = replay_result::resolve_word(ref, table, style);
					u32 found = 0;
					if (!address || !read_word(*address, found))
					{
						pbd_result_log.error("Result member unreadable: side=%u kind=%u index=%u", side, kind, index);
						return false;
					}
					// Aliases retain the same word. The first exact member gives a stable file reference.
					if (found == value)
					{
						pbd_result_log.notice("Result 0x%x (side %u) kept as %s[%u] style %u", value, side,
							kind == 1 ? "commandGroup" : "recordEntry", index, style);
						out = ref;
						return true;
					}
				}
			}
			replay_resource::catalog records;
			const u64 base = u64{bank} + mem::com_payload_offset;
			const reaction_result_ref prefix{3, static_cast<u32>(value - base), style};
			u32 mapped = 0;
			// Style replacement retains this pointer while relocating the new COM image at the same bank.
			if (value >= base && result_catalog(bank, 2, records) &&
				replay_result::resolve_prefix(prefix, bank, records, style) == value && read_word(value, mapped))
			{
				pbd_result_log.notice("Result 0x%x (side %u) kept as dataPrefix+0x%x style %u", value, side, prefix.index, style);
				out = prefix;
				return true;
			}
			return identify_bank_offset(side, value, out);
		}
	} // namespace
	bool read_actor_reaction_result(ppu_thread& owner, u64 generation, actor_reaction_result& out) noexcept
	{
		try
		{
			if (!result_context(owner, generation))
				return false;
			actor_reaction_result sample;
			// Both sides are identified before refusing, so one game start names every result that cannot be carried.
			bool identified = true;
			for (u32 side = 0; side < 2; ++side)
			{
				u32 value = 0;
				if (!read_word(mem::actors + side * mem::actor_stride + mem::actor_reaction_result, value) ||
					!identify_result(side, value, sample.ports[side]))
					identified = false;
			}
			if (!identified)
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_reaction_result(ppu_thread& owner, u64 generation, const actor_reaction_result& saved) noexcept
	{
		try
		{
			if (!owner_ready(owner, generation) || owner.cia != mem::match_initialize || owner.gpr[3] != 2 ||
				!result_context(owner, generation))
				return false;
			std::array<u32, 2> values{};
			for (u32 side = 0; side < 2; ++side)
				if (!resolve_result(side, saved.ports[side], values[side]))
				{
					pbd_result_log.error("Saved result kind=%u offset=0x%x style=%u does not resolve in the loaded banks "
										 "(side %u)",
						saved.ports[side].kind, saved.ports[side].index, saved.ports[side].bank_style, side);
					return false;
				}
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 address = mem::actors + side * mem::actor_stride + mem::actor_reaction_result;
				be_t<u32> word = values[side];
				u32 observed = 0;
				if (!vm::try_access(address, &word, sizeof(word), true) || !read_word(address, observed) ||
					observed != values[side])
				{
					pbd_result_log.error("Reaction result 0x%x did not hold (side %u, read 0x%x)", values[side], side, observed);
					return false;
				}
			}
			pbd_result_log.notice("Reaction results written: 0x%x, 0x%x", values[0], values[1]);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
