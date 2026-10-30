#pragma once

#include "../Cell/pbd_practice_walls.h"

namespace pbd::practice
{
	namespace floor_detail
	{
		namespace geometry = pbd::practice::walls;
		struct edge
		{
			geometry::point a, b, inward;
			unsigned copies = 1;
		};

		inline std::optional<std::array<edge, 4>> tile_edges(const geometry::polygon& polygon)
		{
			if (!geometry::well_formed(polygon) || polygon.normal[1] < .999f)
				return {};
			std::array<geometry::point, 4> points{};
			float area = 0.f;
			for (unsigned at = 0; at < 4; ++at)
			{
				const auto& vertex = polygon.vertices[at];
				if (std::abs(vertex[1]) > .02f)
					return {};
				points[at] = {vertex[0], vertex[2]};
				const auto& next = polygon.vertices[(at + 1) % 4];
				area += vertex[0] * next[2] - next[0] * vertex[2];
			}
			if (!std::isfinite(area) || std::abs(area) < .01f)
				return {};
			const float winding = area > 0.f ? 1.f : -1.f;
			std::array<edge, 4> out{};
			for (unsigned at = 0; at < 4; ++at)
			{
				const auto a = points[at], b = points[(at + 1) % 4], c = points[(at + 2) % 4];
				const auto ab = geometry::subtract(b, a), bc = geometry::subtract(c, b);
				if (winding * (ab.x * bc.z - ab.z * bc.x) <= .01f)
					return {};
				const float length = geometry::distance(a, b);
				if (length <= .01f)
					return {};
				out[at] = {a, b, {-winding * ab.z / length, winding * ab.x / length}};
			}
			return out;
		}

		inline bool append_edge(std::vector<edge>& edges, const edge& next)
		{
			for (auto& previous : edges)
			{
				if (geometry::distance(previous.a, next.b) < .01f && geometry::distance(previous.b, next.a) < .01f)
					return ++previous.copies == 2;
				if (geometry::distance(previous.a, next.a) < .01f && geometry::distance(previous.b, next.b) < .01f)
					return false;
			}
			edges.push_back(next);
			return true;
		}

		inline geometry::polygon outline_panel(const edge& edge)
		{
			geometry::polygon out;
			out.vertices = {{{edge.a.x, -1.f, edge.a.z, 1.f}, {edge.a.x, 1.f, edge.a.z, 1.f},
				{edge.b.x, 1.f, edge.b.z, 1.f}, {edge.b.x, -1.f, edge.b.z, 1.f}}};
			out.normal = {edge.inward.x, 0.f, edge.inward.z, 0.f};
			return out;
		}
	} // namespace floor_detail

	// A flat floor mesh can bound sampling without physical walls. Only a complete convex union around the origin is admitted.
	inline std::optional<pbd::practice::walls::boundary> floor_boundary(
		std::span<const pbd::practice::walls::polygon> polygons)
	{
		if (polygons.empty() || polygons.size() > 256)
			return {};
		std::vector<floor_detail::edge> edges;
		for (const auto& polygon : polygons)
		{
			if (!polygon.active)
				continue;
			const auto tile = floor_detail::tile_edges(polygon);
			if (!tile)
				return {};
			for (const auto& edge : *tile)
				if (!floor_detail::append_edge(edges, edge))
					return {};
		}
		std::vector<pbd::practice::walls::polygon> outline;
		for (const auto& edge : edges)
			if (edge.copies == 1)
				outline.push_back(floor_detail::outline_panel(edge));
		auto out = pbd::practice::walls::classify(outline);
		if (!out.supported || out.walls.empty())
			return {};
		return out;
	}
} // namespace pbd::practice
