#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_carry_guard.h"
#include "pbd_replay_actor_records.h"
#include "pbd_replay_resource_reference.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Memory/vm.h"

LOG_CHANNEL(pbd_cursor_log, "PBD_REPLAY_CURSOR");

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool reaction_cursor_context(ppu_thread& ppu, u64 generation)
		{
			constexpr std::array signatures{
				mem::com_records_load, mem::com_samples_load, mem::opponent_read, mem::reaction_advance};
			return actor_context(ppu, generation, signatures);
		}
		bool cursor_word(u32 address, u32& out)
		{
			be_t<u32> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return false;
			out = value;
			return true;
		}
		struct cursor_section
		{
			u32 start = 0, count = 0, stride = 0, end = 0;
		};
		bool cursor_catalog(u32 bank, u32 kind, cursor_section& out)
		{
			if (kind != 1 && kind != 2)
				return false;
			const u32 offset = kind == 1 ? mem::com_records_catalog : mem::com_samples_catalog;
			out.stride = kind == 1 ? mem::com_record_bytes : mem::com_sample_bytes;
			return cursor_word(bank + offset, out.start) && cursor_word(bank + offset + 4, out.count) &&
			       cursor_word(bank + offset + 8, out.end) &&
			       pbd::replay_resource::valid_catalog(
					   static_cast<pbd::replay_resource::section_kind>(kind), {out.start, out.count, out.end});
		}
		std::string describe(const reaction_cursor_ref& ref)
		{
			if (!ref.kind)
				return "none";
			if (ref.kind == 3)
				return fmt::format("bankOffset %s+0x%x style %u", replay_bank_offset::name(ref.base), ref.byte_offset,
					ref.bank_style);
			return fmt::format("%s[%u]+%u style %u", ref.kind == 1 ? "records80" : "samples16", ref.index, ref.byte_offset,
				ref.bank_style);
		}
		std::optional<u32> resolve_member(u32 side, const reaction_cursor_ref& saved)
		{
			u32 bank = 0;
			u16 style = 0;
			cursor_section section;
			if (saved.base != replay_bank_offset::base_kind::opposing || !read_opponent_com_bank(side, bank, style) ||
				!cursor_catalog(bank, saved.kind, section))
				return std::nullopt;
			return pbd::replay_resource::resolve({static_cast<pbd::replay_resource::section_kind>(saved.kind), saved.index,
													 saved.byte_offset, saved.bank_style},
				{section.start, section.count, section.end}, style);
		}
		bool resolve_cursor(u32 side, const reaction_cursor_ref& saved, u32& address)
		{
			address = 0;
			if (!saved.kind)
				return !saved.index && !saved.byte_offset && !saved.bank_style &&
				       saved.base == replay_bank_offset::base_kind::opposing;
			std::optional<u32> resolved;
			if (saved.kind != 3)
				resolved = resolve_member(side, saved);
			else if (!saved.index)
				resolved = replay_bank_offset::resolve({saved.base, saved.byte_offset, saved.bank_style},
					read_bank_bases(side), replay_bank_offset::cursor_alignment);
			if (!resolved)
				return false;
			be_t<u16> mapped{};
			address = *resolved;
			return vm::try_access(address, &mapped, sizeof(mapped), false);
		}
		// A member of the opponent's records or samples keeps that form; any other word is carried as a bank offset.
		bool identify_cursor(u32 side, reaction_cursor_ref& out)
		{
			out = {};
			u32 address = 0, bank = 0;
			u16 style = 0;
			if (!cursor_word(mem::actors + mem::actor_stride * side + mem::actor_reaction_cursor, address))
			{
				pbd_cursor_log.error("Reaction cursor unreadable (side %u)", side);
				return false;
			}
			if (!address)
				return true;
			// The initializer has not connected the actors yet. The opposing slot's verified bank
			// identifies the retained cursor independently of that temporary actor link.
			if (!read_opponent_com_bank(side, bank, style))
			{
				pbd_cursor_log.error("Opposing COM bank unreadable for cursor 0x%08x (side %u)", address, side);
				return false;
			}
			reaction_cursor_ref member;
			for (u32 kind = 1; kind <= 2; ++kind)
			{
				cursor_section section;
				if (!cursor_catalog(bank, kind, section))
				{
					pbd_cursor_log.error("Opposing COM catalog invalid (side %u, section %u, start 0x%08x, "
										 "count %u, end 0x%08x)",
						side, kind, section.start, section.count, section.end);
					return false;
				}
				if (address >= section.start && address < section.end)
					member = {kind, (address - section.start) / section.stride, (address - section.start) % section.stride,
						style};
			}
			const auto exact = [&](const reaction_cursor_ref& ref)
			{
				u32 resolved = 0;
				return ref.kind && resolve_cursor(side, ref, resolved) && resolved == address;
			};
			const auto bases = read_bank_bases(side);
			const auto offset = replay_bank_offset::identify(address, bases, replay_bank_offset::cursor_alignment);
			if (exact(member))
				out = member;
			else if (offset && exact({3, 0, offset->byte_offset, offset->bank_style, offset->base}))
				out = {3, 0, offset->byte_offset, offset->bank_style, offset->base};
			else
			{
				pbd_cursor_log.error("Retained cursor 0x%08x lies in no COM bank and not in the executable table (side %u, "
									 "opposing bank 0x%08x style %u, own bank 0x%08x style %u)",
					address, side, bases.opposing.base, bases.opposing.style, bases.own.base, bases.own.style);
				return false;
			}
			pbd_cursor_log.notice("Retained cursor 0x%08x (side %u) captured as %s", address, side, describe(out));
			return true;
		}
	} // namespace
	bool read_com_bank(u32 slot, u32& bank, u16& style)
	{
		if (slot > 1)
			return false;
		const u32 actor = mem::actors + mem::actor_stride * slot;
		be_t<u16> value{};
		u32 version = 0, magic = 0;
		if (!cursor_word(actor + mem::actor_com_bank, bank) || !bank || bank > mem::max_com_bank ||
			!vm::try_access(actor + mem::actor_style, &value, sizeof(value), false) || !cursor_word(bank, version) ||
			!cursor_word(bank + 4, magic) || version != mem::com_version || magic != mem::com_magic)
			return false;
		style = value;
		return true;
	}
	bool read_opponent_com_bank(u32 side, u32& bank, u16& style)
	{
		return side <= 1 && read_com_bank(1 - side, bank, style);
	}
	replay_bank_offset::banks read_bank_bases(u32 side)
	{
		replay_bank_offset::banks out;
		if (side > 1)
			return out;
		const auto read = [](u32 slot)
		{
			replay_bank_offset::bank bank;
			std::array<be_t<u32>, 2 * mem::com_clip_pair_count> pairs{};
			if (!read_com_bank(slot, bank.base, bank.style) ||
				!vm::try_access(bank.base + mem::com_clip_pairs, pairs.data(), sizeof(pairs), false))
				return replay_bank_offset::bank{};
			clip_pair_words words{};
			std::copy(pairs.begin(), pairs.end(), words.begin());
			bank.image_end = image_end(words);
			if (std::uint64_t{bank.image_end} < std::uint64_t{bank.base} + mem::com_payload_offset ||
				!replay_bank_offset::span(bank, {}))
				return replay_bank_offset::bank{};
			return bank;
		};
		out.opposing = read(1 - side);
		out.own = read(side);
		return out;
	}
	bool read_actor_reaction_cursor(ppu_thread& ppu, u64 generation, actor_reaction_cursor& out) noexcept
	{
		try
		{
			if (!reaction_cursor_context(ppu, generation))
				return false;
			actor_reaction_cursor sample;
			// Both sides are identified before refusing, so one game start names every cursor that cannot be carried.
			const bool left = identify_cursor(0, sample.ports[0]);
			const bool right = identify_cursor(1, sample.ports[1]);
			if (!left || !right)
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_reaction_cursor(ppu_thread& ppu, u64 generation, const actor_reaction_cursor& saved) noexcept
	{
		try
		{
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!reaction_cursor_context(ppu, generation))
				return false;
			std::array<u32, 2> resolved{};
			// Both current banks must resolve before writing. Fresh kind2 has not set actor+5c yet.
			for (u32 side = 0; side < 2; ++side)
				if (!resolve_cursor(side, saved.ports[side], resolved[side]))
				{
					pbd_cursor_log.error("Saved cursor %s does not resolve in the loaded banks (side %u)",
						describe(saved.ports[side]), side);
					return false;
				}
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 address = mem::actors + mem::actor_stride * side + mem::actor_reaction_cursor;
				be_t<u32> value = resolved[side];
				u32 observed = 0;
				if (!vm::try_access(address, &value, sizeof(value), true) || !cursor_word(address, observed) ||
					observed != resolved[side])
				{
					pbd_cursor_log.error("Reaction cursor 0x%08x did not hold (side %u, read 0x%08x)", resolved[side], side,
						observed);
					return false;
				}
			}
			pbd_cursor_log.notice("Reaction cursors written: 0x%08x (%s), 0x%08x (%s)", resolved[0],
				describe(saved.ports[0]), resolved[1], describe(saved.ports[1]));
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_carry
