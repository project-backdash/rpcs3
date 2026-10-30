#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "practice_wall_model.h"

#include <limits>
#include <string_view>
#include <vector>

namespace
{
	using pbd::test::check;

	using namespace pbd::practice::walls;

	bool wall_pair_oriented(position target, const pair_placement& placed, const crossing& wall)
	{
		const point behind{placed.forward.z, -placed.forward.x};
		return target == position::p1_back    ? dot(placed.forward, wall.inward) > .999f :
		       target == position::p2_back    ? dot(placed.forward, wall.inward) < -.999f :
		       target == position::background ? dot(behind, wall.inward) < -.999f :
		                                        dot(behind, wall.inward) > .999f;
	}

	polygon synthetic_wall_panel(point a, point b, point inward)
	{
		polygon out;
		out.vertices = {{{a.x, -5000.f, a.z, 1.f}, {a.x, 5000.f, a.z, 1.f}, {b.x, 5000.f, b.z, 1.f}, {b.x, -5000.f, b.z, 1.f}}};
		out.normal = {inward.x, 0.f, inward.z, 0.f};
		return out;
	}

	std::vector<polygon> synthetic_wall_ring(const polygon& ground, std::initializer_list<unsigned> missing)
	{
		std::array<point, 24> corners{};
		for (unsigned k = 0; k < corners.size(); ++k)
		{
			const double angle = (7.5 + 15.0 * k) * std::numbers::pi / 180.0;
			corners[k] = {static_cast<float>(9000.0 * std::cos(angle)), static_cast<float>(9000.0 * std::sin(angle))};
		}
		std::vector<polygon> out{ground};
		for (unsigned k = 0; k < corners.size(); ++k)
		{
			if (std::find(missing.begin(), missing.end(), k) != missing.end())
				continue;
			const point a = corners[k], b = corners[(k + 1) % corners.size()];
			point inward{a.z - b.z, b.x - a.x};
			const float length = std::hypot(inward.x, inward.z);
			inward = {inward.x / length, inward.z / length};
			if (dot(inward, a) > 0)
				inward = {-inward.x, -inward.z};
			out.push_back(synthetic_wall_panel(a, b, inward));
		}
		return out;
	}

	void strict_wall_placements(const boundary& arena)
	{
		for (point direction : std::array<point, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
		{
			const auto hit = intersect(arena, direction);
			check(hit && std::abs(hit->distance - 9000.f) < .1f, "all four restart directions must intersect actual finite wall segments");
		}
		const auto diagonal = intersect(arena, {1, 1});
		check(diagonal && std::abs(diagonal->position.x - 6366.2674f) < .1f && std::abs(diagonal->position.z - 6366.2674f) < .1f,
			"the octagonal corner must not become the enclosing rectangle's false corner");
		check(contains(arena, {0, 0}, 200.f) && !contains(arena, {0, 9001}, 0.f), "destinations must stay inside every actual supporting wall");
		for (const auto target : {position::p1_back, position::p2_back, position::foreground, position::background})
		{
			const auto placed = place(arena, target, 300.f, 2000.f);
			check(placed && contains(arena, placed->actors[0], 300.f) && contains(arena, placed->actors[1], 300.f) &&
					  std::abs(distance(placed->actors[0], placed->actors[1]) - 2000.f) < .1f,
				"both fighters must retain requested separation and clearance inside every finite wall");
			const auto wall = intersect(arena, wall_ray(target));
			// The wall on this side of P1's facing shows behind the fighters in game.
			const point behind{placed->forward.z, -placed->forward.x};
			check(target == position::p1_back    ? dot(placed->forward, wall->inward) > .999f :
				  target == position::p2_back    ? dot(placed->forward, wall->inward) < -.999f :
				  target == position::background ? dot(behind, wall->inward) < -.999f :
												   dot(behind, wall->inward) > .999f,
				"back and side wall choices must stay relative to P1's facing");
		}
		check(!place(arena, position::p1_back, 10000.f, 2000.f) && !place(arena, position::foreground, 300.f, 20000.f),
			"a pair that cannot fit inside the real arena must remain unavailable");
		check(!place(arena, static_cast<position>(255), 300.f, 2000.f) && !place(arena, position::p1_back, 0.f, 2000.f) &&
				  !place(arena, position::p1_back, 300.f, 0.f) && !place(arena, position::p1_back, std::numeric_limits<float>::quiet_NaN(), 2000.f),
			"invalid placement choices or physical sizes must not create destinations");
		check(!intersect(arena, {0, 0}) && !intersect(arena, {std::numeric_limits<float>::quiet_NaN(), 0}), "invalid directions must not create placements");
	}

	void malformed_wall_boundaries()
	{
		auto damaged = std::vector<polygon>(wall_model::stage30.begin(), wall_model::stage30.end());
		damaged.erase(damaged.begin());
		check(!classify(damaged).supported, "a missing wall panel must make the entire placement geometry unavailable");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		damaged.push_back(damaged[0]);
		check(!classify(damaged).supported, "duplicated panels must not create ambiguous boundary connections");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		for (auto& polygon : damaged)
			if (std::abs(polygon.normal[1]) < .001f)
			{
				polygon.normal[0] *= -1;
				polygon.normal[2] *= -1;
			}
		check(!classify(damaged).supported, "outward normals must not reverse the placement clearance");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		damaged.back().vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
		check(!classify(damaged).supported, "malformed floor data must not be mistaken for a wall-less collection");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		damaged[0].normal[1] = .2f;
		check(!classify(damaged).supported, "unqualified sloped geometry must remain unavailable");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		damaged[0].normal = {0, 1, 0, 0};
		check(!classify(damaged).supported, "a vertical quad with a mismatched floor normal must not conceal a wall");
		damaged.assign(wall_model::stage30.begin(), wall_model::stage30.end());
		for (const auto& original : wall_model::stage30)
		{
			if (std::abs(original.normal[1]) > .001f)
				continue;
			auto outer = original;
			for (auto& vertex : outer.vertices)
			{
				vertex[0] *= 2;
				vertex[2] *= 2;
			}
			damaged.push_back(outer);
		}
		check(!classify(damaged).supported, "multiple closed loops must not choose a guessed outer wall");
		check(!classify(std::vector<polygon>(8193)).supported, "collections beyond the native polygon budget must be refused");
	}

	void strict_survey_preserves_placements(const boundary& arena, const auto& strict, const auto& targets)
	{
		check(strict.path == wall_path::strict && !strict.arena.general && strict.arena.walls.size() == 21 && !*strict.refusal &&
				  strict.counts.polygons == 37 && strict.counts.walls == 21 && strict.counts.floors == 16 && !strict.counts.malformed,
			"stage30 must keep the strict closed-convex reading");
		for (const float clearance : {300.f, 750.f})
			for (const auto target : targets)
			{
				const auto before = place(arena, target, clearance, 2000.f), after = place(strict.arena, target, clearance, 2000.f);
				check(before && after && before->forward.x == after->forward.x && before->forward.z == after->forward.z &&
						  before->actors[0].x == after->actors[0].x && before->actors[0].z == after->actors[0].z &&
						  before->actors[1].x == after->actors[1].x && before->actors[1].z == after->actors[1].z,
					"a stage the strict reading serves must keep exactly the placements it had");
			}
	}

	void short_convex_wall_clearance()
	{
		{
			// A closed convex loop whose foreground wall (1600 long) sits between two walls closing in on it, as on a hexagonal or
			// chamfered stage end. Foreground's pair spreads 1000 either way along that wall: at the clearance itself one fighter
			// is inside a closing wall's clearance, ten steps (250 units) further in both fit. Trying the clearance alone would
			// refuse the position, and the native exact pair one unit off the wall fails inside the reset.
			const auto wall = [](point a, point b)
			{
				const float dx = b.x - a.x, dz = b.z - a.z, length = std::hypot(dx, dz);
				return polygon{{{{a.x, -5000.f, a.z, 1.f}, {a.x, 5000.f, a.z, 1.f}, {b.x, 5000.f, b.z, 1.f}, {b.x, -5000.f, b.z, 1.f}}},
					{-dz / length, 0.f, dx / length, 0.f}, true};
			};
			const std::array<point, 6> corners{{{-6000, -6000}, {6000, -6000}, {6000, 3000}, {800, 6000}, {-800, 6000}, {-6000, 3000}}};
			std::vector<polygon> hexagon;
			for (std::size_t at = 0; at < corners.size(); ++at)
				hexagon.push_back(wall(corners[at], corners[(at + 1) % corners.size()]));
			const auto closing = survey(hexagon);
			check(closing.path == wall_path::strict && closing.arena.walls.size() == 6, "the closing-in end is one closed convex loop");
			const auto hit = intersect(closing.arena, wall_ray(position::foreground));
			check(hit && std::abs(hit->position.z - 6000.f) < .1f, "foreground meets the short end wall");
			const point first{hit->position.x + hit->inward.x * 750.05f, hit->position.z + hit->inward.z * 750.05f};
			check(!contains(closing.arena, {first.x - 1000.f, first.z}, 750.f) || !contains(closing.arena, {first.x + 1000.f, first.z}, 750.f),
				"at the clearance itself the foreground pair does not fit");
			for (const float clearance : {750.f, 300.f, 50.f})
			{
				const auto placed = place(closing.arena, position::foreground, clearance, 2000.f);
				check(placed && contains(closing.arena, placed->actors[0], clearance) && contains(closing.arena, placed->actors[1], clearance) &&
						  std::abs(distance(placed->actors[0], placed->actors[1]) - 2000.f) < .1f && placed->forward.x > .999f,
					"stepping in from the short wall finds the foreground pair, facing as the side position always does");
			}
			for (const auto target : {position::p1_back, position::p2_back, position::background})
				check(place(closing.arena, target, 750.f, 2000.f).has_value(), "the other positions stay available");
		}
	}

	void general_stage_placements(const auto& general, const auto& targets)
	{
		for (const float clearance : {300.f, 750.f})
			for (const auto target : targets)
			{
				const auto hit = intersect(general.arena, wall_ray(target));
				const auto placed = place(general.arena, target, clearance, 2000.f);
				check(hit && hit->angle == 0 && placed, "every stage17 direction has a wall in line and must offer its position");
				check(contains(general.arena, placed->actors[0], clearance) && contains(general.arena, placed->actors[1], clearance) &&
						  std::abs(distance(placed->actors[0], placed->actors[1]) - 2000.f) < .1f,
					"stage17 pairs must clear every wall segment, be reachable from the origin and keep their separation");
				const point anchor = target == position::p1_back ? placed->actors[0] : target == position::p2_back ? placed->actors[1] :
				                                                                                                     point{(placed->actors[0].x + placed->actors[1].x) * .5f, (placed->actors[0].z + placed->actors[1].z) * .5f};
				check(wall_pair_oriented(target, *placed, *hit) && distance(anchor, hit->position) <= clearance + 1000.1f,
					"general pairs must keep the P1-relative orientation and stay within the step limit of their wall");
				check(pair_clear(general.arena, placed->actors) && dot(hit->inward, wall_ray(target)) <= -.98f,
					"stage17 pairs must be walled in, clear of each other and on a wall facing their direction within 11 degrees");
			}
	}

	void restart_pair_orientation(const auto& strict, const auto& general, const auto& targets)
	{
		// A wall restart writes the menu's pair (clearance 750) before the reset's pose pass and its exact pair after it. Both
		// must face the same way with P1 on the same side of P2, or the camera set up between them could draw the pair swapped.
		const auto same_layout = [](const pair_placement& rough, const pair_placement& exact)
		{
			const point rough_line = subtract(rough.actors[1], rough.actors[0]), exact_line = subtract(exact.actors[1], exact.actors[0]);
			return dot(rough.forward, exact.forward) > .9999f &&
			       dot(rough_line, exact_line) > .9999f * std::hypot(rough_line.x, rough_line.z) * std::hypot(exact_line.x, exact_line.z);
		};
		for (const auto* stage : {&strict.arena, &general.arena})
			for (const auto target : targets)
			{
				const auto rough = place(*stage, target, 750.f, 2000.f);
				for (const float clearance : {300.f, 500.f})
				{
					const auto exact = place(*stage, target, clearance, 2000.f);
					check(rough && exact && same_layout(*rough, *exact),
						"a wall restart's rough and exact pairs must share the facing and the side each fighter is on");
				}
			}
	}

	void general_survey_refusals(const auto& general, const auto& targets)
	{
		// Skipped polygons are counted, never fatal; a collection that is clearly not stage collision is refused whole.
		auto noisy = std::vector<polygon>(wall_model::stage17.begin(), wall_model::stage17.end());
		polygon ramp{{{{0.f, 0.f, 0.f, 1.f}, {1000.f, 0.f, 0.f, 1.f}, {1000.f, 500.f, 1000.f, 1.f}, {0.f, 500.f, 1000.f, 1.f}}}, {0.f, .894427191f, -.447213595f, 0.f}, true};
		polygon lintel{{{{-500.f, 100.f, 8500.f, 1.f}, {-500.f, 3000.f, 8500.f, 1.f}, {500.f, 3000.f, 8500.f, 1.f}, {500.f, 100.f, 8500.f, 1.f}}}, {0.f, 0.f, -1.f, 0.f}, true};
		polygon post{{{{2000.f, -5000.f, 0.f, 1.f}, {2000.f, 5000.f, 0.f, 1.f}, {2000.f, 5000.f, 0.f, 1.f}, {2000.f, -5000.f, 0.f, 1.f}}}, {-1.f, 0.f, 0.f, 0.f}, true};
		polygon broken = noisy.back();
		broken.vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
		polygon retired = noisy.front();
		retired.active = false;
		retired.vertices[0][0] = std::numeric_limits<float>::infinity();
		noisy.insert(noisy.end(), {ramp, lintel, post, broken, retired});
		const auto tolerant = survey(noisy);
		check(tolerant.path == wall_path::general && tolerant.counts.polygons == 72 && tolerant.counts.sloped == 1 && tolerant.counts.off_floor == 1 &&
				  tolerant.counts.shape == 1 && tolerant.counts.malformed == 1 && tolerant.counts.inactive == 1 && tolerant.counts.walls == 36,
			"sloped, off-floor, misshapen, malformed and inactive polygons must be skipped and counted one by one");
		for (const auto target : targets)
		{
			const auto plain = place(general.arena, target, 300.f, 2000.f), skipped = place(tolerant.arena, target, 300.f, 2000.f);
			check(plain && skipped && plain->actors[0].x == skipped->actors[0].x && plain->actors[0].z == skipped->actors[0].z &&
					  plain->actors[1].x == skipped->actors[1].x && plain->actors[1].z == skipped->actors[1].z,
				"skipped polygons must not move a placement");
		}
		auto floorless = std::vector<polygon>(wall_model::stage17.begin(), wall_model::stage17.end());
		std::erase_if(floorless, [](const polygon& record)
			{
				return std::abs(record.normal[1]) > .999f;
			});
		const auto unfloored = survey(floorless);
		check(unfloored.path == wall_path::none && std::string_view(unfloored.refusal) == "no_floor" && unfloored.counts.walls == 36,
			"walls without any floor are not stage collision and must offer nothing");
		const auto garbage = survey(std::vector<polygon>(16));
		check(garbage.path == wall_path::none && std::string_view(garbage.refusal) == "malformed_collection" && garbage.counts.malformed == 16,
			"a collection that is malformed throughout must be refused rather than read for walls");
		const auto oversized = survey(std::vector<polygon>(8193));
		check(oversized.path == wall_path::none && std::string_view(oversized.refusal) == "polygon_budget",
			"collections beyond the native polygon budget must be refused by the general reading too");
	}

	void gapped_ring_placements(const auto& ring)
	{
		check(classify(ring({})).supported && classify(ring({})).walls.size() == 24, "the closed synthetic ring must satisfy the strict reading");
		const auto gapped = survey(ring({11}));
		check(!classify(ring({11})).supported && gapped.path == wall_path::general && gapped.arena.walls.size() == 23,
			"a ring with one missing panel must fall to the general reading");
		const auto swept = intersect(gapped.arena, {-1, 0});
		check(swept && swept->angle == 10.f && swept->position.z < 0,
			"a ray through the missing panel must sweep to the nearest angle that reaches a wall (5 degrees still passes the gap)");
		const auto gap_pair = place(gapped.arena, position::p1_back, 300.f, 2000.f);
		check(gap_pair && contains(gapped.arena, gap_pair->actors[0], 300.f) && contains(gapped.arena, gap_pair->actors[1], 300.f) &&
				  wall_pair_oriented(position::p1_back, *gap_pair, *swept),
			"the swept wall must give a valid Left Wall pair beside the gap");
		check(pair_clear(gapped.arena, gap_pair->actors) && place(gapped.arena, position::p1_back, 750.f, 2000.f),
			"the Left Wall pair beside the gap must stay walled in at the menu clearance too");
		// Seen from the origin through the missing panel, 1466 from the nearest wall end and crossing nothing, yet outside.
		const point through_gap{-9800.f, 0.f};
		check(nearest_wall(gapped.arena, through_gap) >= 750.f && contains(gapped.arena, through_gap, 750.f) && !enclosed(gapped.arena, through_gap) &&
				  enclosed(gapped.arena, gap_pair->actors[0]) && enclosed(gapped.arena, gap_pair->actors[1]),
			"a point the origin sees through an opening must not count as walled in");
		const auto opening = survey(ring({9, 10, 11, 12, 13}));
		check(opening.path == wall_path::general && !intersect(opening.arena, {-1, 0}) && !place(opening.arena, position::p1_back, 300.f, 2000.f) &&
				  place(opening.arena, position::p2_back, 300.f, 2000.f) && place(opening.arena, position::foreground, 300.f, 2000.f) &&
				  place(opening.arena, position::background, 300.f, 2000.f),
			"an opening wider than the sweep must make that one position unavailable and leave the others");
	}

	void concave_room_placements(const polygon& ground, const auto& targets)
	{
		// L-shaped room with a narrow arm: wide arm [-8000, 400] x [-8000, 1000], narrow arm [-400, 400] x [1000, 8000].
		const std::vector<polygon> narrow_arm{ground,
			synthetic_wall_panel({-8000, -8000}, {400, -8000}, {0, 1}), synthetic_wall_panel({400, -8000}, {400, 8000}, {-1, 0}), synthetic_wall_panel({400, 8000}, {-400, 8000}, {0, -1}),
			synthetic_wall_panel({-400, 8000}, {-400, 1000}, {1, 0}), synthetic_wall_panel({-400, 1000}, {-8000, 1000}, {0, -1}), synthetic_wall_panel({-8000, 1000}, {-8000, -8000}, {1, 0})};
		const auto arm = survey(narrow_arm);
		check(!classify(narrow_arm).supported && arm.path == wall_path::general && arm.arena.walls.size() == 6,
			"the concave L-shaped room must take the general reading");
		// Foreground's first pair puts P2 here: 600 from the arm's wall, but behind it as seen from the origin.
		const point behind_wall{1000.f, 8000.f - 300.05f};
		check(nearest_wall(arm.arena, behind_wall) >= 300.f && !contains(arm.arena, behind_wall, 300.f),
			"a fighter that clears every wall but stands behind one, seen from the origin, must be rejected");
		check(!place(arm.arena, position::foreground, 300.f, 2000.f) && !place(arm.arena, position::background, 300.f, 2000.f),
			"side-wall pairs whose path from the origin crosses a wall must stay unavailable");
		check(place(arm.arena, position::p1_back, 300.f, 2000.f) && place(arm.arena, position::p2_back, 300.f, 2000.f),
			"back-wall pairs in plain view of the origin must stay available");
		// L-shaped room with the second arm off to the side: its inner wall faces away from the origin and becomes an obstacle.
		const std::vector<polygon> side_arm{ground,
			synthetic_wall_panel({-8000, -8000}, {8000, -8000}, {0, 1}), synthetic_wall_panel({8000, -8000}, {8000, 1000}, {-1, 0}), synthetic_wall_panel({8000, 1000}, {-3000, 1000}, {0, -1}),
			synthetic_wall_panel({-3000, 1000}, {-3000, 8000}, {-1, 0}), synthetic_wall_panel({-3000, 8000}, {-8000, 8000}, {0, -1}), synthetic_wall_panel({-8000, 8000}, {-8000, -8000}, {1, 0})};
		const auto side = survey(side_arm);
		check(side.path == wall_path::general && side.arena.walls.size() == 5 && side.arena.obstacles.size() == 1 && side.counts.facing_away == 1,
			"a wall facing away from the origin must be kept as an obstacle, never offered as a restart wall");
		check(nearest_wall(side.arena, {-5500, 6000}) >= 750.f && !contains(side.arena, {-5500, 6000}, 750.f) &&
				  contains(side.arena, {-5500, -2000}, 750.f),
			"a point inside the room but hidden round its corner must be rejected");
		for (const auto target : targets)
		{
			const auto placed = place(side.arena, target, 750.f, 2000.f);
			check(placed && contains(side.arena, placed->actors[0], 750.f) && contains(side.arena, placed->actors[1], 750.f),
				"every wall in plain view of the origin must offer its position");
		}
	}

	void open_room_and_partition(const polygon& ground)
	{
		// Room open on the left: the sweep's 25-degree ray reaches the background wall, at right angles to Left Wall. That
		// wall must not stand in for the missing one, or Left Wall would put P1's back to the background wall.
		const std::vector<polygon> open_left{ground,
			synthetic_wall_panel({-9000, -4000}, {9000, -4000}, {0, 1}), synthetic_wall_panel({9000, -4000}, {9000, 4000}, {-1, 0}), synthetic_wall_panel({9000, 4000}, {-9000, 4000}, {0, -1})};
		const auto open_room = survey(open_left);
		const float skew = 25.f * std::numbers::pi_v<float> / 180.f;
		const auto sideways = cast(open_room.arena.walls, {-std::cos(skew), -std::sin(skew)});
		check(open_room.path == wall_path::general && sideways && sideways->inward.z == 1.f && !intersect(open_room.arena, {-1, 0}) &&
				  !place(open_room.arena, position::p1_back, 300.f, 2000.f) && place(open_room.arena, position::p2_back, 300.f, 2000.f) &&
				  place(open_room.arena, position::foreground, 300.f, 2000.f) && place(open_room.arena, position::background, 300.f, 2000.f),
			"a wall at right angles to the requested direction must never stand in for a missing one");

		// A partition between the fighters crosses neither path from the origin, yet would face the pair at each other through it.
		const std::vector<polygon> fenced_room{ground,
			synthetic_wall_panel({-9000, -9000}, {9000, -9000}, {0, 1}), synthetic_wall_panel({9000, -9000}, {9000, 9000}, {-1, 0}), synthetic_wall_panel({9000, 9000}, {-9000, 9000}, {0, -1}),
			synthetic_wall_panel({-9000, 9000}, {-9000, -9000}, {1, 0}), synthetic_wall_panel({300, 7000}, {300, 8800}, {-1, 0}), synthetic_wall_panel({300, 8800}, {300, 7000}, {1, 0})};
		const auto fenced = survey(fenced_room);
		const std::array<point, 2> astride{{{-1000.f, 8699.95f}, {1000.f, 8699.95f}}};
		check(fenced.path == wall_path::general && contains(fenced.arena, astride[0], 300.f) && contains(fenced.arena, astride[1], 300.f) &&
				  !pair_clear(fenced.arena, astride) && !place(fenced.arena, position::foreground, 300.f, 2000.f) &&
				  place(fenced.arena, position::p1_back, 300.f, 2000.f) && place(fenced.arena, position::p2_back, 300.f, 2000.f) &&
				  place(fenced.arena, position::background, 300.f, 2000.f),
			"a pair must never be placed either side of a partition");
	}

	void notched_room_enclosure(const polygon& ground)
	{
		// Room whose top has an opening beside a slanted panel: Foreground's ray passes the opening, the sweep meets the
		// slanted panel at 5 degrees, and a pair along that panel would put P2 175 units past the opening, outside the room.
		const float slant = std::hypot(2000.f, 2500.f);
		const std::vector<polygon> notched_room{ground,
			synthetic_wall_panel({-9000, -9000}, {9000, -9000}, {0, 1}), synthetic_wall_panel({9000, -9000}, {9000, 9000}, {-1, 0}), synthetic_wall_panel({9000, 9000}, {2000, 9000}, {0, -1}),
			synthetic_wall_panel({-500, 9000}, {-3000, 7000}, {2000.f / slant, -2500.f / slant}), synthetic_wall_panel({-3000, 7000}, {-9000, 7000}, {0, -1}),
			synthetic_wall_panel({-9000, 7000}, {-9000, -9000}, {1, 0})};
		const auto notched = survey(notched_room);
		const auto slanted = intersect(notched.arena, {0, 1});
		const point beyond{200.f, 9150.f};
		check(notched.path == wall_path::general && notched.arena.walls.size() == 6 && slanted && slanted->angle == 5.f &&
				  nearest_wall(notched.arena, beyond) >= 300.f && contains(notched.arena, beyond, 300.f) && !enclosed(notched.arena, beyond) &&
				  !place(notched.arena, position::foreground, 300.f, 2000.f) && !place(notched.arena, position::foreground, 750.f, 2000.f),
			"a fighter the origin sees through an opening, past where its wall would be, must not be placed there");
		for (const auto target : {position::p1_back, position::p2_back, position::background})
			check(place(notched.arena, target, 300.f, 2000.f) && place(notched.arena, target, 750.f, 2000.f),
				"positions walled in on their own side must stay available beside an opening");
	}

	void floor_level_obstacles(const auto& ring)
	{
		// Polygons that are never a restart wall still block where they reach floor level: a slab leaning back (normal.y .28,
		// too steep to stand on) across the Left Wall ray and a trapezoid partition across the Right Wall ray, inside an
		// otherwise closed ring.
		const polygon slab{{{{-4720.f, -960.f, -2000.f, 1.f}, {-4720.f, -960.f, 2000.f, 1.f}, {-5840.f, 2880.f, 2000.f, 1.f}, {-5840.f, 2880.f, -2000.f, 1.f}}}, {.96f, .28f, 0.f, 0.f}, true};
		const polygon partition{{{{5000.f, -1000.f, -2000.f, 1.f}, {5000.f, -1000.f, 2000.f, 1.f}, {5000.f, 3000.f, 1000.f, 1.f}, {5000.f, 3000.f, -1000.f, 1.f}}}, {-1.f, 0.f, 0.f, 0.f}, true};
		const polygon raised{{{{-500.f, 100.f, 0.f, 1.f}, {-500.f, 3000.f, 0.f, 1.f}, {500.f, 3000.f, 0.f, 1.f}, {500.f, 100.f, 0.f, 1.f}}}, {0.f, 0.f, -1.f, 0.f}, true};
		const auto slab_cut = floor_section(slab), partition_cut = floor_section(partition);
		check(slab_cut && std::abs((*slab_cut)[0].x + 5000.f) < .01f && std::abs((*slab_cut)[1].x + 5000.f) < .01f &&
				  std::abs(distance((*slab_cut)[0], (*slab_cut)[1]) - 4000.f) < .01f &&
				  partition_cut && std::abs(distance((*partition_cut)[0], (*partition_cut)[1]) - 3500.f) < .01f && !floor_section(raised),
			"a polygon's floor-level cut must span exactly where it meets y = 0, and a raised one has none");
		auto blocked_ring = ring({});
		blocked_ring.push_back(slab);
		blocked_ring.push_back(partition);
		const auto blocked = survey(blocked_ring);
		check(blocked.path == wall_path::general && blocked.counts.sloped == 1 && blocked.counts.shape == 1 && blocked.arena.walls.size() == 24 &&
				  blocked.arena.obstacles.size() == 2 && !place(blocked.arena, position::p1_back, 300.f, 2000.f) &&
				  !place(blocked.arena, position::p2_back, 300.f, 2000.f) && place(blocked.arena, position::foreground, 300.f, 2000.f) &&
				  place(blocked.arena, position::background, 300.f, 2000.f),
			"a steep sloped or misshapen polygon reaching floor level must block a pair like a wall, without becoming one");
	}

} // namespace

PBD_TEST(PbdPractice, WallChecks)
{
	using namespace pbd::practice::walls;
	check(active_polygon(0x01000000) && active_polygon(0x81000000) && !active_polygon(0x00000001) && !active_polygon(0),
		"native active flags must come from byte5C rather than the low byte of its big-endian word");
	const auto arena = classify(wall_model::stage30);
	check(arena.supported && arena.walls.size() == 21, "actual floor and segmented wall records must retain the closed boundary");
	strict_wall_placements(arena);
	const auto floor = classify(wall_model::stage31);
	check(floor.supported && floor.walls.empty() && !intersect(floor, {-1, 0}) && !contains(floor, {0, 0}, 0),
		"actual floor-only data must not offer a wall position");
	check(!place(floor, position::p1_back, 300.f, 2000.f), "floor-only collections must never construct a wall destination");
	check(!classify(wall_model::stage17).supported,
		"the captured gapped and slightly concave stage17 boundary must remain unavailable under the closed-convex guard");
	malformed_wall_boundaries();
	// The geometry Practice uses: the strict reading whenever it qualifies, otherwise the general reading.
	const std::array targets{position::p1_back, position::p2_back, position::foreground, position::background};
	const auto strict = survey(wall_model::stage30);
	strict_survey_preserves_placements(arena, strict, targets);
	short_convex_wall_clearance();
	const auto bare = survey(wall_model::stage31);
	check(bare.path == wall_path::none && !bare.arena.supported && std::string_view(bare.refusal) == "no_walls" &&
			  bare.counts.floors == 16 && !bare.counts.walls,
		"actual floor-only data must offer no wall under either reading");
	for (const auto target : targets)
		check(!place(bare.arena, target, 300.f, 2000.f), "a floor-only stage must never construct a wall destination");

	// stage17: gapped and slightly concave, refused by the strict reading above; the general reading serves it.
	const auto general = survey(wall_model::stage17);
	check(general.path == wall_path::general && general.arena.general && general.arena.walls.size() == 36 && general.arena.obstacles.empty() &&
			  general.counts.polygons == 67 && general.counts.walls == 36 && general.counts.floors == 31 && !general.counts.inactive &&
			  !general.counts.malformed && !general.counts.sloped && !general.counts.shape && !general.counts.off_floor && !general.counts.facing_away,
		"the captured stage17 collection must take the general reading with every wall and floor accounted for");

	general_stage_placements(general, targets);
	restart_pair_orientation(strict, general, targets);
	general_survey_refusals(general, targets);
	// Synthetic stages: vertical panels five thousand units each way of the floor at y = 0, on one floor tile.
	const polygon ground{{{{-9500.f, 0.f, -9500.f, 1.f}, {9500.f, 0.f, -9500.f, 1.f}, {9500.f, 0.f, 9500.f, 1.f}, {-9500.f, 0.f, 9500.f, 1.f}}}, {0.f, 1.f, 0.f, 0.f}, true};

	// A 24-sided ring of radius 9000 with corners at 7.5 + 15k degrees, so the -X ray meets the middle of panel 11.

	const auto ring = [&](std::initializer_list<unsigned> missing)
	{
		return synthetic_wall_ring(ground, missing);
	};
	gapped_ring_placements(ring);
	concave_room_placements(ground, targets);
	open_room_and_partition(ground);
	notched_room_enclosure(ground);
	floor_level_obstacles(ring);
}
