#pragma once

#include "../PBD/pbd_practice_wall_classification.h"

namespace pbd::practice::walls
{
	// Nearest finite wall segment facing a unit ray from the stage origin. A plane extended beyond its polygon can produce
	// a destination through a corner, so the hit must lie on the segment itself, including its finite extent.
	inline std::optional<crossing> cast(std::span<const segment> walls, point direction)
	{
		std::optional<crossing> closest;
		float alignment = 0;
		for (const auto& wall : walls)
		{
			const float divisor = dot(wall.inward, direction);
			if (divisor >= -.00001f)
				continue;
			const float travel = wall.plane / divisor;
			point p{direction.x * travel, direction.z * travel};
			const point edge = subtract(wall.b, wall.a);
			const float fraction = dot(subtract(p, wall.a), edge) / dot(edge, edge);
			if (!std::isfinite(travel) || travel <= 0 || fraction < -.00001f || fraction > 1.00001f)
				continue;
			if (!closest || travel < closest->distance - .01f || (std::abs(travel - closest->distance) < .01f && -divisor > alignment))
			{
				closest = crossing{p, wall.inward, travel};
				alignment = -divisor;
			}
		}
		return closest;
	}

	// The general reading's gap sweep: the requested direction first, then alternately either side in 5-degree steps up to
	// 30 degrees, so the nearest angle that reaches a wall wins and a narrow opening does not cost the whole position.
	inline constexpr std::array<float, 13> sweep_degrees{0, 5, -5, 10, -10, 15, -15, 20, -20, 25, -25, 30, -30};
	// General walls must face back within 60 degrees of the requested direction; otherwise a swept ray could place a
	// fighter against a perpendicular wall. Skip those hits and continue the sweep.
	inline constexpr float general_facing_limit = .5f;

	inline std::optional<crossing> intersect(const boundary& arena, point direction)
	{
		if (!arena.supported || !std::isfinite(direction.x) || !std::isfinite(direction.z))
			return {};
		const float length = std::hypot(direction.x, direction.z);
		if (!std::isfinite(length) || length < .00001f)
			return {};
		direction.x /= length;
		direction.z /= length;
		if (!arena.general)
			return cast(arena.walls, direction);
		for (const float degrees : sweep_degrees)
		{
			const float angle = degrees * (std::numbers::pi_v<float> / 180.f);
			const float cosine = std::cos(angle), sine = std::sin(angle);
			auto hit = cast(arena.walls, {direction.x * cosine - direction.z * sine, direction.x * sine + direction.z * cosine});
			if (hit && dot(hit->inward, direction) <= -general_facing_limit)
			{
				hit->angle = degrees;
				return hit;
			}
		}
		return {};
	}

	inline float distance_to(const segment& wall, point p)
	{
		const point edge = subtract(wall.b, wall.a);
		const float fraction = std::clamp(dot(subtract(p, wall.a), edge) / dot(edge, edge), 0.f, 1.f);
		return distance(p, {wall.a.x + edge.x * fraction, wall.a.z + edge.z * fraction});
	}

	// Plan-view distance from p to the nearest wall or obstacle segment.
	inline float nearest_wall(const boundary& arena, point p)
	{
		float out = std::numeric_limits<float>::infinity();
		for (const auto& wall : arena.walls)
			out = std::min(out, distance_to(wall, p));
		for (const auto& wall : arena.obstacles)
			out = std::min(out, distance_to(wall, p));
		return out;
	}

	inline float orientation(point origin, point a, point b)
	{
		return (a.x - origin.x) * (b.z - origin.z) - (a.z - origin.z) * (b.x - origin.x);
	}

	// Closed segments pq and ab share a point. Touching counts, so a path through a wall's end point is refused too.
	inline bool crosses(point p, point q, point a, point b)
	{
		const float d1 = orientation(p, q, a), d2 = orientation(p, q, b), d3 = orientation(a, b, p), d4 = orientation(a, b, q);
		if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0) || (d3 > 0 && d4 > 0) || (d3 < 0 && d4 < 0))
			return false;
		if (d1 != 0 || d2 != 0 || d3 != 0 || d4 != 0)
			return true;
		// All four points on one line: the segments meet only if their extents along it overlap.
		const point axis = subtract(b, a);
		const float at_p = dot(subtract(p, a), axis), at_q = dot(subtract(q, a), axis);
		return std::max(std::min(at_p, at_q), 0.f) <= std::min(std::max(at_p, at_q), dot(axis, axis));
	}

	// Strict reading: inside every supporting wall plane by the clearance. The general reading has no closed convex loop
	// to tell inside from outside, so a position is valid when it is at least the clearance from every wall and obstacle
	// segment and reachable in a straight line from the stage origin without crossing one. The origin stands for the
	// fighting area because DR's own centre start (1AFEC) places the pair about it on every stage, and because the menu,
	// the restart and its placement phase must reach the same answer although the fighters move in between.
	inline bool contains(const boundary& arena, point p, float clearance)
	{
		if (!arena.supported || arena.walls.empty() || !std::isfinite(p.x) || !std::isfinite(p.z) ||
			!std::isfinite(clearance) || clearance < 0)
			return false;
		if (arena.general)
		{
			if (!(nearest_wall(arena, p) >= clearance))
				return false;
			const auto blocks = [&](const segment& wall)
			{
				return crosses({0, 0}, p, wall.a, wall.b);
			};
			return std::none_of(arena.walls.begin(), arena.walls.end(), blocks) &&
			       std::none_of(arena.obstacles.begin(), arena.obstacles.end(), blocks);
		}
		return std::all_of(arena.walls.begin(), arena.walls.end(), [&](const segment& wall)
			{
				return dot(wall.inward, p) - wall.plane >= clearance;
			});
	}

	// General reading. contains() also admits a point the origin sees through an opening in the walls (a missing panel,
	// or one too malformed to read) beyond where that wall would be: nothing stands between, and nothing is near. A point
	// is walled in when the ray from the origin through it goes on to meet a restart wall; rays a quarter degree either
	// side count too, so a joint between panels that do not quite meet (a few units on stage17) never decides it.
	inline constexpr float enclosure_tolerance_degrees = .25f;
	inline bool enclosed(const boundary& arena, point p)
	{
		const float length = std::hypot(p.x, p.z);
		if (!std::isfinite(length))
			return false;
		// The origin itself stands for the fighting area.
		if (length < 1.f)
			return true;
		const point direction{p.x / length, p.z / length};
		for (const float degrees : {0.f, enclosure_tolerance_degrees, -enclosure_tolerance_degrees})
		{
			const float angle = degrees * (std::numbers::pi_v<float> / 180.f);
			const float cosine = std::cos(angle), sine = std::sin(angle);
			const auto hit = cast(arena.walls, {direction.x * cosine - direction.z * sine, direction.x * sine + direction.z * cosine});
			if (hit && hit->distance >= length)
				return true;
		}
		return false;
	}

	// General reading: a pair contains() admitted one fighter at a time is a restart only when each fighter is walled in
	// and no wall or obstacle stands between the two: a pillar or partition inside the triangle the origin and the pair
	// make crosses neither path from the origin, so contains() alone would face the fighters at each other through it.
	inline bool pair_clear(const boundary& arena, const std::array<point, 2>& actors)
	{
		const auto between = [&](const segment& wall)
		{
			return crosses(actors[0], actors[1], wall.a, wall.b);
		};
		return enclosed(arena, actors[0]) && enclosed(arena, actors[1]) &&
		       std::none_of(arena.walls.begin(), arena.walls.end(), between) &&
		       std::none_of(arena.obstacles.begin(), arena.obstacles.end(), between);
	}

	// Direction from the stage origin to the wall a position uses. Placement and its clearance check must share it.
	inline point wall_ray(position target)
	{
		return target == position::p1_back ? point{-1, 0} : target == position::p2_back ? point{1, 0} :
		                                                target == position::foreground  ? point{0, 1} :
		                                                                                  point{0, -1};
	}

	// At a corner, or along a curved run of short segments, a fighter standing at the clearance from the hit wall can still be
	// inside a neighbouring wall's clearance. The pair then steps away from the hit wall in 25-unit steps, at most 1000 units
	// (half the usual separation) beyond the clearance; further out it is no longer a wall start, and the position stays
	// unavailable. Both readings step. A closed convex loop has the same corners: a side position's pair spreads 1000 units
	// either way along the hit wall, so a wall shorter than that between two walls that close in on it (a hexagonal or
	// chamfered end) leaves no room at the clearance itself but does a few steps further in; trying the clearance alone would
	// refuse such a position outright. A pair that fits at the clearance is found by the first step, so stepping never moves
	// a position that fits there.
	inline constexpr float general_inset_step = 25.f;
	inline constexpr unsigned general_inset_steps = 40;

	// Back-wall positions put the pair on the wall normal. Side-wall positions put it on the wall tangent. The camera
	// frames the restarted pair from a fixed side of P1's facing, so foreground or background depends only on which side
	// of P1 the wall is on, never on which world wall is used. In game, a wall on the {forward.z, -forward.x} side of P1
	// shows behind the fighters: Background puts it there and Foreground on the other side. Validate both centres with
	// contains(): every actual boundary plane for the strict reading, every segment and the path from the origin for the
	// general one, which also needs pair_clear(). pair_clear() belongs to the pair rather than to contains(), whose
	// clearance alone is what the restart's body samples are held to: a sample beside a fighter that is walled in can
	// look out through the opening next to it, and a restart the menu offered must still land.
	inline std::optional<pair_placement> place(const boundary& arena, position target, float clearance, float separation)
	{
		if (static_cast<unsigned>(target) > static_cast<unsigned>(position::background) ||
			!std::isfinite(clearance) || !std::isfinite(separation) || clearance <= 0 || separation <= 0)
			return {};
		const auto hit = intersect(arena, wall_ray(target));
		if (!hit)
			return {};
		const auto add = [](point a, point b, float scale)
		{
			return point{a.x + b.x * scale, a.z + b.z * scale};
		};
		for (unsigned step = 0; step <= general_inset_steps; ++step)
		{
			// A small inward rounding allowance keeps a mathematically exact clearance inside after native float conversion.
			const point anchor = add(hit->position, hit->inward, clearance + .05f + static_cast<float>(step) * general_inset_step);
			pair_placement out;
			if (target == position::p1_back)
			{
				out.actors = {anchor, add(anchor, hit->inward, separation)};
				out.forward = hit->inward;
			}
			else if (target == position::p2_back)
			{
				out.actors = {add(anchor, hit->inward, separation), anchor};
				out.forward = {-hit->inward.x, -hit->inward.z};
			}
			else
			{
				out.forward = target == position::background ? point{hit->inward.z, -hit->inward.x} : point{-hit->inward.z, hit->inward.x};
				out.actors = {add(anchor, out.forward, -separation * .5f), add(anchor, out.forward, separation * .5f)};
			}
			if (!contains(arena, out.actors[0], clearance) || !contains(arena, out.actors[1], clearance))
				continue;
			if (arena.general && !pair_clear(arena, out.actors))
				continue;
			return out;
		}
		return {};
	}
} // namespace pbd::practice::walls
