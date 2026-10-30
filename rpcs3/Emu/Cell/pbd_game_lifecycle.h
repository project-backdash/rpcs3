#pragma once

#include "util/types.hpp"
#include "pbd_practice_walls.h"
#include <array>
#include <optional>
#include <vector>

class ppu_thread;

namespace pbd::game_lifecycle
{
	// A placement token deliberately contains no battle state. A new match or selection screen invalidates its scope.
	struct scope
	{
		u64 emulation_generation = 0, stage_generation = 0, pair_generation = 0;
		u32 stage = ~0u;
		std::array<u32, 2> characters{~0u, ~0u};
		bool operator==(const scope&) const = default;
	};
	struct actor_placement
	{
		// IEEE-754 coordinate bits are stored unchanged so placement readback can compare the native replicas exactly.
		s32 x = 0, y = 0, z = 0;
		s16 facing = 0;
		u32 yaw_bits = 0;
	};
	struct placement_token
	{
		scope lifetime;
		std::array<u32, 2> actors{};
		std::array<actor_placement, 2> positions{};
	};
	enum class placement : u8
	{
		native_start,
		remembered,
		centre,
		left_wall,
		right_wall,
		foreground_wall,
		background_wall,
		random
	};
	// Restart Settings' changes to the pair a restart writes. switched: the fighters trade places, each taking the other's
	// spot and facing, so they still face each other. dummy_back: the dummy (fighter 1) looks straight away from P1 from its
	// spot. Both apply where the pair itself is written, inside the reset; the round-start placement writes no pair, so
	// they leave it alone.
	struct placement_variant
	{
		bool switched = false, dummy_back = false;
		bool operator==(const placement_variant&) const = default;
	};
	struct practice_reset
	{
		u64 request_id = 0;
		scope lifetime;
		placement target = placement::native_start;
		std::optional<placement_token> remembered;
		u32 random_seed = 0;
		placement_variant variant;
	};
	// Diagnostic only (practice_reset_sides): what DR's camera and screen-side rule read for both fighters. Raw words, ~0u
	// where unreadable. bone is bone 0's world X/Z (the camera tracker's input), position the canonical X/Z Practice writes,
	// projected the screen X 28AB40 writes at +6C8, right the byte 808D8 sets at +1C3, move the current move at +C4.
	struct sides_sample
	{
		struct fighter
		{
			u32 bone_x = ~0u, bone_z = ~0u, x = ~0u, z = ~0u, projected = ~0u, right = ~0u, move = ~0u;
		};
		std::array<fighter, 2> fighters{};
		// Camera mode, yaw and its frame counter (the first 60 frames keep the side nearest the previous yaw), eye and target.
		u32 camera_mode = ~0u, camera_yaw = ~0u, camera_frames = ~0u;
		std::array<u32, 3> eye{~0u, ~0u, ~0u}, target{~0u, ~0u, ~0u};
		// Practice's 180-degree flip inputs (byte 1008EBD2, word 1009BCA0).
		u32 flip_flag = ~0u, flip_mode = ~0u;
	};
	struct receipt
	{
		u64 request_id = 0, stage_generation = 0, pair_generation = 0;
		u32 native_kind = 0x14, counter_before = 0, counter_after = 0;
		// statistics_attempted: the statistics clear that precedes the reset was entered. attempted: the kind-20 reset itself
		// was entered, so a failure after it may have left the fight part-way through a reset.
		bool statistics_attempted = false, attempted = false, completed = false, preserved = false, readback = false;
		bool placement_phase = false, neutral_verified = false;
		const char* reason = "unavailable";
		// Exact placement read at the guarded phase, followed by the actual sample after native animation/controller work.
		std::array<actor_placement, 2> applied_positions{};
		std::array<actor_placement, 2> positions{};
		scope observed_lifetime;
		// Wall restarts: the rough pair was written before the pose pass, and what the game read at the wall phase, just
		// before the exact pair was written (empty when that phase was not reached).
		bool provisional_placement = false;
		std::optional<sides_sample> phase_sides;
	};
	bool requested() noexcept;
	// Queued callers wait for this owner boundary before consuming a request; execute still checks it independently.
	bool boundary_ready(const ppu_thread& ppu) noexcept;
	scope observe_scope(u64 stage_generation, u64 pair_generation) noexcept;
	bool placement_available(placement target, const scope& lifetime, const std::optional<placement_token>& remembered) noexcept;
	// Practice's reading of the current stage's collision for wall restarts, for its once-per-stage diagnostic.
	struct wall_report
	{
		// read: the whole collection was read. reason: why it was not, or why no wall reading applies.
		bool read = false;
		const char* reason = "";
		const char* path = "none";
		u32 objects = 0;
		u64 digest = 0;
		pbd::practice::walls::wall_counts counts;
		// Left, Right, Foreground and Background Wall, tested exactly as the Restart Position menu tests them.
		std::array<bool, 4> available{};
		std::vector<pbd::practice::walls::polygon> raw;
	};
	// Reads the collection on the owner thread, like placement_available. Costs one full collection read per call.
	wall_report survey_walls(const scope& lifetime) noexcept;
	std::optional<placement_token> capture_placement(ppu_thread& ppu, const scope& lifetime) noexcept;
	receipt execute(ppu_thread& ppu, const practice_reset& command) noexcept;
	// Called only after both native actor initializers, before the remaining camera and controller reconstruction.
	void placement_phase(ppu_thread& ppu, u32 address) noexcept;
	// Reads a sides_sample in the Practice actor layout; read-only, on the owner thread at a boundary or inside a reset.
	sides_sample read_sides() noexcept;
} // namespace pbd::game_lifecycle
