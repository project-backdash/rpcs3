#pragma once

#include "../Cell/pbd_practice_walls.h"

namespace pbd::practice
{
	namespace geometry = pbd::practice::walls;

	struct random_position_options
	{
		float clearance = 750.f;
		float separation = 2000.f;
	};

	namespace detail
	{
		struct position_random
		{
			std::uint32_t state;

			std::uint32_t next() noexcept
			{
				state = state * 1664525u + 1013904223u;
				return state;
			}

			float unit() noexcept
			{
				return static_cast<float>(next() >> 8) * (1.f / 16777216.f);
			}
		};

		inline bool valid_pair(const geometry::boundary& arena, const geometry::pair_placement& pair, float clearance)
		{
			return geometry::contains(arena, pair.actors[0], clearance) &&
			       geometry::contains(arena, pair.actors[1], clearance) &&
			       geometry::pair_clear(arena, pair.actors);
		}

		inline std::optional<geometry::pair_placement> near_wall(
			const geometry::boundary& arena, position_random& random, const random_position_options& options)
		{
			const auto first = random.next() >> 30;
			const float clearance = options.clearance + random.unit() * 250.f;
			for (unsigned offset = 0; offset < 4; ++offset)
			{
				const auto target = static_cast<geometry::position>((first + offset) % 4);
				const auto pair = geometry::place(arena, target, clearance, options.separation);
				if (pair && valid_pair(arena, *pair, options.clearance))
					return pair;
			}
			return {};
		}

		inline std::optional<std::array<geometry::point, 2>> extent(const geometry::boundary& arena)
		{
			geometry::point low{7000000.f, 7000000.f}, high{-7000000.f, -7000000.f};
			for (const auto& wall : arena.walls)
			{
				for (const auto at : {wall.a, wall.b})
				{
					if (!std::isfinite(at.x) || !std::isfinite(at.z) ||
						std::abs(at.x) > 7000000.f || std::abs(at.z) > 7000000.f)
						return {};
					low.x = std::min(low.x, at.x);
					low.z = std::min(low.z, at.z);
					high.x = std::max(high.x, at.x);
					high.z = std::max(high.z, at.z);
				}
			}
			if (low.x >= high.x || low.z >= high.z)
				return {};
			return std::array{low, high};
		}

		inline geometry::pair_placement interior(
			position_random& random, const std::array<geometry::point, 2>& bounds, float separation)
		{
			const geometry::point centre{
				bounds[0].x + random.unit() * (bounds[1].x - bounds[0].x),
				bounds[0].z + random.unit() * (bounds[1].z - bounds[0].z)};
			const float angle = random.unit() * (2.f * std::numbers::pi_v<float>);
			const geometry::point forward{std::cos(angle), std::sin(angle)};
			const geometry::point half{forward.x * separation * .5f, forward.z * separation * .5f};
			return {{{{centre.x - half.x, centre.z - half.z}, {centre.x + half.x, centre.z + half.z}}}, forward};
		}
	} // namespace detail

	// Entropy is supplied by the caller. Sampling never consumes the game's random sequence, and one request keeps one seed.
	inline std::optional<geometry::pair_placement> random_position(
		const geometry::boundary& arena, std::uint32_t seed, const random_position_options& options = {})
	{
		if (!arena.supported || arena.walls.empty() || arena.walls.size() > 256 || arena.obstacles.size() > 8192 ||
			!std::isfinite(options.clearance) || options.clearance <= 0.f ||
			!std::isfinite(options.separation) || options.separation <= 0.f)
			return {};
		const auto bounds = detail::extent(arena);
		if (!bounds)
			return {};
		detail::position_random random{seed};
		if ((random.next() >> 30) == 0)
		{
			const auto wall = detail::near_wall(arena, random, options);
			if (wall)
				return wall;
		}
		for (unsigned attempt = 0; attempt < 96; ++attempt)
		{
			const auto pair = detail::interior(random, *bounds, options.separation);
			if (detail::valid_pair(arena, pair, options.clearance))
				return pair;
		}
		return detail::near_wall(arena, random, options);
	}
} // namespace pbd::practice
