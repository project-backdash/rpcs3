#include "stdafx.h"
#include "pbd_replay_carry.h"

LOG_CHANNEL(pbd_carry_log, "PBD_REPLAY_CARRY");

namespace pbd::replay_carry
{
	namespace
	{
		bool captured(bool readable, const char* field)
		{
			if (!readable)
				pbd_carry_log.error("Game-start field unreadable: %s", field);
			return readable;
		}
	} // namespace
	std::optional<game_values> capture_game_values(ppu_thread& owner, u64 generation) noexcept
	{
		vs_match_history history;
		previous_game_flow flow;
		actor_planar_terms planar;
		u8 flag = 0;
		actor_transform_sum transform;
		actor_descriptor_code descriptor;
		actor_descriptor_deltas deltas;
		actor_reaction_cursor cursor;
		actor_motion_step motion;
		actor_last_reaction_code reaction;
		actor_reaction_conditions conditions;
		actor_bone_segments bones;
		actor_reaction_result result;
		actor_hit_contacts contacts;
		actor_segment_deltas segment_deltas;
		actor_retained_scalars scalars;
		actor_records records;
		// The records are read under the same context as the actor fields above, from the executable's data block, which
		// is mapped from boot, so an unreadable block refuses the capture like them: a file without it replays a later
		// game only until its fighters diverge.
		if (!captured(read_vs_match_history(owner, generation, history), "vsMatchHistory") ||
			!captured(read_previous_game_flow(owner, generation, flow), "previousGameFlow") ||
			!captured(read_actor_planar_terms(owner, generation, planar), "actorPlanarTerms") ||
			!captured(read_descriptor_derived_flag(owner, generation, flag), "descriptorDerivedFlag") ||
			!captured(read_actor_transform_sum(owner, generation, transform), "actorTransformSum") ||
			!captured(read_actor_descriptor_code(owner, generation, descriptor), "actorDescriptorCode") ||
			!captured(read_actor_descriptor_deltas(owner, generation, deltas), "actorDescriptorDeltas") ||
			!captured(read_actor_reaction_cursor(owner, generation, cursor), "actorReactionCursor") ||
			!captured(read_actor_motion_step(owner, generation, motion), "actorMotionStep") ||
			!captured(read_actor_last_reaction_code(owner, generation, reaction), "actorLastReactionCode") ||
			!captured(read_actor_reaction_conditions(owner, generation, conditions), "actorReactionConditions") ||
			!captured(read_actor_bone_segments(owner, generation, bones), "actorBoneSegments") ||
			!captured(read_actor_reaction_result(owner, generation, result), "actorReactionResult") ||
			!captured(read_actor_hit_contacts(owner, generation, contacts), "actorHitContacts") ||
			!captured(read_actor_segment_deltas(owner, generation, segment_deltas), "actorSegmentDeltas") ||
			!captured(read_actor_retained_scalars(owner, generation, scalars), "actorRetainedScalars") ||
			!captured(read_actor_records(owner, generation, records), "actorRecords"))
			return {};
		return game_values{
			history, flow, planar, flag, transform, descriptor, deltas, cursor, motion, reaction, conditions, bones, result,
			contacts, segment_deltas, actor_scalar_bits{scalars.prior_life},
			actor_scalar_bits{scalars.reaction_scalar}, actor_scalar_bits{scalars.previous_blend_weight}, records};
	}
} // namespace pbd::replay_carry
