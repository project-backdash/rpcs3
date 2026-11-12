#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace pbd::practice::walls
{
	struct point
	{
		float x = 0, z = 0;
	};
	struct polygon
	{
		std::array<std::array<float, 4>, 4> vertices{};
		std::array<float, 4> normal{};
		bool active = true;
	};
	struct segment
	{
		point a, b, inward;
		float plane = 0;
	};
	struct boundary
	{
		bool supported = false;
		// Set when the walls come from the general reading (classify_general) rather than one closed convex loop.
		bool general = false;
		std::vector<segment> walls;
		// General reading only: floor-level vertical segments whose inward side faces away from the stage origin, and where
		// steep sloped or misshapen polygons meet floor level. They are never a restart wall, but fighters keep their
		// clearance from them and no path from the origin, or between the pair, may cross them.
		std::vector<segment> obstacles;
	};
	// angle: degrees between the requested direction and the ray that reached the wall; only the general sweep turns it.
	struct crossing
	{
		point position, inward;
		float distance = 0, angle = 0;
	};
	enum class position
	{
		p1_back,
		p2_back,
		foreground,
		background
	};
	struct pair_placement
	{
		std::array<point, 2> actors{};
		point forward;
	};
	// What the general reading made of each polygon. polygons counts every record read; the other counts cover active
	// polygons only. Floors are expected and ignored; inactive, malformed, sloped, misshapen and off-floor polygons are
	// never a restart wall; facing_away segments are kept as obstacles. A sloped polygon too steep to stand on and a
	// misshapen vertical one still block where they meet floor level (see classify_general), though counted as skipped.
	struct wall_counts
	{
		std::uint32_t polygons = 0, walls = 0, floors = 0, inactive = 0, malformed = 0, sloped = 0, shape = 0, off_floor = 0, facing_away = 0;
	};
	enum class wall_path : std::uint8_t
	{
		none,
		strict,
		general
	};
	struct wall_survey
	{
		boundary arena;
		wall_path path = wall_path::none;
		wall_counts counts;
		// Why no path applies ("polygon_budget", "malformed_collection", "no_floor" or "no_walls"); empty when one does.
		const char* refusal = "";
	};
	// Native callback6DB04 reads a byte at object+5C. That is the most significant byte of the big-endian word.
	inline bool active_polygon(std::uint32_t flags_word)
	{
		return ((flags_word >> 24) & 1u) != 0;
	}
	inline float dot(point a, point b)
	{
		return a.x * b.x + a.z * b.z;
	}
	inline point subtract(point a, point b)
	{
		return {a.x - b.x, a.z - b.z};
	}
	inline float distance(point a, point b)
	{
		return std::hypot(a.x - b.x, a.z - b.z);
	}
	inline const char* path_name(wall_path path)
	{
		return path == wall_path::strict ? "strict" : path == wall_path::general ? "general" :
		                                                                           "none";
	}

} // namespace pbd::practice::walls
