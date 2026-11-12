#include "stdafx.h"
#include "pbd_practice_reset_internal.h"

namespace pbd::game_lifecycle::detail
{
	bool collision_span_valid(u32 address, u64 bytes)
	{
		return address && !(address & 3) && bytes && bytes <= 2 * 1024 * 1024 &&
		       static_cast<u64>(address) + bytes <= 0x100000000ull;
	}

	void mix_collision(collision_view& out, u32 value)
	{
		out.digest ^= value;
		out.digest *= 1099511628211ull;
	}

	const char* read_collision_polygons(collision_view& out, std::vector<pbd::practice::walls::polygon>& polygons)
	{
		using pbd::practice::native::word;
		using namespace pbd::practice::walls;
		for (u32 index = 0; index < out.count; ++index)
		{
			std::array<be_t<u32>, 24> object{};
			const u32 address = out.objects + index * 0x60;
			if (!vm::try_access(address, object.data(), sizeof(object), false))
				return "object_unreadable";
			const u32 kind = object[16], payload = object[17];
			mix_collision(out, address);
			mix_collision(out, kind);
			mix_collision(out, payload);
			mix_collision(out, active_polygon(object[23]) ? 1u : 0u);
			if (kind != 3 && kind != 5 && kind != 6 && kind != 9)
				return "object_kind";
			if (kind != 3 && kind != 9)
				continue;
			const u32 count_offset = kind == 3 ? 0x10 : 0;
			const u32 record_offset = kind == 3 ? 0x20 : 0x10;
			if (!collision_span_valid(payload, record_offset))
				return "polygon_payload";
			const u32 count = word(payload + count_offset);
			if (count > 4096 || polygons.size() + count > 8192)
				return "polygon_budget";
			if (count && !collision_span_valid(payload, record_offset + count * 0x50ull))
				return "polygon_payload";
			mix_collision(out, count);
			for (u32 record = 0; record < count; ++record)
			{
				std::array<be_t<u32>, 20> words{};
				if (!vm::try_access(payload + record_offset + record * 0x50, words.data(), sizeof(words), false))
					return "polygon_unreadable";
				polygon polygon;
				for (u32 at = 0; at < words.size(); ++at)
				{
					const u32 bits = words[at];
					mix_collision(out, bits);
					if (at < 16)
						polygon.vertices[at / 4][at % 4] = std::bit_cast<float>(bits);
					else
						polygon.normal[at - 16] = std::bit_cast<float>(bits);
				}
				polygon.active = active_polygon(object[23]);
				polygons.push_back(polygon);
			}
		}
		return nullptr;
	}

	std::optional<collision_view> read_collision(const scope& lifetime, wall_report* report, bool admit_floor)
	{
		using pbd::practice::native::word;
		using namespace pbd::practice::walls;
		const auto refuse = [&](const char* reason) -> std::optional<collision_view>
		{
			if (report)
				report->reason = reason;
			return std::nullopt;
		};
		if (!same_scope(lifetime))
			return refuse("stale_scope");
		if (word(0x469704) != 0x10098e94 || word(0x46965c) != 0x10053630 ||
			word(0x469654) != 0x10095d6c || word(0x6db80) != 0x380000c0 || word(0x77b34) != 0x800291c4 ||
			word(0x78d04) != 0x90160008 || word(0x78d08) != 0x993c0010)
			return refuse("collection_code_mismatch");
		collision_view out;
		out.source = word(0x10098e94);
		out.query = word(0x10098e9c);
		// 78D08 publishes the imported flag at +10, after the format string's +F terminator. Read the same byte
		// as the native importer and the paused collection observer before treating its pointers as relocated.
		if (!collision_span_valid(out.source, 0x40) || !collision_span_valid(out.query, 0x60) ||
			pbd::practice::native::byte(out.source + 0x10) != 1 || word(out.source + 0x30) != out.query)
			return refuse("collection_not_imported");
		out.count = word(out.source + 0x34);
		out.objects = word(out.source + 0x38);
		if (!out.count || out.count > 2048 || !collision_span_valid(out.objects, out.count * 0x60ull) || word(out.query + 0x40) != 5)
			return refuse("collection_shape");
		const u32 grid = word(out.query + 0x44);
		if (!collision_span_valid(grid, 12))
			return refuse("collection_grid");
		const u32 width = word(grid), height = word(grid + 4);
		const float cell = std::bit_cast<float>(word(grid + 8));
		if (!width || width > 256 || !height || height > 256 || !std::isfinite(cell) || cell <= 0)
			return refuse("collection_grid");
		// Read the persistent imported collection, never the last actor's transient nearby-query results. Its complete
		// records also become the placement lifetime signature, so changed flags or relocated payloads invalidate a reset.
		out.digest = 14695981039346656037ull;
		std::vector<polygon> polygons;
		if (const auto* error = read_collision_polygons(out, polygons))
			return refuse(error);
		if (!same_scope(lifetime) || word(0x10098e94) != out.source || word(0x10098e9c) != out.query ||
			word(out.source + 0x30) != out.query || word(out.source + 0x34) != out.count || word(out.source + 0x38) != out.objects)
			return refuse("collection_changed");
		// The strict closed-convex reading first, so every stage it served keeps exactly its placements; otherwise the
		// general reading, which tolerates gaps, slight concavity and stray polygons.
		auto surveyed = survey(polygons);
		if (admit_floor && surveyed.path == wall_path::none)
		{
			const auto floor = pbd::practice::floor_boundary(polygons);
			if (floor)
			{
				out.geometry = *floor;
				out.floor_only = true;
			}
		}
		if (report)
		{
			report->read = true;
			report->reason = surveyed.refusal;
			report->path = path_name(surveyed.path);
			report->objects = out.count;
			report->digest = out.digest;
			report->counts = surveyed.counts;
			report->raw = std::move(polygons);
		}
		if (surveyed.path == wall_path::none && !out.floor_only)
			return {};
		if (!out.floor_only)
			out.geometry = std::move(surveyed.arena);
		return out;
	}
	bool same_collision(const collision_view& a, const collision_view& b) noexcept
	{
		return a.source == b.source && a.query == b.query && a.objects == b.objects && a.count == b.count && a.digest == b.digest;
	}
	pbd::practice::walls::position wall_position(placement target) noexcept
	{
		return static_cast<pbd::practice::walls::position>(static_cast<u32>(target) - static_cast<u32>(placement::left_wall));
	}
	struct body_sample
	{
		pbd::practice::walls::point offset;
		float radius = 0;
	};
	using body_samples = std::array<body_sample, 6>;
	std::optional<body_samples> collision_body(u32 actor)
	{
		using pbd::practice::native::word;
		constexpr std::array<u32, 6> bones{0, 20, 16, 12, 8, 3};
		constexpr std::array<u32, 6> radii{0x43160000, 0x43160000, 0x43160000, 0x43160000, 0x43160000, 0x43610000};
		if (word(0x468d68) != 0x10052a98 || word(0x468d74) != 0x10052ab0)
			return {};
		actor_placement centre;
		if (!read_position(actor, centre))
			return {};
		const u32 owner = word(actor + 0x8a4);
		if (!owner || (owner & 3) || owner > 0xfffffff0u)
			return {};
		const u32 matrices = word(owner + 0x10);
		if (!matrices || (matrices & 15) || static_cast<u64>(matrices) + 21 * 0x90 > 0x100000000ull)
			return {};
		body_samples out;
		for (u32 at = 0; at < bones.size(); ++at)
		{
			if (word(0x10052ab0 + at * 4) != bones[at] || word(0x10052a98 + at * 4) != radii[at])
				return {};
			std::array<be_t<u32>, 4> position{};
			if (!vm::try_access(matrices + bones[at] * 0x90 + 0x70, position.data(), sizeof(position), false))
				return {};
			for (u32 bits : position)
				if (!coordinate_valid(bits))
					return {};
			if (std::abs(std::bit_cast<float>(u32{position[3]}) - 1.f) > .001f)
				return {};
			out[at] = {{std::bit_cast<float>(u32{position[0]}) - std::bit_cast<float>(static_cast<u32>(centre.x)),
						   std::bit_cast<float>(u32{position[2]}) - std::bit_cast<float>(static_cast<u32>(centre.z))},
				std::bit_cast<float>(radii[at])};
			if (std::hypot(out[at].offset.x, out[at].offset.z) > 5000.f)
				return {};
		}
		if (word(actor + 0x8a4) != owner || word(owner + 0x10) != matrices)
			return {};
		return out;
	}
	std::optional<placement_token> wall_destination(const collision_view& arena, const practice_reset& command)
	{
		using namespace pbd::practice::walls;
		const auto target = wall_position(command.target);
		// This runs inside the native reset, where a refusal ends Practice, and execute() admitted the restart only because
		// the menu's placement exists on this same collection (its digest covers every polygon bit). Neither reading guarantees
		// that a pair which fits at the menu's clearance also fits nearer the wall: a side position's pair spreads along the hit
		// wall, and walls that close in on a short one (a hexagonal or chamfered end) leave less room the nearer the pair stands
		// to it, even on a closed convex loop. So both readings take the orientation from that admitted placement (the facing
		// depends only on the hit wall, never on the clearance). A pair one unit off the wall cannot give it: such an end refuses
		// that pair, which would end Practice on a restart the menu had offered.
		const auto trial = place(arena.geometry, target, wall_menu_clearance, wall_pair_separation);
		if (!trial)
			return {};
		placement_token out;
		out.lifetime = command.lifetime;
		out.actors = actors();
		// Each fighter as it will stand: its spot and facing in the pair, after Restart Settings' variant. The variant only
		// trades the two spots or turns the dummy along the pair's line, so every try's facings are the trial's.
		const auto standing = [&](const pair_placement& pair)
		{
			const point back{-pair.forward.x, -pair.forward.z};
			return pbd::practice::arranged({pbd::practice::facing_placement(pair.actors[0], pair.forward),
											   pbd::practice::facing_placement(pair.actors[1], back)},
				command.variant);
		};
		const auto oriented = standing(*trial);
		std::array<body_samples, 2> bodies;
		float clearance = 0;
		const auto hit = intersect(arena.geometry, wall_ray(target));
		if (!hit)
			return {};
		for (u32 side = 0; side < 2; ++side)
		{
			const auto body = collision_body(out.actors[side]);
			actor_placement initial;
			if (!body || !read_position(out.actors[side], initial))
				return {};
			const float delta = std::bit_cast<float>(oriented[side].yaw_bits) - std::bit_cast<float>(initial.yaw_bits);
			const float cosine = std::cos(delta), sine = std::sin(delta);
			bodies[side] = *body;
			for (auto& sample : bodies[side])
			{
				const auto offset = sample.offset;
				sample.offset = {cosine * offset.x + sine * offset.z, -sine * offset.x + cosine * offset.z};
				clearance = std::max(clearance, sample.radius - dot(hit->inward, sample.offset));
			}
		}
		// Fresh neutral-pose spheres, with a margin for idle displacement, must fit every strict wall plane or clear each
		// general wall segment and remain reachable from the origin. If a sphere reaches a neighbouring wall, retry in
		// 25-unit insets up to 300 units, skipping pairs that do not fit, then try the menu's 750-unit clearance.
		constexpr u32 retries = 13;
		for (u32 retry = 0; retry <= retries; ++retry)
		{
			const float wanted = retry == retries ? wall_menu_clearance : clearance + 50.f + static_cast<float>(retry) * 25.f;
			const auto placed = place(arena.geometry, target, wanted, wall_pair_separation);
			if (!placed)
				continue;
			const auto pair = standing(*placed);
			const auto fits = [&](u32 side)
			{
				const point at{std::bit_cast<float>(static_cast<u32>(pair[side].x)),
					std::bit_cast<float>(static_cast<u32>(pair[side].z))};
				return std::all_of(bodies[side].begin(), bodies[side].end(), [&](const body_sample& sample)
					{
						return contains(arena.geometry, {at.x + sample.offset.x, at.z + sample.offset.z}, sample.radius + 25.f);
					});
			};
			if (!fits(0) || !fits(1))
				continue;
			out.positions = pair;
			return out;
		}
		return {};
	}
	// The rough pair a wall restart writes before the reset's pose pass: the menu's own placement (750 from every wall, which
	// execute() admitted on this same collection), facing as the exact pair will. The facing depends only on the hit wall,
	// and the exact pair differs from this one only by its distance from that wall, so both lie on the same line with P1 on
	// the same side of P2. Both take the restart's variant the same way.
	std::optional<placement_token> rough_wall_pair(const collision_view& arena, const practice_reset& command)
	{
		using namespace pbd::practice::walls;
		const auto rough = place(arena.geometry, wall_position(command.target), wall_menu_clearance, wall_pair_separation);
		if (!rough)
			return {};
		placement_token out;
		out.lifetime = command.lifetime;
		out.actors = actors();
		const point back{-rough->forward.x, -rough->forward.z};
		out.positions = pbd::practice::arranged({pbd::practice::facing_placement(rough->actors[0], rough->forward),
													pbd::practice::facing_placement(rough->actors[1], back)},
			command.variant);
		return out;
	}
	bool wall_choice_available(placement target, const scope& lifetime)
	{
		using pbd::practice::native::word;
		struct menu_cache
		{
			scope lifetime;
			std::chrono::steady_clock::time_point expires{};
			std::array<u32, 4> identity{};
			std::optional<collision_view> collision;
		};
		thread_local menu_cache cache;
		const u32 source = word(0x10098e94);
		if (!source || source > 0xffffffc0u)
			return false;
		const std::array identity{source, word(0x10098e9c), word(source + 0x34), word(source + 0x38)};
		const auto now = std::chrono::steady_clock::now();
		// Menu labels can share a short read-only cache. Execute and the placement phase always read the full collection
		// independently, so a relocated or modified wall cannot be used just because the previous label was available.
		if (cache.lifetime != lifetime || cache.identity != identity || now >= cache.expires)
		{
			cache.lifetime = lifetime;
			cache.identity = identity;
			cache.expires = now + std::chrono::seconds(1);
			cache.collision = read_collision(lifetime, nullptr, true);
		}
		if (!cache.collision)
			return false;
		if (target == placement::random)
		{
			practice_reset command;
			command.lifetime = lifetime;
			return centre_basis_matches() &&
			       pbd::practice::random_destination(cache.collision->geometry, command, actors()).has_value();
		}
		if (cache.collision->floor_only)
			return false;
		return pbd::practice::walls::place(cache.collision->geometry, wall_position(target),
			wall_menu_clearance, wall_pair_separation)
		    .has_value();
	}
} // namespace pbd::game_lifecycle::detail
