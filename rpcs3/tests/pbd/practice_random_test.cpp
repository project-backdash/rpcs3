#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_random.h"
#include "practice_wall_model.h"

namespace
{
	using pbd::test::check;

	void sample_arena(const pbd::practice::walls::boundary& arena)
	{
		namespace geometry = pbd::practice::walls;
		unsigned near_wall = 0, interior = 0;
		std::optional<geometry::pair_placement> previous;
		unsigned changed = 0;
		for (std::uint32_t seed = 0; seed < 4096; ++seed)
		{
			const auto pair = pbd::practice::random_position(arena, seed);
			check(pair.has_value(), "a roomy verified arena produces a position");
			const auto repeated = pbd::practice::random_position(arena, seed);
			check(repeated && repeated->actors[0].x == pair->actors[0].x &&
					  repeated->actors[0].z == pair->actors[0].z && repeated->actors[1].x == pair->actors[1].x &&
					  repeated->actors[1].z == pair->actors[1].z,
				"retries keep the same requested position");
			for (const auto actor : pair->actors)
				check(geometry::contains(arena, actor, 750.f), "each fighter has the conservative wall clearance");
			check(geometry::pair_clear(arena, pair->actors), "no wall or obstacle separates the fighters");
			check(std::abs(geometry::distance(pair->actors[0], pair->actors[1]) - 2000.f) < .01f,
				"the fighters keep the native centre separation");
			const float closest = std::min(geometry::nearest_wall(arena, pair->actors[0]),
				geometry::nearest_wall(arena, pair->actors[1]));
			near_wall += closest < 1100.f;
			interior += closest > 1500.f;
			changed += previous && (previous->actors[0].x != pair->actors[0].x || previous->actors[0].z != pair->actors[0].z);
			previous = pair;
		}
		// Seeds are entropy words, so both wall practice and translated interior starts must be reachable.
		check(near_wall > 500 && interior > 500 && changed > 3500, "the sample includes walls and varied interior spots");
	}
} // namespace

PBD_TEST(PbdPracticeRandom, Checks)
{
	namespace geometry = pbd::practice::walls;
	const auto stage30 = geometry::survey(wall_model::stage30);
	check(stage30.path != geometry::wall_path::none, "the captured stage geometry is supported");
	sample_arena(stage30.arena);
	const auto stage31 = geometry::survey(wall_model::stage31);
	check(!pbd::practice::random_position(stage31.arena, 1),
		"the captured floor-only stage needs a separate floor reader");
	const auto stage17 = geometry::survey(wall_model::stage17);
	check(stage17.path != geometry::wall_path::none, "the captured general arena is supported");
	sample_arena(stage17.arena);
	geometry::boundary unavailable;
	check(!pbd::practice::random_position(unavailable, 1), "unsupported geometry is refused");
	unavailable.supported = true;
	check(!pbd::practice::random_position(unavailable, 1), "an unbounded floor needs a separate floor reader");
	check(!pbd::practice::random_position(stage30.arena, 1, {0.f, 2000.f}), "zero clearance is refused");
	check(!pbd::practice::random_position(stage30.arena, 1, {750.f, -1.f}), "negative separation is refused");
	check(!pbd::practice::random_position(stage30.arena, 1, {750.f, 100000.f}),
		"a pair that cannot fit in the arena is refused");
	check(!pbd::practice::random_position(stage30.arena, 1,
			  {std::numeric_limits<float>::quiet_NaN(), 2000.f}),
		"nonfinite options are refused");
}
