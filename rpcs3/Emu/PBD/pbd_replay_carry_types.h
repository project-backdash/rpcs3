#pragma once

#include "pbd_replay_bank_offset.h"
#include "pbd_t5dr_memory.h"
#include <array>
#include <cstdint>
#include <optional>

namespace pbd::replay_carry
{
	struct vs_match_history
	{
		std::uint32_t completed_games = 0;
		std::array<std::uint16_t, 2> wins{};
	};
	// pbd_t5dr_memory.h previous_game_flow, in that order.
	struct previous_game_flow
	{
		std::array<std::uint32_t, 5> words{};
	};
	struct actor_planar_terms
	{
		std::array<std::array<std::uint32_t, 2>, 2> bits{};
	};
	struct actor_transform_sum
	{
		std::array<std::uint32_t, 2> bits{};
	};
	struct actor_descriptor_code
	{
		std::array<std::uint16_t, 2> codes{};
	};
	struct actor_descriptor_deltas
	{
		std::array<std::array<std::uint16_t, 2>, 2> bits{};
	};
	// 0=none, 1=records80 and 2=samples16 name a member of the opponent's COM bank; 3=bankOffset is a byte offset from
	// the base it names (see pbd_replay_bank_offset.h) and leaves index 0.
	struct reaction_cursor_ref
	{
		std::uint32_t kind = 0, index = 0, byte_offset = 0;
		std::uint16_t bank_style = 0;
		replay_bank_offset::base_kind base = replay_bank_offset::base_kind::opposing;
	};
	struct actor_reaction_cursor
	{
		std::array<reaction_cursor_ref, 2> ports{};
	};
	struct actor_motion_step
	{
		std::array<std::array<std::uint32_t, 2>, 2> bits{};
	};
	struct actor_last_reaction_code
	{
		std::array<std::uint16_t, 2> codes{};
	};
	struct actor_reaction_conditions
	{
		std::array<std::uint8_t, 2> command_11e{};
		std::array<std::uint8_t, 2> command_22{};
	};
	struct bone_segment
	{
		std::array<std::uint32_t, 3> start_bits{}, end_bits{};
		bool operator==(const bone_segment&) const = default;
	};
	struct actor_bone_segments
	{
		std::array<std::array<bone_segment, 4>, 2> ports{};
	};
	// Kinds 1/2 index loaded COM members; kind 3 is a byte offset in the relocated payload prefix; kind 4 is a byte
	// offset from the base it names (see pbd_replay_bank_offset.h). Kinds 3 and 4 keep the offset in index.
	struct reaction_result_ref
	{
		std::uint32_t kind = 0, index = 0;
		std::uint16_t bank_style = 0;
		replay_bank_offset::base_kind base = replay_bank_offset::base_kind::opposing;
	};
	struct actor_reaction_result
	{
		std::array<reaction_result_ref, 2> ports{};
	};
	struct hit_contact
	{
		std::array<std::uint32_t, 3> end_bits{}, delta_bits{};
		std::uint16_t sphere_index = 0;
		bool operator==(const hit_contact&) const = default;
	};
	struct actor_hit_contacts
	{
		std::array<std::array<hit_contact, 2>, 2> ports{};
	};
	struct segment_delta
	{
		std::array<std::uint32_t, 3> own_bits{}, copied_bits{};
		bool operator==(const segment_delta&) const = default;
	};
	struct actor_segment_deltas
	{
		std::array<segment_delta, 2> ports{};
	};
	struct actor_scalar_bits
	{
		std::array<std::uint32_t, 2> bits{};
	};
	struct actor_retained_scalars
	{
		std::array<std::uint32_t, 2> prior_life{}, reaction_scalar{}, previous_blend_weight{};
	};
	struct descriptor_flag
	{
		std::uint32_t descriptor = 0;
		std::uint8_t before = 0, value = 0;
	};
	// Both fighters' actors at the match initializer, byte for byte (pbd_t5dr_memory.h actors). A game after the first
	// starts on what the one before left in them, which a fresh boot holds as zeros and the initializer keeps.
	struct actor_records
	{
		std::array<std::uint8_t, replay_memory::actor_records_bytes> bytes{};
		bool operator==(const actor_records&) const = default;
	};
	// Missing fields in older files stay absent; playback must not invent their values.
	struct game_values
	{
		std::optional<vs_match_history> history;
		std::optional<previous_game_flow> flow;
		std::optional<actor_planar_terms> planar;
		std::optional<std::uint8_t> descriptor_derived_flag;
		std::optional<actor_transform_sum> transform_sum;
		std::optional<actor_descriptor_code> descriptor_code;
		std::optional<actor_descriptor_deltas> descriptor_deltas;
		std::optional<actor_reaction_cursor> reaction_cursor;
		std::optional<actor_motion_step> motion_step;
		std::optional<actor_last_reaction_code> last_reaction_code;
		std::optional<actor_reaction_conditions> reaction_conditions;
		std::optional<actor_bone_segments> bone_segments;
		std::optional<actor_reaction_result> reaction_result;
		std::optional<actor_hit_contacts> hit_contacts;
		std::optional<actor_segment_deltas> segment_deltas;
		std::optional<actor_scalar_bits> prior_life, reaction_scalar, previous_blend_weight;
		std::optional<actor_records> records;
	};
} // namespace pbd::replay_carry
