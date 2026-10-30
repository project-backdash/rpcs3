#pragma once

#include "pbd_replay_carry_types.h"
#include "pbd_replay_resource_reference.h"
#include "pbd_t5dr_memory.h"

namespace pbd::replay_result
{
	struct member_spec
	{
		std::uint32_t catalog_offset = 0, stride = 0, word_offset = 0;
	};
	constexpr member_spec member(std::uint32_t kind) noexcept
	{
		namespace mem = pbd::replay_memory;
		if (kind == 1)
			return {mem::com_groups_catalog, mem::com_group_bytes, mem::com_group_result_offset};
		if (kind == 2)
			return {mem::com_records_catalog, mem::com_record_bytes, mem::com_record_result_offset};
		return {};
	}
	constexpr bool valid_catalog(std::uint32_t kind, replay_resource::catalog table) noexcept
	{
		const auto spec = member(kind);
		return spec.stride && table.start && !(table.start & 3) && table.count && table.count <= 0x100000 &&
		       std::uint64_t{table.start} + std::uint64_t{table.count} * spec.stride == table.next;
	}
	constexpr std::optional<std::uint32_t> resolve_word(
		replay_carry::reaction_result_ref saved, replay_resource::catalog table, std::uint16_t loaded_style) noexcept
	{
		if (!saved.kind)
			return !saved.index && !saved.bank_style ? std::optional<std::uint32_t>{0} : std::nullopt;
		if (!valid_catalog(saved.kind, table) || saved.index >= table.count || saved.bank_style != loaded_style ||
			saved.base != replay_bank_offset::base_kind::opposing)
			return {};
		const auto spec = member(saved.kind);
		const auto address = std::uint64_t{table.start} + std::uint64_t{saved.index} * spec.stride + spec.word_offset;
		return address + 4 <= table.next && address <= 0xfffffffc ?
		           std::optional<std::uint32_t>{static_cast<std::uint32_t>(address)} :
		           std::nullopt;
	}
	constexpr std::optional<std::uint32_t> resolve_prefix(replay_carry::reaction_result_ref saved, std::uint32_t bank,
		replay_resource::catalog records, std::uint16_t loaded_style) noexcept
	{
		const auto base = std::uint64_t{bank} + pbd::replay_memory::com_payload_offset;
		const auto address = base + saved.index;
		if (saved.kind != 3 || saved.bank_style != loaded_style || !bank || (bank & 3) || (saved.index & 3) ||
			saved.base != replay_bank_offset::base_kind::opposing || !valid_catalog(2, records) || base > records.start ||
			address + 4 > records.start)
			return {};
		return static_cast<std::uint32_t>(address);
	}
	// Kind 4 keeps a word that no loaded member or prefix offset names, as a byte offset from a bank or the executable.
	constexpr std::optional<std::uint32_t> resolve_bank_offset(
		replay_carry::reaction_result_ref saved, replay_bank_offset::banks loaded) noexcept
	{
		if (saved.kind != 4)
			return {};
		return replay_bank_offset::resolve(
			{saved.base, saved.index, saved.bank_style}, loaded, replay_bank_offset::result_alignment);
	}
} // namespace pbd::replay_result
