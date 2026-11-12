#pragma once

#include "pbd_practice_wall_geometry.h"

namespace pbd::practice::walls
{
	// Finite, bounded, homogeneous vertices, a finite unit normal with no w component, and every vertex on that plane.
	inline bool well_formed(const polygon& polygon)
	{
		for (const auto& vertex : polygon.vertices)
		{
			for (float value : vertex)
				if (!std::isfinite(value) || std::abs(value) > 7000000.f)
					return false;
			if (std::abs(vertex[3] - 1.f) > .001f)
				return false;
		}
		for (float value : polygon.normal)
			if (!std::isfinite(value))
				return false;
		const auto& n = polygon.normal;
		if (std::abs(n[3]) > .001f || std::abs(std::hypot(std::hypot(n[0], n[2]), n[1]) - 1.f) > .001f)
			return false;
		const auto& base = polygon.vertices[0];
		const float support = n[0] * base[0] + n[1] * base[1] + n[2] * base[2];
		for (const auto& vertex : polygon.vertices)
			if (std::abs(n[0] * vertex[0] + n[1] * vertex[1] + n[2] * vertex[2] - support) > .02f)
				return false;
		return true;
	}

	inline bool closed_convex(std::span<const segment> walls)
	{
		std::vector<point> nodes;
		std::vector<std::array<unsigned, 2>> edges;
		const auto node = [&](point p)
		{
			for (unsigned at = 0; at < nodes.size(); ++at)
				if (distance(p, nodes[at]) < .01f)
					return at;
			nodes.push_back(p);
			return static_cast<unsigned>(nodes.size() - 1);
		};
		for (const auto& wall : walls)
			edges.push_back({node(wall.a), node(wall.b)});
		std::vector<unsigned> degrees(nodes.size());
		for (const auto& edge : edges)
		{
			++degrees[edge[0]];
			++degrees[edge[1]];
		}
		if (std::any_of(degrees.begin(), degrees.end(), [](unsigned degree)
				{
					return degree != 2;
				}))
			return false;
		std::vector<bool> visited(nodes.size());
		std::vector<unsigned> pending{0};
		while (!pending.empty())
		{
			const auto next = pending.back();
			pending.pop_back();
			if (visited[next])
				continue;
			visited[next] = true;
			for (const auto& edge : edges)
			{
				if (edge[0] == next && !visited[edge[1]])
					pending.push_back(edge[1]);
				if (edge[1] == next && !visited[edge[0]])
					pending.push_back(edge[0]);
			}
		}
		if (std::any_of(visited.begin(), visited.end(), [](bool value)
				{
					return !value;
				}))
			return false;
		for (const auto& wall : walls)
			for (point p : nodes)
				if (dot(wall.inward, p) - wall.plane < -.02f)
					return false;
		return true;
	}

	// Native collision collections include floors in the same record type as walls. Require the geometry itself to
	// describe one closed, inward, convex boundary; ambiguous props, broken loops and sloped walls are refused here and
	// left to the general reading below.
	inline boundary classify(std::span<const polygon> polygons)
	{
		boundary out;
		if (polygons.size() > 8192)
			return out;
		for (const auto& polygon : polygons)
		{
			if (!polygon.active)
				continue;
			if (!well_formed(polygon))
				return {};
			const auto& n = polygon.normal;
			if (std::abs(n[1]) > .999f)
				continue;
			if (std::abs(n[1]) > .001f)
				return {};
			if (out.walls.size() >= 256)
				return {};
			std::vector<point> ends;
			float low = polygon.vertices[0][1], high = low;
			for (const auto& vertex : polygon.vertices)
			{
				point p{vertex[0], vertex[2]};
				if (std::none_of(ends.begin(), ends.end(), [&](point old)
						{
							return distance(p, old) < .01f;
						}))
					ends.push_back(p);
				low = std::min(low, vertex[1]);
				high = std::max(high, vertex[1]);
			}
			if (ends.size() != 2 || low > 0.f || high < 0.f || high - low < .01f || distance(ends[0], ends[1]) < .01f)
				return {};
			const point normal{n[0], n[2]};
			const float plane = dot(normal, ends[0]);
			if (plane >= -.01f || std::abs(dot(normal, ends[1]) - plane) > .02f)
				return {};
			out.walls.push_back({ends[0], ends[1], normal, plane});
		}
		if (out.walls.empty())
		{
			out.supported = true;
			return out;
		}
		if (!closed_convex(out.walls))
			return {};
		out.supported = true;
		return out;
	}

	// Plan-view extent of a polygon that is not horizontal at floor level (y = 0, where the restart puts the fighters): the
	// farthest pair of the points where its outline reaches y = 0. Such a planar polygon meets y = 0 along one line, so
	// the pair spans the whole cut. Nothing when it does not reach floor level or meets it in a single point.
	inline std::optional<std::array<point, 2>> floor_section(const polygon& polygon)
	{
		std::array<point, 8> cuts{};
		unsigned count = 0;
		for (unsigned at = 0; at < polygon.vertices.size(); ++at)
		{
			const auto& a = polygon.vertices[at];
			const auto& b = polygon.vertices[(at + 1) % polygon.vertices.size()];
			if ((a[1] > 0.f && b[1] > 0.f) || (a[1] < 0.f && b[1] < 0.f))
				continue;
			if (a[1] == b[1])
			{
				// Neither above nor below and equal: both ends lie on floor level, and so does the edge.
				cuts[count++] = {a[0], a[2]};
				cuts[count++] = {b[0], b[2]};
				continue;
			}
			const float fraction = a[1] / (a[1] - b[1]);
			cuts[count++] = {a[0] + (b[0] - a[0]) * fraction, a[2] + (b[2] - a[2]) * fraction};
		}
		std::optional<std::array<point, 2>> out;
		float widest = .01f;
		for (unsigned i = 0; i < count; ++i)
			for (unsigned j = i + 1; j < count; ++j)
				if (const float span = distance(cuts[i], cuts[j]); span >= widest)
				{
					widest = span;
					out = std::array<point, 2>{cuts[i], cuts[j]};
				}
		return out;
	}

	inline void floor_obstacle(boundary& out, const polygon& source)
	{
		const auto section = floor_section(source);
		if (!section)
			return;
		const float length = std::hypot(source.normal[0], source.normal[2]);
		const point inward = length > .00001f ? point{source.normal[0] / length, source.normal[2] / length} : point{};
		out.obstacles.push_back({(*section)[0], (*section)[1], inward, dot(inward, (*section)[0])});
	}

	inline void read_general_polygon(const polygon& polygon, boundary& out, wall_counts& counts, bool& walkable)
	{
		if (!polygon.active)
		{
			++counts.inactive;
			return;
		}
		if (!well_formed(polygon))
		{
			++counts.malformed;
			return;
		}
		const auto& n = polygon.normal;
		walkable |= n[1] > .5f;
		if (std::abs(n[1]) > .999f)
		{
			++counts.floors;
			return;
		}
		if (std::abs(n[1]) > .001f)
		{
			++counts.sloped;
			if (std::abs(n[1]) <= .5f)
				floor_obstacle(out, polygon);
			return;
		}
		std::array<point, 4> ends{};
		unsigned distinct = 0;
		float low = polygon.vertices[0][1], high = low;
		for (const auto& vertex : polygon.vertices)
		{
			const point p{vertex[0], vertex[2]};
			if (std::none_of(ends.begin(), ends.begin() + distinct, [&](point old)
					{
						return distance(p, old) < .01f;
					}))
				ends[distinct++] = p;
			low = std::min(low, vertex[1]);
			high = std::max(high, vertex[1]);
		}
		if (distinct != 2)
		{
			++counts.shape;
			floor_obstacle(out, polygon);
			return;
		}
		if (low > 0.f || high < 0.f || high - low < .01f)
		{
			++counts.off_floor;
			return;
		}
		const point normal{n[0], n[2]};
		const float plane = dot(normal, ends[0]);
		if (std::abs(dot(normal, ends[1]) - plane) > .02f)
		{
			++counts.malformed;
			return;
		}
		if (plane < -.01f)
			out.walls.push_back({ends[0], ends[1], normal, plane});
		else
		{
			out.obstacles.push_back({ends[0], ends[1], normal, plane});
			++counts.facing_away;
		}
	}

	// General boundaries retain floor-level walls and obstacles while counting unusable polygons.
	// Malformed collections, missing floors and missing restart walls are refused as a whole.
	inline boundary classify_general(std::span<const polygon> polygons, wall_counts& counts, const char*& refusal)
	{
		counts = {};
		refusal = "";
		counts.polygons = static_cast<std::uint32_t>(std::min<std::size_t>(polygons.size(), 0xffffffffu));
		if (polygons.size() > 8192)
		{
			refusal = "polygon_budget";
			return {};
		}
		boundary out;
		bool walkable = false;
		for (const auto& polygon : polygons)
			read_general_polygon(polygon, out, counts, walkable);
		counts.walls = static_cast<std::uint32_t>(out.walls.size());
		const std::uint32_t well = counts.polygons - counts.inactive - counts.malformed;
		if (counts.malformed > well)
		{
			refusal = "malformed_collection";
			return {};
		}
		if (!walkable)
		{
			refusal = "no_floor";
			return {};
		}
		if (out.walls.empty())
		{
			refusal = "no_walls";
			return {};
		}
		out.supported = out.general = true;
		return out;
	}

	// Use the strict closed-convex reading when it qualifies, otherwise the general reading. Counts come from the general
	// pass so diagnostics describe the same polygons with either reading.
	inline wall_survey survey(std::span<const polygon> polygons)
	{
		wall_survey out;
		auto general = classify_general(polygons, out.counts, out.refusal);
		auto strict = classify(polygons);
		if (strict.supported && !strict.walls.empty())
		{
			out.arena = std::move(strict);
			out.path = wall_path::strict;
			out.refusal = "";
		}
		else if (general.supported)
		{
			out.arena = std::move(general);
			out.path = wall_path::general;
		}
		return out;
	}

} // namespace pbd::practice::walls
