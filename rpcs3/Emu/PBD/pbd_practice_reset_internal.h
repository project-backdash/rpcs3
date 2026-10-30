#pragma once

#include "pbd_native_call.h"

namespace pbd::game_lifecycle::detail
{
	constexpr u32 reset_entry = 0x159f0, reset_phase = 0x15f5c, wall_phase = 0x16018;
	extern thread_local const practice_reset* active_reset;
	extern thread_local bool phase_seen, phase_ok;
	extern thread_local const char* placement_failure;
	// Wall restarts only: the rough pair written at reset_phase, before the pose pass (see placement_phase).
	extern thread_local bool provisional_seen, provisional_ok;
	extern thread_local std::array<actor_placement, 2> phase_positions;
	extern thread_local std::optional<sides_sample> phase_sides;
	struct collision_view
	{
		u32 source = 0, query = 0, objects = 0, count = 0;
		u64 digest = 0;
		pbd::practice::walls::boundary geometry;
		bool floor_only = false;
	};
	extern thread_local const collision_view* reset_collision;

	// The clearance the Restart Position menu tests a wall position with, and the separation of every wall restart.
	constexpr float wall_menu_clearance = 750.f, wall_pair_separation = 2000.f;
	struct reset_scope
	{
		explicit reset_scope(const practice_reset& command, const collision_view* collision = nullptr)
		{
			active_reset = &command;
			reset_collision = collision;
			phase_seen = false;
			placement_failure = nullptr;
			phase_ok = false;
			phase_positions = {};
			provisional_seen = false;
			provisional_ok = false;
			phase_sides.reset();
		}
		~reset_scope()
		{
			active_reset = nullptr;
			reset_collision = nullptr;
		}
	};
	bool same_scope(const scope& lifetime) noexcept;
	bool coordinate_valid(u32 bits) noexcept;
	bool read_position(u32 actor, actor_placement& out) noexcept;
	bool placement_replicas_match(u32 actor, const actor_placement& position) noexcept;
	std::array<u32, 2> actors() noexcept;
	bool token_matches(const placement_token& token, const scope& lifetime) noexcept;
	bool centre_basis_matches() noexcept;
	bool wall_target(placement target) noexcept;
	// Reads the stage's imported collision collection. A report receives refusal details or the polygons and wall reading.
	std::optional<collision_view> read_collision(const scope& lifetime, wall_report* report, bool admit_floor = false);
	bool same_collision(const collision_view& a, const collision_view& b) noexcept;
	pbd::practice::walls::position wall_position(placement target) noexcept;
	std::optional<placement_token> wall_destination(const collision_view& arena, const practice_reset& command);
	std::optional<placement_token> rough_wall_pair(const collision_view& arena, const practice_reset& command);
	bool wall_choice_available(placement target, const scope& lifetime);
	bool at_phase(const ppu_thread& ppu, u32 phase) noexcept;
	bool own_tables_qualified();
	bool bind_own_tables(ppu_thread& ppu);
	bool write_pair(const placement_token& destination, std::array<actor_placement, 2>& written) noexcept;
} // namespace pbd::game_lifecycle::detail
