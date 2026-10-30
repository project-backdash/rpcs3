#pragma once

#include "util/types.hpp"
#include "pbd_replay_actor_records.h"
#include "pbd_replay_carry_types.h"

class ppu_thread;

namespace pbd::replay_carry
{
	// Three raw words, transferred in axis order with big-endian conversion and no floating-point arithmetic.
	bool vector_access(u32 address, std::array<std::uint32_t, 3>& bits, bool write);
	// Native signed planar terms serialized as unsigned bit patterns by player port.
	bool read_actor_planar_terms(ppu_thread& owner, u64 generation, actor_planar_terms& out) noexcept;
	bool apply_actor_planar_terms(ppu_thread& owner, u64 generation, const actor_planar_terms& saved) noexcept;
	// Exact float sum produced by the native actor transform path, by player port.
	bool read_actor_transform_sum(ppu_thread& owner, u64 generation, actor_transform_sum& out) noexcept;
	bool apply_actor_transform_sum(ppu_thread& owner, u64 generation, const actor_transform_sum& saved) noexcept;
	bool read_actor_descriptor_code(ppu_thread& owner, u64 generation, actor_descriptor_code& out) noexcept;
	bool apply_actor_descriptor_code(ppu_thread& owner, u64 generation, const actor_descriptor_code& saved) noexcept;
	bool read_actor_descriptor_deltas(ppu_thread& owner, u64 generation, actor_descriptor_deltas& out) noexcept;
	bool apply_actor_descriptor_deltas(
		ppu_thread& owner, u64 generation, const actor_descriptor_deltas& saved) noexcept;
	// COM bank reference; no guest address is serialized. 0=none, 1=records80, 2=samples16, 3=bankOffset.
	bool read_actor_reaction_cursor(ppu_thread& owner, u64 generation, actor_reaction_cursor& out) noexcept;
	bool apply_actor_reaction_cursor(ppu_thread& owner, u64 generation, const actor_reaction_cursor& saved) noexcept;
	// Exact float X/Z motion steps stored by the native movement paths.
	bool read_actor_motion_step(ppu_thread& owner, u64 generation, actor_motion_step& out) noexcept;
	bool apply_actor_motion_step(ppu_thread& owner, u64 generation, const actor_motion_step& saved) noexcept;
	bool read_actor_last_reaction_code(ppu_thread& owner, u64 generation, actor_last_reaction_code& out) noexcept;
	bool apply_actor_last_reaction_code(
		ppu_thread& owner, u64 generation, const actor_last_reaction_code& saved) noexcept;
	bool read_actor_reaction_conditions(ppu_thread& owner, u64 generation, actor_reaction_conditions& out) noexcept;
	bool apply_actor_reaction_conditions(
		ppu_thread& owner, u64 generation, const actor_reaction_conditions& saved) noexcept;
	bool read_actor_bone_segments(ppu_thread& owner, u64 generation, actor_bone_segments& out) noexcept;
	bool apply_actor_bone_segments(ppu_thread& owner, u64 generation, const actor_bone_segments& saved) noexcept;
	bool read_actor_reaction_result(ppu_thread& owner, u64 generation, actor_reaction_result& out) noexcept;
	bool apply_actor_reaction_result(ppu_thread& owner, u64 generation, const actor_reaction_result& saved) noexcept;
	// The verified COM bank (version and magic checked) of actor slot 0 or 1 and the style loaded into it.
	bool read_com_bank(u32 slot, u32& bank, u16& style);
	bool read_opponent_com_bank(u32 side, u32& bank, u16& style);
	// Both slots' banks as seen from one side; an unreadable slot has base 0.
	replay_bank_offset::banks read_bank_bases(u32 side);
	std::optional<game_values> capture_game_values(ppu_thread& owner, u64 generation) noexcept;
	bool read_descriptor_derived_flag(ppu_thread& owner, u64 generation, u8& out) noexcept;
	bool apply_descriptor_derived_flag(ppu_thread& owner, u64 generation, u8 saved) noexcept;
	bool refresh_descriptor_flag(ppu_thread& owner, u64 generation, descriptor_flag& out) noexcept;
	bool read_vs_match_history(ppu_thread& owner, u64 generation, vs_match_history& out) noexcept;
	bool apply_vs_match_history(ppu_thread& owner, u64 generation, const vs_match_history& saved) noexcept;
	bool read_previous_game_flow(ppu_thread& owner, u64 generation, previous_game_flow& out) noexcept;
	// The VS flow object's saved words, each only when the file has it: the match history and the previous game's flow.
	bool apply_vs_flow(ppu_thread& owner, u64 generation, const game_values& saved) noexcept;
	bool owner_ready(ppu_thread& ppu, u64 generation) noexcept;
	bool read_actor_hit_contacts(ppu_thread& owner, u64 generation, actor_hit_contacts& out) noexcept;
	bool apply_actor_hit_contacts(ppu_thread& owner, u64 generation, const actor_hit_contacts& saved) noexcept;
	bool read_actor_segment_deltas(ppu_thread& owner, u64 generation, actor_segment_deltas& out) noexcept;
	bool apply_actor_segment_deltas(ppu_thread& owner, u64 generation, const actor_segment_deltas& saved) noexcept;
	bool read_actor_retained_scalars(ppu_thread& owner, u64 generation, actor_retained_scalars& out) noexcept;
	bool apply_actor_retained_scalars(ppu_thread& owner, u64 generation, const game_values& saved) noexcept;
	bool read_actor_records(ppu_thread& owner, u64 generation, actor_records& out) noexcept;
	// Writes the records over both actors at the match initializer as plan_actor_records places them, checked against
	// the COM images and bone-root objects this boot loaded; `changed` counts the bytes that differed from what the
	// actors held.
	records_refusal apply_actor_records(ppu_thread& owner, u64 generation, const actor_records& saved, u32& changed) noexcept;
	// The same placement/readback policy, at an authenticated replay round boundary with loaded resources.
	records_refusal pbd_apply_round_actor_records(ppu_thread& owner, u64 generation, const actor_records& saved, u32& changed) noexcept;
} // namespace pbd::replay_carry
