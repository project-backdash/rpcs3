#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_floor.h"
#include "Emu/PBD/pbd_practice_random.h"
#include "practice_wall_model.h"

namespace
{
	using pbd::test::check;
} // namespace

PBD_TEST(PbdPracticeFloor, Checks)
{
	namespace geometry = pbd::practice::walls;
	const auto arena = pbd::practice::floor_boundary(wall_model::stage31);
	check(arena.has_value(), "captured flat floor union admits a convex outline");
	for (std::uint32_t seed = 0; seed < 4096; ++seed)
	{
		const auto pair = pbd::practice::random_position(*arena, seed);
		check(pair.has_value(), "captured floor has a valid random pair");
		for (const auto at : pair->actors)
		{
			check(geometry::contains(*arena, at, 750.f), "pair remains inside the floor clearance");
			check(at.x >= -5250.f && at.x <= 5250.f && at.z >= -5250.f && at.z <= 5250.f,
				"pair stays inside the independently captured six-thousand unit floor extent");
		}
		check(std::abs(geometry::distance(pair->actors[0], pair->actors[1]) - 2000.f) < .01f,
			"pair preserves restart separation");
	}
	check(!pbd::practice::floor_boundary(wall_model::stage30), "physical-wall collection uses its own reading");
	check(!pbd::practice::floor_boundary({}), "empty floor is refused");
	auto incomplete = std::vector(wall_model::stage31.begin(), wall_model::stage31.end());
	incomplete.erase(incomplete.begin() + 1);
	check(!pbd::practice::floor_boundary(incomplete), "missing internal cell is refused");
	auto duplicate = std::vector(wall_model::stage31.begin(), wall_model::stage31.end());
	duplicate.push_back(duplicate.front());
	check(!pbd::practice::floor_boundary(duplicate), "duplicate cell is refused");
	auto malformed = std::vector(wall_model::stage31.begin(), wall_model::stage31.end());
	malformed[0].vertices[0][1] = 10.f;
	check(!pbd::practice::floor_boundary(malformed), "different floor elevation is refused");
}
