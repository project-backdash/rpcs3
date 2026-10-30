#include "stdafx.h"
#include "pbd_practice_reset_internal.h"

LOG_CHANNEL(pbd_reset_log, "PBD restart");

namespace pbd::game_lifecycle
{
	using namespace detail;
	namespace detail
	{
		thread_local const practice_reset* active_reset = nullptr;
		thread_local bool phase_seen = false, phase_ok = false;
		thread_local const char* placement_failure = nullptr;
		thread_local bool provisional_seen = false, provisional_ok = false;
		thread_local std::array<actor_placement, 2> phase_positions{};
		thread_local std::optional<sides_sample> phase_sides;
		thread_local const collision_view* reset_collision = nullptr;
		bool same_scope(const scope& lifetime) noexcept
		{
			return lifetime.emulation_generation && lifetime.stage_generation && lifetime.pair_generation &&
			       lifetime.stage != ~0u && lifetime.characters[0] != ~0u && lifetime.characters[1] != ~0u &&
			       lifetime == observe_scope(lifetime.stage_generation, lifetime.pair_generation);
		}
		bool coordinate_valid(u32 bits) noexcept
		{
			// 1AFEC places actors with lfs/stfs. Keep the exact float bits in the token, but validate their numeric value.
			const float coordinate = std::bit_cast<float>(bits);
			return std::isfinite(coordinate) && std::abs(coordinate) <= 7000000.f;
		}
		bool read_position(u32 actor, actor_placement& out) noexcept
		{
			using pbd::practice::native::word;
			// DR's recovery path 2EFA4 uses this world-position pair. The base and previous-frame copies include animation
			// displacement during ordinary play, so requiring all three to agree would reject a valid Remember or reset.
			const u32 x = word(actor + 0x760), y = word(actor + 4), z = word(actor + 0x768), yaw = word(actor + 0x74);
			be_t<s16> facing{};
			if (!vm::try_access(actor + 0x7a, &facing, sizeof(facing), false) ||
				!std::isfinite(std::bit_cast<float>(yaw)))
				return false;
			// The recovery radius bounds corrupt values; it does not describe a stage's wall geometry.
			if (!coordinate_valid(x) || !coordinate_valid(y) || !coordinate_valid(z))
				return false;
			out = {static_cast<s32>(x), static_cast<s32>(y), static_cast<s32>(z), s16{facing}, yaw};
			return true;
		}
		bool placement_replicas_match(u32 actor, const actor_placement& position) noexcept
		{
			using pbd::practice::native::word;
			return word(actor) == static_cast<u32>(position.x) && word(actor + 0x14) == static_cast<u32>(position.x) &&
			       word(actor + 8) == static_cast<u32>(position.z) && word(actor + 0x1c) == static_cast<u32>(position.z) &&
			       word(actor + 0x20) == position.yaw_bits &&
			       pbd::practice::native::halfword(actor + 0xe) == static_cast<u16>(position.facing);
		}
		std::array<u32, 2> actors() noexcept
		{
			const auto owner = pbd::practice::native::inspect(false);
			return owner.practice ? std::array<u32, 2>{0x10090040, 0x10090910} : std::array<u32, 2>{};
		}
		bool token_matches(const placement_token& token, const scope& lifetime) noexcept
		{
			if (token.lifetime != lifetime || token.actors != actors())
				return false;
			for (const auto& position : token.positions)
				if (!std::isfinite(std::bit_cast<float>(position.yaw_bits)) ||
					!coordinate_valid(static_cast<u32>(position.x)) || !coordinate_valid(static_cast<u32>(position.y)) ||
					!coordinate_valid(static_cast<u32>(position.z)))
					return false;
			return true;
		}
		bool centre_basis_matches() noexcept
		{
			// 1AFEC's origin fallback places the pair at -1000/+1000 on X, with Z zero. Pin both that native branch and its
			// constants; the normal stage-spawn branch can cycle positions and is a separate placement choice.
			using pbd::practice::native::word;
			return word(0x468774) == 0xc47a0000 && word(0x46877c) == 0x447a0000 && word(0x46876c) == 0 &&
			       word(0x468784) == 0x40490fdb &&
			       pbd::practice::native::body_matches(0x1b3a4, 0x178, "13a1f2f9a06fd0bf5b46db2df855e897517f5331ee0f63d83e6ef73e1b69edd7");
		}
		bool wall_target(placement target) noexcept
		{
			return target >= placement::left_wall && target <= placement::background_wall;
		}
		// The guarded reset phases: reached by the owner inside this kind-20 reset, straight after the call each one follows
		// (actor initialization for reset_phase, the 14C4C(1) pose pass for wall_phase), with the native code still in place.
		bool at_phase(const ppu_thread& ppu, u32 phase) noexcept
		{
			const bool wall = phase == wall_phase;
			return pbd::practice::native::in_native_call() && ppu.id == pbd::t5dr::main_ppu_id && ppu.gpr[2] == pbd::t5dr::toc &&
			       ppu.lr == phase && ppu.gpr[26] == 0x14 && same_scope(active_reset->lifetime) &&
			       pbd::practice::native::word(phase) == (wall ? 0x381affed : 0x480d32e5) &&
			       pbd::practice::native::word(phase - 4) == (wall ? 0x4bffec39 : 0x4bffe669);
		}
		// DR's own Practice restart (EC4B8..EC514) binds each fighter to its own move table before it runs the same kind-20 reset:
		// 2A92C reads the table loaded for the fighter's port (0x100529A4 + 0x2C + 4 * port), and 13CC8 writes it to +0x50, +0x54
		// and +0xBC and copies its animation banks (+0x248..+0x26C) to +0x83C..+0x890 (13C34). The kind-20 actor reset (145C0)
		// rebinds +0x54, +0x58 and +0xBC from +0x50 but never +0x50 itself or the banks, so a restart that skips this step starts a
		// fighter on whatever table and banks it held when the chord was pressed. Restart therefore does what DR's restart does:
		// it begins from the bindings DR itself restarts from, whatever either fighter was doing (a throw, a knockdown, a style
		// change). For a fighter already on its own table every write stores the value already there.
		constexpr u32 own_table_entry = 0x2a92c, own_table_bind = 0x13cc8, own_table_banks = 0x13c34;
		bool own_tables_qualified()
		{
			// One digest covers 13C34 and the 13CC8 that follows it and tail-branches into it (0x94 + 0x64 bytes).
			return pbd::practice::native::word(pbd::t5dr::toc - 0x7bdc) == 0x100529a4 &&
			       pbd::practice::native::body_matches(own_table_entry, 0x20, "f6341edf08d10f03aeb59e5ecf87279c57c74de12bbc13c570429332b3bd2fe0") &&
			       pbd::practice::native::body_matches(own_table_banks, 0xf8, "667a21a289ba8a43fcf863852830a9e49f370519cfafaf4b732805ff87e05c65");
		}
		bool bind_own_tables(ppu_thread& ppu)
		{
			for (const u32 actor : {0x10090040u, 0x10090910u})
			{
				u32 table = 0;
				std::array<u8, 0x270> header{};
				if (!pbd::practice::native::preserved_call(ppu, own_table_entry, actor, 0, 0, nullptr, &table) ||
					!table || (table & 3) || !vm::try_access(table, header.data(), header.size(), false) ||
					!pbd::practice::native::preserved_call(ppu, own_table_bind, actor, table) ||
					pbd::practice::native::word(actor + 0x50) != table || pbd::practice::native::word(actor + 0x54) != table ||
					pbd::practice::native::word(actor + 0xbc) != table)
					return false;
			}
			return true;
		}
		// Writes a pair into every native placement replica and reads each fighter back exactly; written receives what was
		// observed. Health, inputs, timers, RNG and animation state stay those of the full reset: the token holds placement only.
		bool write_pair(const placement_token& destination, std::array<actor_placement, 2>& written) noexcept
		{
			if (destination.actors != std::array<u32, 2>{0x10090040, 0x10090910})
				return false;
			constexpr std::array<u32, 10> offsets{0, 4, 8, 0x14, 0x1c, 0x20, 0x74, 0x760, 0x768, 0xe};
			for (const u32 actor : destination.actors)
			{
				for (const u32 offset : offsets)
					if (!vm::check_addr(actor + offset, vm::page_readable | vm::page_writable, offset == 0xe ? 2 : 4))
						return false;
				if (!vm::check_addr(actor + 0x7a, vm::page_readable | vm::page_writable, 2))
					return false;
			}
			for (u32 side = 0; side < 2; ++side)
			{
				const auto& position = destination.positions[side];
				const u32 actor = destination.actors[side];
				for (const u32 offset : {0u, 0x14u, 0x760u})
					vm::write32(actor + offset, static_cast<u32>(position.x));
				for (const u32 offset : {8u, 0x1cu, 0x768u})
					vm::write32(actor + offset, static_cast<u32>(position.z));
				vm::write32(actor + 4, 0);
				vm::write16(actor + 0xe, static_cast<u16>(position.facing));
				vm::write16(actor + 0x7a, static_cast<u16>(position.facing));
				vm::write32(actor + 0x20, position.yaw_bits);
				vm::write32(actor + 0x74, position.yaw_bits);
				actor_placement observed;
				if (!read_position(actor, observed) || !placement_replicas_match(actor, observed) ||
					observed.x != position.x || observed.y != 0 || observed.z != position.z ||
					observed.facing != position.facing || observed.yaw_bits != position.yaw_bits)
					return false;
				written[side] = observed;
			}
			return true;
		}
		bool admit_reset(ppu_thread& ppu, const practice_reset& command, receipt& out, pbd::practice::native::owner_view& owner)
		{
			if (!command.request_id)
			{
				out.reason = "missing_request";
				return false;
			}
			if (!boundary_ready(ppu))
			{
				out.reason = "native_boundary_unavailable";
				return false;
			}
			if (!same_scope(command.lifetime))
			{
				out.reason = "stale_scope";
				return false;
			}
			owner = pbd::practice::native::inspect(false);
			if (!owner.practice || owner.pause_acknowledged)
			{
				out.reason = "native_pause_owned";
				return false;
			}
			if (command.target != placement::native_start && command.target != placement::remembered &&
				command.target != placement::centre && command.target != placement::random && !wall_target(command.target))
			{
				out.reason = "placement_unqualified";
				return false;
			}
			return true;
		}

		bool resolve_reset(const practice_reset& command, practice_reset& resolved, std::optional<collision_view>& collision, receipt& out)
		{
			if (wall_target(command.target) || command.target == placement::random)
			{
				collision = read_collision(command.lifetime, nullptr, command.target == placement::random);
				if (!collision)
				{
					out.reason = "wall_geometry_unavailable";
					return false;
				}
				if (command.target == placement::random)
				{
					if (!centre_basis_matches())
					{
						out.reason = "centre_basis_unavailable";
						return false;
					}
					resolved.remembered = pbd::practice::random_destination(collision->geometry, command, actors());
					if (!resolved.remembered)
					{
						out.reason = "random_position_unavailable";
						return false;
					}
				}
				else if (!pbd::practice::walls::place(collision->geometry, wall_position(command.target),
							 wall_menu_clearance, wall_pair_separation))
				{
					out.reason = "wall_geometry_unavailable";
					return false;
				}
			}
			if (command.target == placement::centre && !centre_basis_matches())
			{
				out.reason = "centre_basis_unavailable";
				return false;
			}
			if (command.target == placement::remembered && (!command.remembered || !token_matches(*command.remembered, command.lifetime)))
			{
				out.reason = "stale_placement";
				return false;
			}
			return true;
		}

		bool qualify_reset_body(const practice_reset& command, const pbd::practice::native::owner_view& owner, receipt& out)
		{
			// The native initializer 2D574 sets pose one. Admit that baseline and pose zero without treating either as neutral proof.
			// Other configured poses are not accepted as a fresh-neutral Restart.
			if (pbd::practice::native::word(owner.practice + 0x74) > 1 ||
				pbd::practice::native::word(owner.practice + 0x3ec) != 0 || pbd::practice::native::word(owner.practice + 0x3f0) != 0)
			{
				out.reason = "pose_or_demo_active";
				return false;
			}
			if (!pbd::practice::native::body_matches(reset_entry, 0x1054, "b45802ad8d90b3bc1584caf64ee76b678b3ec86468682391cb0d059c61e1e2ad") ||
				!pbd::practice::native::body_matches(0x2d844, 0x3c, "76db74fc8b90658d3d0952c0620dec0d1bad13b284ef6c3461712400778c6494") ||
				pbd::practice::native::word(reset_phase) != 0x480d32e5 ||
				pbd::practice::native::word(reset_phase - 4) != 0x4bffe669 ||
				!own_tables_qualified())
			{
				out.reason = "native_body_mismatch";
				return false;
			}
			if (wall_target(command.target) &&
				(pbd::practice::native::word(wall_phase) != 0x381affed ||
					pbd::practice::native::word(wall_phase - 4) != 0x4bffec39 ||
					!pbd::practice::native::body_matches(0x14c4c, 0x274, "830bf5635cd36f5cbbd5ede5f52524c9d84b7778a21320f9993a28fe4ff39b57") ||
					!pbd::practice::native::body_matches(0x28c0f0, 0x1be4, "55f97cddef70356a92ddfb7639684f93566d0724872a28c8e62896f32e4f8f0a") ||
					!pbd::practice::native::body_matches(0x28de50, 0x278, "c00f1d5358b36d5a7d8fcf965ab522e1eeb7c26561d95f125bf0babb069c1db8") ||
					!pbd::practice::native::body_matches(0x28dcd4, 0xc0, "91512d9d1dcbeb2fc48e2f372b3974103158791aa752ba045b74f01d6c9e9b83")))
			{
				out.reason = "native_pose_body_mismatch";
				return false;
			}
			return true;
		}

		void dispatch_reset(ppu_thread& ppu, const practice_reset& command, const pbd::practice::native::owner_view& owner, const practice_reset& resolved, const std::optional<collision_view>& collision, receipt& out)
		{
			constexpr std::array<u32, 8> option_offsets{0x6c, 0x70, 0x30, 0x3c, 0x14, 0x18, 0x74, 0x64};
			std::array<u32, 8> options{};
			for (u32 i = 0; i < options.size(); ++i)
				options[i] = pbd::practice::native::word(owner.practice + option_offsets[i]);
			out.counter_before = pbd::practice::native::word(0x1008f05c);
			reset_scope claim(resolved, collision ? &*collision : nullptr);
			// Native demonstration retirement clears Practice's accumulated damage/command statistics before the same full
			// kind-20 reset. Use that leaf instead of carrying a prior combo across a position restart or patching its fields.
			// The leaf only zeroes those counters, and preserved_call restores the registers and scratch stack either way, so
			// a refused clear leaves the fight as it was: the reset is skipped and the caller may let the player try again.
			out.statistics_attempted = true;
			out.preserved = pbd::practice::native::preserved_call(ppu, 0x2d844, 0, 0);
			if (!out.preserved)
			{
				out.reason = "native_statistics_reset_failed";
				return;
			}
			// From here a failure can leave the full reset part-way through, so it is reported as attempted.
			out.attempted = true;
			if (!bind_own_tables(ppu))
			{
				out.preserved = false;
				out.reason = "native_own_tables_failed";
				return;
			}
			out.preserved = pbd::practice::native::preserved_call(ppu, reset_entry, 0x14, 0);
			out.placement_phase = phase_seen && phase_ok;
			out.applied_positions = phase_positions;
			out.provisional_placement = provisional_ok;
			out.phase_sides = phase_sides;
			out.readback = out.preserved && out.placement_phase && same_scope(command.lifetime) && actors() == std::array<u32, 2>{0x10090040, 0x10090910};
			for (u32 i = 0; i < options.size(); ++i)
				out.readback &= pbd::practice::native::word(owner.practice + option_offsets[i]) == options[i];
			// The exact destination was read back at the placement phase. Native animation/controller initialization can
			// subsequently displace its canonical samples; keep those real samples instead of inventing motionless actors.
			for (u32 side = 0; side < 2; ++side)
				out.readback &= read_position(side ? 0x10090910 : 0x10090040, out.positions[side]);
			out.counter_after = pbd::practice::native::word(0x1008f05c);
			out.completed = out.preserved && out.readback;
			// Completion covers this native operation and placement only, not a fresh neutral state of the fighters.
			out.reason = out.completed ? "native_complete_neutral_unqualified" : placement_failure ? placement_failure :
			                                                                                         "native_reset_failed";
			if (placement_failure)
				pbd_reset_log.warning("Restart %u placement: %s (completed %s).", command.request_id, placement_failure, out.completed ? "true" : "false");
		}

	} // namespace detail

	bool requested() noexcept
	{
		const char* value = pbd::env::find("PBD_PRACTICE_RESET");
		return value && std::strcmp(value, "1") == 0;
	}
	bool boundary_ready(const ppu_thread& ppu) noexcept
	{
		return requested() && !pbd::practice::native::in_native_call() && ppu.id == pbd::t5dr::main_ppu_id &&
		       ppu.cia == pbd::t5dr::boundary_pc && ppu.gpr[2] == pbd::t5dr::toc && !ppu.current_function && !ppu.state.load();
	}
	scope observe_scope(u64 stage_generation, u64 pair_generation) noexcept
	{
		scope out;
		const auto owner = pbd::practice::native::inspect(false);
		if (!owner.practice)
			return out;
		out.emulation_generation = owner.generation;
		out.stage_generation = stage_generation;
		out.pair_generation = pair_generation;
		out.stage = pbd::practice::native::word(0x1008f088);
		out.characters = {pbd::practice::native::word(0x10093f50), pbd::practice::native::word(0x10093f54)};
		return out;
	}
	bool placement_available(placement target, const scope& lifetime, const std::optional<placement_token>& remembered) noexcept
	{
		if (!requested() || !same_scope(lifetime))
			return false;
		if (target == placement::native_start)
			return true;
		if (target == placement::centre)
			return centre_basis_matches();
		if (target == placement::remembered)
			return remembered && token_matches(*remembered, lifetime);
		if (wall_target(target) || target == placement::random)
		{
			try
			{
				return wall_choice_available(target, lifetime);
			}
			catch (...)
			{
				return false;
			}
		}
		return false;
	}
	wall_report survey_walls(const scope& lifetime) noexcept
	{
		wall_report out;
		try
		{
			if (!requested())
			{
				out.reason = "restarts_off";
				return out;
			}
			const auto collision = read_collision(lifetime, &out);
			if (collision)
				for (u32 at = 0; at < out.available.size(); ++at)
					out.available[at] = pbd::practice::walls::place(collision->geometry, static_cast<pbd::practice::walls::position>(at),
						wall_menu_clearance, wall_pair_separation)
					                        .has_value();
		}
		catch (...)
		{
			out = wall_report{};
			out.reason = "exception";
		}
		return out;
	}
	std::optional<placement_token> capture_placement(ppu_thread& ppu, const scope& lifetime) noexcept
	{
		// Remember only reads actor placement at the owned input boundary. Pending PPU flags can defer a guest
		// call, but do not invalidate this read; the repeated scope/pair check still rejects a changing arena.
		if (!requested() || pbd::practice::native::in_native_call() || ppu.id != pbd::t5dr::main_ppu_id ||
			ppu.cia != pbd::t5dr::boundary_pc || ppu.gpr[2] != pbd::t5dr::toc || !same_scope(lifetime))
			return {};
		placement_token token;
		token.lifetime = lifetime;
		token.actors = actors();
		for (u32 side = 0; side < 2; ++side)
			if (!token.actors[side] || !read_position(token.actors[side], token.positions[side]))
				return {};
		if (!same_scope(lifetime) || token.actors != actors())
			return {};
		return token;
	}
	receipt execute(ppu_thread& ppu, const practice_reset& command) noexcept
	{
		receipt out;
		out.request_id = command.request_id;
		out.stage_generation = command.lifetime.stage_generation;
		out.pair_generation = command.lifetime.pair_generation;
		try
		{
			out.observed_lifetime = observe_scope(command.lifetime.stage_generation, command.lifetime.pair_generation);
			pbd::practice::native::owner_view owner;
			if (!admit_reset(ppu, command, out, owner))
				return out;
			practice_reset resolved = command;
			std::optional<collision_view> collision;
			if (!resolve_reset(command, resolved, collision, out) || !qualify_reset_body(command, owner, out))
				return out;
			dispatch_reset(ppu, command, owner, resolved, collision, out);
		}
		catch (...)
		{
			out.reason = "native_reset_exception";
		}
		return out;
	}
} // namespace pbd::game_lifecycle
